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


def make_guest(directory, texture_window=False, sprite_uv_wrap=False, vram_x_wrap=False,
               coord_alias=False, coord_span_cull=False):
    path = ROOT / "tools/vita-harness/check_system12_framebuffer_vita3k.py"
    spec = importlib.util.spec_from_file_location("native_framebuffer_guest", path)
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    directory.mkdir()
    fixture.make_regions(directory, texture_window=texture_window,
                         sprite_uv_wrap=sprite_uv_wrap, vram_x_wrap=vram_x_wrap,
                         coord_alias=coord_alias, coord_span_cull=coord_span_cull)
    code, commands = fixture.make_guest_code(texture_window=texture_window,
                                             sprite_uv_wrap=sprite_uv_wrap,
                                             vram_x_wrap=vram_x_wrap,
                                             coord_alias=coord_alias,
                                             coord_span_cull=coord_span_cull)
    drc = fixture.module("native_framebuffer_drc", ROOT / "tests/check_system12_drc.py")
    # Remove only the terminal infinite jump; GPU operations and RAM results
    # are identical to the native Vita probe.
    code = code[:-2]
    if sprite_uv_wrap:
        registers = tuple(range(1, 16))
        marker_register = None
        result_indices = tuple(range(19, 34))
    elif vram_x_wrap:
        registers = tuple(range(1, 10))
        marker_register = None
        result_indices = tuple(range(19, 28))
    elif coord_alias:
        registers = tuple(range(1, 10))
        marker_register = None
        result_indices = tuple(range(19, 28))
    elif coord_span_cull:
        registers = tuple(range(1, 9))
        marker_register = None
        result_indices = tuple(range(19, 27))
    else:
        registers = (tuple(range(1, 16)) + tuple(range(17, 26))
                     if texture_window else REGISTERS)
        marker_register = 30 if texture_window else 20
        result_indices = tuple(range(1, len(registers) + 1))
    for index, register in zip(result_indices, registers):
        code.extend((drc.lw(register, index * 4, 16), drc.nop()))
    if marker_register is not None:
        code.extend((drc.lw(marker_register, 0, 16), drc.nop()))
    loop = 0xbfc01000 + len(code) * 4
    code.extend((drc.j(loop), drc.nop()))
    main = bytearray((directory / "main.bin").read_bytes())
    struct.pack_into(f"<{len(code)}I", main, 0x1000, *code)
    (directory / "main.bin").write_bytes(main)
    return dict(registers=registers,
                expected=(fixture.EXPECTED_SPRITE_UV_WRAP_PIXELS if sprite_uv_wrap else
                        fixture.EXPECTED_VRAM_X_WRAP_PIXELS if vram_x_wrap else
                        fixture.EXPECTED_COORD_ALIAS_PIXELS if coord_alias else
                        fixture.EXPECTED_COORD_SPAN_CULL_PIXELS if coord_span_cull else
                          fixture.EXPECTED_PIXELS + fixture.EXPECTED_POLYGON_PIXELS +
                          (fixture.EXPECTED_TEXTURE_WINDOW_PIXELS if texture_window else ())),
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
    parser.add_argument("--sprite-uv-wrap", action="store_true",
                        help="also compare P4/P8/P16 sprite sampling across UV 255")
    parser.add_argument("--vram-x-wrap", action="store_true",
                        help="compare P8/P16 texture-page and P8 CLUT X wrap at VRAM edge")
    parser.add_argument("--coord-alias", action="store_true",
                        help="compare 11-bit GP0 coordinate aliases for FT3/FT4/sprites")
    parser.add_argument("--coord-span-cull", action="store_true",
                        help="verify oversized GP0 triangle rejection against NEON/MAME")
    args = parser.parse_args()
    if not 0 < args.timeout <= 600:
        parser.error("timeout must be in (0, 600]")
    harness = args.harness.resolve(strict=True)
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("output directory must be empty; existing evidence is retained")
    output.mkdir(parents=True, exist_ok=True)
    if sum((args.texture_window, args.sprite_uv_wrap, args.vram_x_wrap,
            args.coord_alias, args.coord_span_cull)) > 1:
        parser.error("texture-window, sprite-uv-wrap, vram-x-wrap, coord-alias, and coord-span-cull are separate modes")
    oracle = make_guest(output / "regions", texture_window=args.texture_window,
                        sprite_uv_wrap=args.sprite_uv_wrap, vram_x_wrap=args.vram_x_wrap,
                        coord_alias=args.coord_alias,
                        coord_span_cull=args.coord_span_cull)
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
        marker = (int(final[f"r{oracle['marker_register']}"], 16)
                  if oracle["marker_register"] is not None else None)
        evidence.update(snapshot_count=len(rows), colors_rgb555=values, marker=marker,
                        frame_sha256=sha256(output / "frame.ppm"),
                        host_summary=[line for line in result.stdout.decode(errors="replace").splitlines()
                                      if line.startswith("host=")])
        if (values != list(oracle["expected"]) or
                (oracle["marker_register"] is not None and marker != oracle["marker"])):
            raise RuntimeError("guest marker or framebuffer/polygon pixels differ from oracle")
        evidence["passed"] = True
    except subprocess.TimeoutExpired as error:
        (output / "harness.log").write_bytes(error.stdout or b"")
        evidence["error"] = "owned native harness timed out and was killed"
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        evidence["error"] = str(error)
    (output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    label = ("coordinate span culling" if args.coord_span_cull else
             "coordinate aliases" if args.coord_alias else
             "VRAM X wrap" if args.vram_x_wrap else "sprite UV wrap" if args.sprite_uv_wrap else
             "texture-window" if args.texture_window else "framebuffer/polygon")
    print(f"Mac {label} guest: {'PASS' if evidence['passed'] else 'FAIL'}; {output}")
    return 0 if evidence["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
