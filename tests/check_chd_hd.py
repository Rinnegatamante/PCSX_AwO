#!/usr/bin/env python3
"""Create actual compressed HDD CHDs from generated sectors, never game media."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def create_metadata_chd(chdman, root, name, payload, metadata):
    raw = root / f"{name}.raw"
    image = root / f"{name}.chd"
    raw.write_bytes(payload)
    run(chdman, "createhd", "-i", str(raw), "-o", str(image), "-chs",
        "2,2,5", "-ss", "512", "-hs", "1536", "-c", "none")
    for tag, value in metadata.items():
        value_path = root / f"{name}-{tag.strip()}.meta"
        value_path.write_bytes(value)
        run(chdman, "addmeta", "-i", str(image), "-t", tag, "-vf",
            str(value_path))
    return image


def set_metadata_empty(image, tag):
    """Set one CHD metadata record's payload length to zero for an empty-value fixture."""
    data = bytearray(image.read_bytes())
    offset = int.from_bytes(data[48:56], "big")  # CHD v5 metaoffset.
    while offset:
        current = bytes(data[offset:offset + 4]).decode("ascii")
        length_word = int.from_bytes(data[offset + 4:offset + 8], "big")
        following = int.from_bytes(data[offset + 8:offset + 16], "big")
        if current == tag:
            data[offset + 4:offset + 8] = (length_word & 0xff000000).to_bytes(4, "big")
            image.write_bytes(data)
            return
        offset = following
    raise AssertionError(f"metadata tag {tag!r} not found in {image}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test", required=True, type=Path)
    parser.add_argument("--chdman", default="chdman")
    args = parser.parse_args()
    chdman = shutil.which(args.chdman)
    if not chdman:
        parser.error("chdman is required to exercise the real CHD codecs")
    with tempfile.TemporaryDirectory(prefix="pcsx-chd-hd-") as temp:
        root = Path(temp)
        # Includes incompressible-looking bytes, hunk boundaries, final partial
        # hunk and a second, independently identified image.
        data = bytes((lba * 17 + byte * 13 + (byte >> 4)) & 255
                     for lba in range(20) for byte in range(512))
        for name, payload in (("disk", data), ("other", bytes(x ^ 0xAA for x in data))):
            raw = root / f"{name}.raw"
            raw.write_bytes(payload)
            run(chdman, "createhd", "-i", str(raw), "-o", str(root / f"{name}.chd"),
                "-chs", "2,2,5", "-ss", "512", "-hs", "1536")
        # A duplicate geometry record is invalid even if its numbers match.
        run(chdman, "createhd", "-i", str(root / "disk.raw"), "-o", str(root / "duplicate.chd"),
            "-chs", "2,2,5", "-ss", "512", "-hs", "1536", "-c", "none")
        run(chdman, "addmeta", "-i", str(root / "duplicate.chd"), "-t", "GDDD",
            "-ix", "1", "-vt", "CYLS:2,HEADS:2,SECS:5,BPS:512")
        (root / "truncated.chd").write_bytes((root / "disk.chd").read_bytes()[:124])
        create_metadata_chd(chdman, root, "custom", data, {
            "CIS ": bytes((0x01, 0x02, 0xfe, 0xff)),
            "KEY ": bytes((0x10, 0x20, 0x30, 0x40, 0x50)),
            "IDNT": bytes((index * 29 + 7) & 0xff for index in range(512)),
        })
        empty = create_metadata_chd(chdman, root, "empty", data,
                                    {"CIS ": b"x"})
        set_metadata_empty(empty, "CIS ")
        create_metadata_chd(chdman, root, "wronglen", data, {
            "KEY ": bytes((1, 2, 3, 4)),
            "IDNT": bytes((index * 3) & 0xff for index in range(511)),
        })
        create_metadata_chd(chdman, root, "metadata-error", data, {
            "CIS ": bytes(range(1, 17)),
        })
        before = {p: hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in root.glob("*.chd")}
        subprocess.run([str(args.test.resolve()), str(root)], check=True)
        for path, digest in before.items():
            assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, path
    print("CHD HDD native backing: generated real media, reads, overlay persistence and rejection PASS")


if __name__ == "__main__":
    main()
