#!/usr/bin/env python3
"""Compare PSX instruction timing markers exactly against real MAME stores.

The interpreter and DRC runs use identical injected words. Observer-on/off
board reports and PPMs are checked independently; every marker then compares
address, value, width, and reset-relative attoseconds with zero tolerance.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NATIVE_MAIN_PERIOD_AS = 20_000_000_000  # 50 MHz native cycle counter.


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_fixture_helper():
    helper_path = ROOT / "tests/check_system12_vblank.py"
    spec = importlib.util.spec_from_file_location("system12_vblank", helper_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load System 12 fixture helper {helper_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prepare_fixture(directory: Path, main_words: list[int]) -> None:
    helper = load_fixture_helper()
    helper.prepare(directory, False)
    main = directory / "main.bin"
    with main.open("r+b") as stream:
        stream.seek(0)
        stream.write(struct.pack("<" + "I" * len(main_words), *main_words))
    sub = directory / "sub.bin"
    with sub.open("r+b") as stream:
        stream.seek(0)
        stream.write(struct.pack(">I", 0x00000100))
        stream.seek(0x100)
        stream.write(struct.pack(">H", 0x0180))


def run_case(harness: Path, directory: Path, main_words: list[int], drc: bool) -> dict:
    directory.mkdir(parents=True)
    prepare_fixture(directory, main_words)
    common = [str(harness), "--system12", str(directory), "--frames", "1"]
    if drc:
        common.append("--drc")
    observed_command = common + ["--system12-edge-trace", str(directory / "edges.csv"),
                                 "--system12-trace-shared-accesses", "--output",
                                 str(directory / "observed.ppm")]
    control_command = common + ["--output", str(directory / "control.ppm")]
    observed = subprocess.run(observed_command, capture_output=True, text=True, timeout=60)
    control = subprocess.run(control_command, capture_output=True, text=True, timeout=60)
    for run in (observed, control):
        if run.returncode:
            raise RuntimeError(f"native harness failed: {run.stderr[-4000:]}")
        if "overflow" in run.stderr or "halted=1" in run.stderr:
            raise RuntimeError(f"native fixture overflowed or halted: {run.stderr[-4000:]}")
        h8 = re.search(r"illegal=([0-9a-f]+):([0-9a-f]+) unsupported=([0-9a-f]+)", run.stderr)
        if not h8 or any(int(value, 16) for value in h8.groups()):
            raise RuntimeError(f"H8 fixture did not remain clean/inert: {run.stderr[-4000:]}")
        if "unmapped reads=0 writes=0" not in run.stderr:
            raise RuntimeError(f"native firmware accessed unmapped memory: {run.stderr[-4000:]}")
    report = lambda run: [line for line in run.stderr.splitlines() if line.startswith("System12:")]
    report_equal = report(observed) == report(control)
    ppm_equal = digest(directory / "observed.ppm") == digest(directory / "control.ppm")
    if not report_equal or not ppm_equal:
        raise RuntimeError(f"shared-access observer changed native output/report: {report_equal=} {ppm_equal=}")

    with (directory / "edges.csv").open(newline="") as stream:
        raw_rows = list(csv.DictReader(stream))
    rows = [{key: (value if key == "event" else int(value)) for key, value in row.items()}
            for row in raw_rows]
    if not all(first["sequence"] < second["sequence"] for first, second in zip(rows, rows[1:])):
        raise RuntimeError(f"native observer sequence is not monotonic: {rows!r}")
    stores = []
    for row in rows:
        if row["event"] != "main_shared_store" or not (0x1F080000 <= row["address"] <= 0x1F08000E):
            continue
        stores.append({"sequence": row["sequence"], "address": row["address"],
                       "value": row["value"], "width": row["width"],
                       "main_cycle": row["main_cycle"],
                       "time_as": row["main_cycle"] * NATIVE_MAIN_PERIOD_AS})
    return {"cpu": "drc" if drc else "interpreter", "command": observed_command,
            "control_command": control_command, "observer_control_equal": True,
            "board_report": report(observed), "stderr": observed.stderr,
            "trace_rows": rows, "stores": stores,
            "observed_ppm_sha256": digest(directory / "observed.ppm"),
            "control_ppm_sha256": digest(directory / "control.ppm")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    harness = args.harness.resolve()
    if not harness.is_file():
        parser.error(f"native harness not found: {harness}")
    oracle_path = args.mame.resolve()
    oracle = json.loads(oracle_path.read_text())
    if oracle.get("schema") != "pcsx-aow.mame-system12-psx-instruction-timing.v1":
        raise RuntimeError(f"unexpected MAME evidence schema: {oracle.get('schema')!r}")
    if len(oracle["reset_events"]) != 1 or oracle["reset_events"][0]["time"] != {
            "seconds": 0, "attoseconds": 0}:
        raise RuntimeError("MAME timing probe did not start from reset epoch zero")
    if not oracle["complete"] or oracle["runs"][0]["returncode"] != 0 or oracle["runs"][0]["timed_out"]:
        raise RuntimeError("MAME timing probe did not complete cleanly")
    words = {cpu: [int(value, 0) for value in values]
             for cpu, values in oracle["firmware_words"].items()}
    if words["main"] != [int(value, 0) for value in oracle["loaded_main_firmware_words"]]:
        raise RuntimeError("MAME's loaded PSX firmware differs from its declared words")
    expected = [{"address": row["address"], "value": row["value"],
                 "width": row["width"], "time_as": row["time_as"]}
                for row in oracle["stores"]]
    marker_rows = oracle["markers"]
    expected_identity = [(row["address"], row["value"]) for row in marker_rows]
    if [(row["address"], row["value"]) for row in expected] != expected_identity:
        raise RuntimeError("MAME markers do not match firmware marker definitions")

    args.output.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.log.resolve().parent.mkdir(parents=True, exist_ok=True)
    cases = []
    logs = []
    with tempfile.TemporaryDirectory(prefix="native-system12-psx-timing-") as temporary:
        base = Path(temporary)
        for drc in (False, True):
            case = run_case(harness, base / ("drc" if drc else "interpreter"), words["main"], drc)
            native_rows = case["stores"]
            actual_identity = [(row["address"], row["value"]) for row in native_rows]
            if actual_identity != expected_identity or any(row["width"] != 16 for row in native_rows):
                raise RuntimeError(f"{case['cpu']} marker address/value/width mismatch: {native_rows!r}")
            differences = []
            for marker, want, got in zip(marker_rows, expected, native_rows):
                if any(want[key] != got[key] for key in ("address", "value", "width", "time_as")):
                    differences.append({"marker": marker["label"], "mame": want,
                                        "native": {key: got[key] for key in
                                                   ("address", "value", "width", "time_as")},
                                        "time_delta_as": got["time_as"] - want["time_as"]})
            case["differences"] = differences
            case["equal"] = not differences
            cases.append(case)
            logs.append(f"=== {case['cpu']} observed harness ===\n{case['stderr']}\n")
            print(f"{case['cpu']}: fixture and observer controls pass; "
                  f"{len(differences)}/{len(expected)} exact MAME marker differences", flush=True)

    evidence = {"schema": "pcsx-aow.native-system12-psx-instruction-timing.v1",
                "harness": str(harness), "harness_sha256": digest(harness),
                "mame_evidence": str(oracle_path), "mame_evidence_sha256": digest(oracle_path),
                "firmware_words": words, "markers": marker_rows,
                "comparison": "exact reset-relative attoseconds, address, value, and 16-bit width; zero tolerance",
                "native_main_period_as": NATIVE_MAIN_PERIOD_AS,
                "observer_control_equal": all(case["observer_control_equal"] for case in cases),
                "expected_mame_stores": expected, "captures": cases,
                "equal": all(case["equal"] for case in cases)}
    args.output.resolve().write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    args.log.resolve().write_text("\n".join(logs), encoding="utf-8")
    print(f"native evidence JSON: {args.output.resolve()}")
    print("PSX instruction timing MAME fidelity " + ("PASS" if evidence["equal"] else "FAIL"))
    return int(not evidence["equal"])


if __name__ == "__main__":
    raise SystemExit(main())
