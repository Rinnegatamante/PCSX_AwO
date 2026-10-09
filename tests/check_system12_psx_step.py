#!/usr/bin/env python3
"""Compare the source-step PSX interpreter exactly against real MAME step cases.

Each case from tests/check_mame_psx_step.py (branches, load delay, exceptions
in delay slots and a branch in a delay slot) runs its identical injected words
with --system12-source-step. Shared-RAM stores compare address, value, width
and reset-relative attoseconds with zero tolerance.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STORE_LIMIT = 0x1F08003F  # The MAME tap range.


def load(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {relative}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    harness = args.harness.resolve()
    oracle_path = args.mame.resolve()
    oracle = json.loads(oracle_path.read_text(encoding="utf-8"))
    if oracle.get("schema") != "pcsx-aow.mame-psx-step.v1":
        parser.error("unexpected MAME step evidence schema")
    timing = load("system12_psx_timing", "tests/check_system12_psx_instruction_timing.py")
    steps = load("mame_psx_step", "tests/check_mame_psx_step.py")
    definitions = {case["name"]: case for case in steps.CASES}

    captures = []
    with tempfile.TemporaryDirectory(prefix="native-system12-psx-step-") as temporary:
        for mame_case in oracle["cases"]:
            name = mame_case["name"]
            words = steps.full_main_image(definitions[name])
            if [f"0x{word:08x}" for word in words] != mame_case["firmware_words"]["main"]:
                raise RuntimeError(f"{name}: MAME evidence firmware differs from case definition")
            native = timing.run_case(harness, Path(temporary) / name, words, False,
                                     source_step=True, store_limit=STORE_LIMIT)
            keys = ("address", "value", "width", "time_as")
            want = [{key: row[key] for key in keys} for row in mame_case["stores"]]
            got = [{key: row[key] for key in keys} for row in native["stores"]]
            # MAME exits at a case's terminal marker, while native runs a full
            # frame: compare up to that marker. A case without one ran two MAME
            # frames, so every native store of the frame must match.
            if mame_case["complete"]:
                got = got[:len(want)]
            equal = got == want
            captures.append({"name": name, "equal": equal, "mame": want, "native": got,
                             "native_command": native["command"]})
            print(f"{name}: {'PASS' if equal else 'FAIL'} ({len(want)} stores)")
            if not equal:
                for index, (a, b) in enumerate(zip(want, got + [None] * len(want))):
                    if a != b:
                        print(f"  store {index}: mame={a} native={b}")

    evidence = {"schema": "pcsx-aow.native-system12-psx-step.v1",
                "harness": str(harness), "harness_sha256": digest(harness),
                "mame_evidence": str(oracle_path), "mame_evidence_sha256": digest(oracle_path),
                "comparison": "exact address, value, width and reset-relative attoseconds",
                "captures": captures, "equal": all(case["equal"] for case in captures)}
    args.output.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.output.resolve().write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print("PSX source step MAME fidelity " + ("PASS" if evidence["equal"] else "FAIL"))
    return int(not evidence["equal"])


if __name__ == "__main__":
    raise SystemExit(main())
