#!/usr/bin/env python3
"""Create real CHD media and exercise the standalone GQ board adapter."""
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
    with tempfile.TemporaryDirectory(prefix="pcsx-konamigq-") as temp:
        root = Path(temp)
        image = root / "gq-hdd.chd"
        overlay = root / "gq-hdd.overlay"
        raw = root / "gq-hdd.raw"
        raw.write_bytes(bytes((lba * 17 + byte * 13 + (byte >> 4)) & 0xff
                              for lba in range(20) for byte in range(512)))
        subprocess.run([chdman, "createhd", "-i", str(raw), "-o", str(image),
                        "-chs", "2,2,5", "-ss", "512", "-hs", "1536"],
                       check=True, stdout=subprocess.DEVNULL)
        digest = hashlib.sha256(image.read_bytes()).digest()
        subprocess.run([str(args.test.resolve()), str(image), str(overlay)],
                       check=True)
        assert hashlib.sha256(image.read_bytes()).digest() == digest
    print("GQ guest sound, board map, and CHD-backed NCR/HD DMA PASS")


if __name__ == "__main__":
    main()
