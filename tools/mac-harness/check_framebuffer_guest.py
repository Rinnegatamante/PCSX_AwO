#!/usr/bin/env python3
"""Execute the Vita framebuffer/polygon guest on a native Mac harness.

The GP0 stream is unchanged. Only final RAM-to-GPR loads are added so the
existing cycle snapshot interface can expose the eighteen guest readbacks.
This validates the probe, not authentic-game compatibility.
"""
import argparse
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
REGISTERS = tuple(range(1, 16)) + (17, 18, 19)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_guest(directory, texture_window=False):
    path = ROOT / "tools/vita-harness/check_system12_framebuffer_vita3k.py"
    spec = importlib.util.spec_from_file_location("native_framebuffer_guest", path)
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    directory.mkdir()
    fixture.make_regions(directory, texture_window=texture_window)
    code, commands = fixture.make_guest_code(texture_window=texture_window)
    drc = fixture.module("native_framebuffer_drc", ROOT / "tests/check_system12_drc.py")
    # Remove only the terminal infinite jump; GPU operations and RAM results
    # are identical to the native Vita probe.
    code = code[:-2]
    registers = (tuple(range(1, 16)) + tuple(range(17, 26))
                 if texture_window else REGISTERS)
    marker_register = 30 if texture_window else 20
    for index, register in enumerate(registers, 1):
        code.extend((drc.lw(register, index * 4, 16), drc.nop()))
    code.extend((drc.lw(marker_register, 0, 16), drc.nop()))
    loop = 0xbfc01000 + len(code) * 4
    code.extend((drc.j(loop), drc.nop()))
    main = bytearray((directory / "main.bin").read_bytes())
    struct.pack_into(f"<{len(code)}I", main, 0x1000, *code)
    (directory / "main.bin").write_bytes(main)
    return dict(registers=registers,
                expected=fixture.EXPECTED_PIXELS + fixture.EXPECTED_POLYGON_PIXELS +
                         (fixture.EXPECTED_TEXTURE_WINDOW_PIXELS if texture_window else ()),
                marker_register=marker_register, marker=fixture.RESULT_MAGIC,
                main_sha256=sha256(directory / "main.bin"),
                gp0_sha256=hashlib.sha256(struct.pack(f"<{len(commands)}I", *commands)).hexdigest(),
                fixture_source_sha256=sha256(path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--texture-window", action="store_true",
                        help="also compare E2 mask/offset sampling and E2 reset")
    args = parser.parse_args()
    if not 0 < args.timeout <= 600:
        parser.error("timeout must be in (0, 600]")
    harness = args.harness.resolve(strict=True)
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("output directory must be empty; existing evidence is retained")
    output.mkdir(parents=True, exist_ok=True)
    oracle = make_guest(output / "regions", texture_window=args.texture_window)
    (output / "oracle.json").write_text(json.dumps(oracle, indent=2) + "\n")
    command = [str(harness), "--system12", str(output / "regions"), "--frames", "2",
               "--cycle-snapshots", str(output / "registers.csv"),
               "--snapshot-interval", "100000", "--output", str(output / "frame.ppm")]
    evidence = dict(passed=False, harness=str(harness), harness_sha256=sha256(harness),
                    command=command, oracle=oracle)
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=args.timeout, check=False)
        (output / "harness.log").write_bytes(result.stdout)
        evidence["exit_code"] = result.returncode
        if result.returncode:
            raise RuntimeError(f"harness exited with {result.returncode}")
        with (output / "registers.csv").open(newline="") as file:
            rows = list(csv.DictReader(file))
        if not rows:
            raise RuntimeError("harness produced no register snapshots")
        final = rows[-1]
        values = [int(final[f"r{register}"], 16) for register in oracle["registers"]]
        marker = int(final[f"r{oracle['marker_register']}"], 16)
        evidence.update(snapshot_count=len(rows), colors_rgb555=values, marker=marker,
                        frame_sha256=sha256(output / "frame.ppm"),
                        host_summary=[line for line in result.stdout.decode(errors="replace").splitlines()
                                      if line.startswith("host=")])
        if values != list(oracle["expected"]) or marker != oracle["marker"]:
            raise RuntimeError("guest marker or framebuffer/polygon pixels differ from oracle")
        evidence["passed"] = True
    except subprocess.TimeoutExpired as error:
        (output / "harness.log").write_bytes(error.stdout or b"")
        evidence["error"] = "owned native harness timed out and was killed"
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        evidence["error"] = str(error)
    (output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"Mac framebuffer/polygon guest: {'PASS' if evidence['passed'] else 'FAIL'}; {output}")
    return 0 if evidence["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
