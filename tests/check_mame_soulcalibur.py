#!/usr/bin/env python3
"""Audit the supplied Soul Calibur set and capture a headless MAME attract run."""

import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def logged_run(command, log):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log.write_text("$ " + " ".join(map(str, command)) + "\n" + result.stdout
                   + f"\nexit_code={result.returncode}\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame", default="mame")
    parser.add_argument("--rom", type=Path, default=ROOT / "roms/soulclbr.zip")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/mame")
    args = parser.parse_args()
    rom = args.rom.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(rom) as archive:
        manifest = [{"name":info.filename, "size":info.file_size,
                     "crc32":f"{info.CRC:08x}",
                     "sha1":hashlib.sha1(archive.read(info)).hexdigest()}
                    for info in archive.infolist()]
    (output / "rom-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    names = {entry["name"] for entry in manifest}
    if not {"soc14verc.2e", "soc14verc.2j"} <= names:
        parser.error("This checker expects the supplied Asia SOC14/VER.C set (soulclbrac).")

    version = logged_run([args.mame, "-version"], output / "version.log")
    version.check_returncode()
    # Keep the expected parent-set failure as evidence of the misleading zip name.
    parent = logged_run([args.mame, "-noreadconfig", "-rompath", str(rom.parent),
                         "-verifyroms", "soulclbr"], output / "soulclbr-verify.log")
    print(parent.stdout.strip())

    with tempfile.TemporaryDirectory(prefix="soulclbr-mame-") as tempdir:
        temp = Path(tempdir)
        # Supply the correct MAME driver name without altering the user's archive.
        shutil.copyfile(rom, temp / "soulclbrac.zip")
        audit = logged_run([args.mame, "-noreadconfig", "-rompath", str(temp),
                            "-verifyroms", "soulclbrac"], output / "soulclbrac-verify.log")
        print(audit.stdout.strip())
        audit.check_returncode()
        capture = output / "capture.lua"
        capture.write_text('''local screen = manager.machine.screens[":screen"]
local elapsed = 0
for _, secs in ipairs({5, 15, 30, 45, 60}) do
    emu.wait(secs - elapsed)
    elapsed = secs
    local err = screen:snapshot(string.format("attract-%03ds.png", secs))
    print(string.format("CAPTURE time=%ds snapshot=%s", secs, tostring(err)))
end
manager.machine:exit()
''')
        command = [args.mame, "soulclbrac", "-noreadconfig", "-rompath", str(temp),
                   "-snapshot_directory", str(output), "-cfg_directory", str(temp / "cfg"),
                   "-nvram_directory", str(temp / "nvram"), "-autoboot_script", str(capture),
                   "-autoboot_delay", "0", "-skip_gameinfo", "-video", "none",
                   "-sound", "none", "-nothrottle", "-seconds_to_run", "65"]
        # Delete old captures so a failed run cannot be mistaken for success.
        for seconds in (5, 15, 30, 45, 60):
            (output / f"attract-{seconds:03d}s.png").unlink(missing_ok=True)
        run = logged_run(command, output / "run.log")
        print(run.stdout.strip())
        run.check_returncode()
        if "LUA ERROR" in run.stdout:
            raise RuntimeError("MAME capture script failed; see run.log")
        # A clean NVRAM boot still shows the small Namco logo at 15 seconds.
        for seconds in (30, 45, 60):
            snapshot = output / f"attract-{seconds:03d}s.png"
            if not snapshot.exists() or snapshot.stat().st_size < 10000:
                raise RuntimeError(f"Missing or unexpectedly empty attract capture: {snapshot}")
    print(f"ROM audit and 60-second attract capture passed. Inspect PNGs in {output}")


if __name__ == "__main__":
    main()
