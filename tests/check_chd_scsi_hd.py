#!/usr/bin/env python3
"""Build generated CHD media and verify the NCR-to-SCSI disk path."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test", required=True, type=Path)
    parser.add_argument("--chdman", default="chdman")
    args = parser.parse_args()
    chdman = shutil.which(args.chdman)
    if not chdman:
        parser.error("chdman is required to exercise the real CHD codecs")

    with tempfile.TemporaryDirectory(prefix="pcsx-chd-scsi-hd-") as temp:
        root = Path(temp)
        image = root / "disk.chd"
        raw = root / "disk.raw"
        overlay = root / "disk.overlay"
        data = bytes((lba * 17 + byte * 13 + (byte >> 4)) & 0xff
                     for lba in range(20) for byte in range(512))
        raw.write_bytes(data)
        subprocess.run(
            [chdman, "createhd", "-i", str(raw), "-o", str(image), "-chs",
             "2,2,5", "-ss", "512", "-hs", "1536"],
            check=True, stdout=subprocess.DEVNULL)
        original_digest = hashlib.sha256(image.read_bytes()).digest()
        subprocess.run([str(args.test.resolve()), str(image), str(overlay)],
                       check=True)
        assert hashlib.sha256(image.read_bytes()).digest() == original_digest
        assert overlay.is_file() and overlay.stat().st_size > 80

    print("CHD-backed NCR/NSCSI HDD integration and source-image immutability PASS")


if __name__ == "__main__":
    main()
