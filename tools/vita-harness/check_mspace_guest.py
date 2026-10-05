#!/usr/bin/env python3
"""Compile a Vita allocator ABI guest and run one independently guarded host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

import check_system12_gameplay_vita3k as helper

ROOT = Path(__file__).resolve().parents[2]
TITLE = "MSPACET01"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--firmware-fs", type=Path, required=True)
    parser.add_argument("--config-template", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sdk = Path(os.environ.get("VITASDK", "/usr/local/vitasdk"))
    env = dict(os.environ, VITASDK=str(sdk),
               PATH=str(sdk / "bin") + os.pathsep + os.environ["PATH"])
    source = ROOT / "tests/vita_guest_mspace.c"
    commands = [
        [str(sdk / "bin/arm-vita-eabi-gcc"), "-O2", "-g", "-mcpu=cortex-a9", "-marm",
         "-Wl,-q", str(source), "-lSceLibKernel_stub", "-o", str(out / "guest.elf")],
        ["vita-elf-create", str(out / "guest.elf"), str(out / "guest.velf")],
        ["vita-make-fself", "-c", "-s", "-na", str(out / "guest.velf"), str(out / "eboot.bin")],
        ["vita-mksfoex", "-s", "TITLE_ID=" + TITLE, "-d", "ATTRIBUTE2=12",
         "Vita mspace ABI test", str(out / "param.sfo")],
        ["vita-pack-vpk", "-s", str(out / "param.sfo"), "-b", str(out / "eboot.bin"),
         str(out / "mspace.vpk")],
    ]
    facts = {"scope": "Guest allocator API, statistics ABI and arena bounds; no GPU/game validation",
             "commands": commands, "status": "not run"}
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    try:
        with (out / "compile.log").open("w") as log:
            for cmd in commands:
                subprocess.run(cmd, env=env, check=True, stdout=log, stderr=subprocess.STDOUT)
        facts.update(source_sha256=digest(source), vpk_sha256=digest(out / "mspace.vpk"))
        with tempfile.TemporaryDirectory(prefix="pcsx-vita-mspace-") as temporary:
            emulator, _fs, _portable = helper.read_cli(Path(temporary), args)
            facts["host_sha256"] = digest(emulator)
            log = out / "vita3k.log"
            if not helper.install_vpk(emulator, out / "mspace.vpk", TITLE, log, args.timeout):
                raise RuntimeError("mspace VPK installation failed")
            helper.TITLE_ID = TITLE
            passed, issues = helper.run_guest(emulator, "mspace", log, args.timeout,
                ("pcsx-vita: mspace PASS", "Stopping -> Idle"))
            facts.update(runtime_issues=issues, status="passed" if passed else "failed")
            if not passed:
                raise RuntimeError("guest allocator test did not pass")
    except Exception as exc:
        facts.update(status="failed", error=str(exc))
    finally:
        (out / "evidence.json").write_text(json.dumps(facts, indent=2) + "\n")
    print(json.dumps(facts, indent=2))
    return 0 if facts["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
