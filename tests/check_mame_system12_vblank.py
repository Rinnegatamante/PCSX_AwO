#!/usr/bin/env python3
"""Measure H8 port-B transitions against MAME's System 12 screen vblank API.

The fixture is injected into a temporary in-memory copy of the H8 ROM after
MAME has loaded soulclbrac.  The ROM archives and repository artifacts are
never modified.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "tests/mame_system12_vblank_oracle.lua"


def run_mame(mame: str, rompath: Path, script: Path, work: Path) -> tuple[str, int, list[str], bool]:
    command = [mame, "soulclbrac", "-noreadconfig", "-rompath", str(rompath),
               "-skip_gameinfo", "-video", "none", "-sound", "none", "-nothrottle",
               "-seconds_to_run", "4", "-nonvram_save",
               "-nvram_directory", str(work / "nvram"),
               "-cfg_directory", str(work / "cfg"), "-inipath", str(work),
               "-autoboot_script", str(script), "-autoboot_delay", "0"]
    proc = subprocess.Popen(command, cwd=work, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        output, _ = proc.communicate(timeout=45)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.send_signal(signal.SIGTERM)
        try:
            output, _ = proc.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            output, _ = proc.communicate()
        return output, proc.returncode if proc.returncode is not None else -999, command, timed_out
    return output, proc.returncode, command, False


def absolute_attos(parts: dict[str, int]) -> int:
    return parts["seconds"] * 10**18 + parts["attoseconds"]


def split_attos(value: int) -> dict[str, int]:
    seconds, attoseconds = divmod(value, 10**18)
    return {"seconds": seconds, "attoseconds": attoseconds}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame", default=os.environ.get("MAME", "/opt/homebrew/bin/mame"))
    parser.add_argument("--rompath", type=Path, default=ROOT / "roms")
    parser.add_argument("--output", type=Path,
                        help="write structured JSON evidence here (stdout log goes beside it)")
    parser.add_argument("--keep", action="store_true", help="retain private transient files")
    args = parser.parse_args()
    if not (args.rompath / "soulclbr.zip").is_file():
        parser.error(f"requires parent archive soulclbr.zip in {args.rompath}")

    work = Path(tempfile.mkdtemp(prefix="mame-s12-vblank-"))
    try:
        rom_hash_before = hashlib.sha256((args.rompath / "soulclbr.zip").read_bytes()).hexdigest()
        fixture_hash_before = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        version = subprocess.run([args.mame, "-version"], text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, timeout=10, check=False)
        print("MAME " + version.stdout.strip())
        script = work / "system12-vblank.lua"
        script.write_text(TEMPLATE.read_text())
        output, returncode, command, timed_out = run_mame(args.mame, args.rompath.resolve(), script, work)
        if args.output:
            output_path = args.output.resolve()
            output_path.parent.mkdir(parents=True, exist_ok=True)
            log_path = output_path.with_suffix(output_path.suffix + ".log")
            log_path.write_text("$ " + " ".join(command) + "\n" + output
                                + f"\nexit_code={returncode}\ntimeout={timed_out}\n")
        else:
            log_path = None

        transitions = []
        for match in re.finditer(
            r"S12_PORTB_TRANSITION n=(\d+) before=([0-9a-fA-F]+) before_time=(\d+):(\d+) before_pc=([^\s]+) after=([0-9a-fA-F]+) after_time=(\d+):(\d+) after_pc=([^\s]+)",
            output):
            n, before, bsec, battos, bpc, after, asec, aattos, apc = match.groups()
            transitions.append({
                "n": int(n), "before": int(before, 16),
                "before_time": {"seconds": int(bsec), "attoseconds": int(battos)},
                "before_pc": bpc, "after": int(after, 16),
                "after_time": {"seconds": int(asec), "attoseconds": int(aattos)},
                "after_pc": apc,
            })
        first_read_match = re.search(
            r"S12_PORTB_FIRSTREAD value=([0-9a-fA-F]+) time=(\d+):(\d+) pc=([^\s]+)", output)
        first_read = None
        if first_read_match:
            value, sec, attos, pc = first_read_match.groups()
            first_read = {"value": int(value, 16),
                          "time": {"seconds": int(sec), "attoseconds": int(attos)},
                          "pc": pc}
        edges = []
        for kind, sec, attos in re.findall(
                r"S12_VBLANK_EDGE kind=(start|end) time=(\d+):(\d+)", output):
            edges.append({"kind": kind, "time": {"seconds": int(sec), "attoseconds": int(attos)}})
        edges.sort(key=lambda row: absolute_attos(row["time"]))
        resets = []
        for n, sec, attos, pc in re.findall(
                r"S12_H8_RESET n=(\d+) time=(\d+):(\d+) pc=([^\s]+)", output):
            resets.append({"n": int(n), "time": {"seconds": int(sec), "attoseconds": int(attos)}, "pc": pc})
        reset_time = absolute_attos(resets[0]["time"]) if resets else 0
        if first_read:
            first_read["relative_to_reset"] = split_attos(absolute_attos(first_read["time"]) - reset_time)
        for row in transitions:
            row["before_relative_to_reset"] = split_attos(absolute_attos(row["before_time"]) - reset_time)
            row["after_relative_to_reset"] = split_attos(absolute_attos(row["after_time"]) - reset_time)
            gap = absolute_attos(row["after_time"]) - absolute_attos(row["before_time"])
            row["poll_gap_h8_cycles_ceil"] = (gap * 16934400 + 10**18 - 1) // 10**18
        for row in edges:
            row["relative_to_reset"] = split_attos(absolute_attos(row["time"]) - reset_time)
        rom_hash_after = hashlib.sha256((args.rompath / "soulclbr.zip").read_bytes()).hexdigest()
        fixture_hash_after = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        evidence = {
            "mame_version": version.stdout.strip(),
            "command": command,
            "mame_returncode": returncode,
            "timed_out": timed_out,
            "source_revision_pin": "deb2f54fe7300e7a192690318beb860159bfd52a",
            "installed_binary_source_match": "unknown; localMAME is a prebuilt 0.287 binary",
            "sha256": {"parent_rom_before": rom_hash_before, "parent_rom_after": rom_hash_after,
                       "fixture_before": fixture_hash_before, "fixture_after": fixture_hash_after},
            "clock": {"h8_hz": 16934400, "timestamp_source": "MAME machine.time() at H8 program-space read tap"},
            "conversion": {"portb_address_24bit": "0xffffd6", "mem_mask_rule": "big-endian 16-bit: 0xff00 high byte, otherwise low byte", "attoseconds_per_second": 1000000000000000000},
            "firmware_words": ["0x6a08", "0xffd6", "0x40fa"],
            "first_read": first_read,
            "reset_events": resets,
            "transitions": transitions,
            "screen_edges": edges,
            "screen_edge_source": "screen_device.time_until_vblank_start/end scheduled times sampled from Lua frame callback",
            "stdout": output,
            "stdout_log": str(log_path) if log_path else None,
        }
        if args.output:
            args.output.resolve().write_text(json.dumps(evidence, indent=2) + "\n")
            print(f"evidence JSON: {args.output.resolve()}")
            print(f"MAME stdout log: {log_path}")
        print(f"MAME return code: {returncode}; H8 transitions={len(transitions)}; screen edges={len(edges)}")
        for row in transitions:
            print(f"H8 edge {row['n']}: 0x{row['before']:02x}->{row['after']:02x}, "
                  f"{row['before_time']} PC={row['before_pc']} -> {row['after_time']} PC={row['after_pc']}")
        for row in edges[:4]:
            print(f"screen {row['kind']}: {row['time']}")
        if returncode:
            raise RuntimeError(f"MAME exited {returncode}:\n" + output[-5000:])
        if timed_out:
            raise RuntimeError("MAME exceeded its watchdog even though termination returned success")
        if "LUA ERROR" in output:
            raise RuntimeError("MAME reported a Lua error:\n" + output[-5000:])
        if len(transitions) < 4:
            raise RuntimeError("expected at least four H8 port-B transitions, got "
                               f"{len(transitions)}:\n" + output[-5000:])
        if not any(row["kind"] == "start" for row in edges) or not any(row["kind"] == "end" for row in edges):
            raise RuntimeError("screen vblank API did not produce both edges:\n" + output[-5000:])
        bits = [row["after"] & 0x80 for row in transitions[:4]]
        if bits != [0x80, 0, 0x80, 0]:
            raise RuntimeError(f"unexpected transition levels {bits}:\n" + output[-5000:])
        if rom_hash_before != rom_hash_after or fixture_hash_before != fixture_hash_after:
            raise RuntimeError("parent ROM or fixture changed during MAME capture")
        if not first_read or not resets:
            raise RuntimeError("H8 first-read/reset epoch evidence is missing:\n" + output[-5000:])
        first_read_delta = absolute_attos(first_read["relative_to_reset"])
        attos_per_cycle = 10**18 // 16934400
        if first_read_delta != 12 * attos_per_cycle:
            raise RuntimeError(f"first H8 read time is {first_read_delta}, expected exactly 12 cycles")
        for row in transitions:
            before = absolute_attos(row["before_relative_to_reset"])
            after = absolute_attos(row["after_relative_to_reset"])
            if after - before != 10 * attos_per_cycle:
                raise RuntimeError(f"polling loop does not advance by exactly ten cycles: {row}")
            for timestamp in (before, after):
                cycle, remainder = divmod(timestamp, attos_per_cycle)
                if remainder or (cycle - 12) % 10:
                    raise RuntimeError(f"H8 tap timestamp is outside the polling phase: {row}")
            kind = "start" if row["after"] & 0x80 else "end"
            matching_edges = [edge for edge in edges if edge["kind"] == kind
                              and before < absolute_attos(edge["relative_to_reset"]) <= after]
            if len(matching_edges) != 1:
                raise RuntimeError(f"screen edge is not bracketed by the two H8 polls: {row}")
        print("MAME System 12 vblank oracle passed")
        return 0
    finally:
        if args.keep:
            print(f"private transient files retained at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
