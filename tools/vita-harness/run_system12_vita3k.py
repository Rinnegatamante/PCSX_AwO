#!/usr/bin/env python3
"""Run any prepared System 12 board profile in an isolated Vita3K and keep
its report, final-frame capture and Vita3K's own screenshots.

A lighter companion of check_system12_gameplay_vita3k.py (which validates
Soul Calibur end to end): it reuses that runner's isolation, packaging and
guest helpers, skips the console smoke, and accepts any profile directory
from prepare_system12_board.py.

  run_system12_vita3k.py --emulator Vita3K-fixed.app --vpk pcsx-rearmed-vita-neon.vpk \\
      --firmware-fs .../portable/fs --profile DIR --frames 6000 --output-dir OUT
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tools/vita-harness/check_system12_gameplay_vita3k.py"
GAME_FILES = ("board.cfg", "main.bin", "banked.bin", "sub.bin", "wave.bin")


def load(path: Path):
    spec = importlib.util.spec_from_file_location("system12_vita3k_harness", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--vpk", type=Path, required=True)
    parser.add_argument("--firmware-fs", type=Path, required=True)
    parser.add_argument("--config-template", type=Path)
    parser.add_argument("--profile", type=Path, required=True, help="prepare_system12_board.py output")
    parser.add_argument("--frames", type=int, default=6000)
    parser.add_argument("--screenshots", default="5,10,20,30,45,60,75,90,105,120,150,180")
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--gpu-thread", action="store_true")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--vita-pack-vpk", type=Path, default=shutil.which("vita-pack-vpk"))
    parser.add_argument("--vita-mksfoex", type=Path, default=shutil.which("vita-mksfoex"))
    args = parser.parse_args()
    if args.output_dir.exists():
        parser.error("output directory already exists; choose a fresh path")
    cfg = dict(line.split("=", 1) for line in
               (args.profile / "board.cfg").read_text().splitlines() if "=" in line)
    name = cfg["mame_set"]
    if cfg.get("unsupported", "none") != "none":
        parser.error(f"{name} declares unsupported devices: {cfg['unsupported']}")
    h = load(HARNESS)
    args.output_dir.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix="pcsx-vita-system12-run-") as temp:
        base = Path(temp)
        emulator, fs, portable = h.read_cli(base, args)
        files = [(args.profile / f, f"{name}/{f}") for f in GAME_FILES]
        vpk = h.make_vpk(args.vpk.resolve(), args.vita_pack_vpk, args.vita_mksfoex,
                         base / f"{name}.vpk", base, h.TITLE_ID, f"PCSX Vita System12 {name}", files)
        log = args.output_dir / "vita3k.log"
        log.touch()
        if not h.install_vpk(emulator, vpk, h.TITLE_ID, log, args.timeout):
            raise SystemExit(f"VPK installation failed; log={log}")
        report_guest = f"ux0:data/system12-{args.frames}.txt"
        capture_guest = f"ux0:data/system12-{args.frames}.ppm"
        app_args = (f"pcsx-vita, --system12, ux0:app/{h.TITLE_ID}/{name}, --frames, {args.frames}, "
                    f"--report-system12, {report_guest}, --capture-ppm, {capture_guest}")
        if args.gpu_thread:
            app_args += ", --gpu-thread"
        expected = (f"pcsx-vita: system12-report PASS path={report_guest}",
                    f"pcsx-vita: system12-capture PASS path={capture_guest}")
        screenshot_root = portable / "screenshots"
        before = set(screenshot_root.rglob("*.png")) if screenshot_root.exists() else set()
        ok, issues = h.run_guest(emulator, app_args, log, args.timeout, expected,
                                 extra_env={"VITA3K_AUTO_SCREENSHOT": args.screenshots})
        shots = []
        if screenshot_root.exists():
            dest = args.output_dir / "screenshots"
            dest.mkdir()
            for png in sorted(set(screenshot_root.rglob("*.png")) - before):
                shutil.copy2(png, dest / png.name)
                shots.append(png.name)
        guest = fs / "ux0/data"
        for suffix in ("txt", "ppm"):
            src = guest / f"system12-{args.frames}.{suffix}"
            if src.is_file():
                shutil.copy2(src, args.output_dir / src.name)
        result = {"set": name, "frames": args.frames, "passed": ok, "issues": issues,
                  "screenshots": shots, "vpk": str(args.vpk.resolve())}
        (args.output_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
