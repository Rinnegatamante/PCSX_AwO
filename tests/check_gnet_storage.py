#!/usr/bin/env python3
"""Build synthetic CHDs and run the G-Net storage composition tests."""
from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def create(chdman: str, root: Path, name: str, sector_bytes: int = 512,
           metadata: dict[str, bytes] | None = None,
           hunk_bytes: int = 1536) -> Path:
    path = root / f"{name}.chd"
    source_chd = root / f"{name}-source.chd"
    sectors = 20
    raw = root / f"{name}.raw"
    raw.write_bytes(bytes((n * 13 + 7) & 0xff
                          for n in range(sectors * sector_bytes)))
    run(chdman, "createhd", "-i", str(raw), "-o", str(source_chd), "-chs",
        "2,2,5", "-ss", str(sector_bytes), "-hs", str(hunk_bytes), "-c", "none")
    for tag, value in (metadata or {}).items():
        value_path = root / f"{name}-{tag.strip() or 'blank'}.meta"
        value_path.write_bytes(value)
        run(chdman, "addmeta", "-i", str(source_chd), "-t", tag, "-vf",
            str(value_path))
    # `addmeta` only writes uncompressed CHDs in the pinned chdman. Re-encode
    # the resulting source data and metadata so overlay identity SHA1 exists.
    run(chdman, "copy", "-i", str(source_chd), "-o", str(path), "-c", "zlib")
    return path


def metadata_offset(data: bytes, wanted: bytes) -> int:
    offset = int.from_bytes(data[48:56], "big")
    seen = set()
    while offset:
        if offset in seen or offset + 16 > len(data):
            raise AssertionError("invalid CHD metadata chain")
        seen.add(offset)
        tag = data[offset:offset + 4]
        next_offset = int.from_bytes(data[offset + 8:offset + 16], "big")
        if tag == wanted:
            return offset
        offset = next_offset
    raise AssertionError(f"metadata {wanted!r} not found")


def set_metadata_empty(image: Path, tag: bytes) -> None:
    data = bytearray(image.read_bytes())
    offset = metadata_offset(data, tag)
    length_word = int.from_bytes(data[offset + 4:offset + 8], "big")
    data[offset + 4:offset + 8] = (length_word & 0xff000000).to_bytes(4, "big")
    image.write_bytes(data)


def make_broken_metadata(source: Path, destination: Path, tag: bytes) -> None:
    data = bytearray(source.read_bytes())
    offset = metadata_offset(data, tag)
    length_word = int.from_bytes(data[offset + 4:offset + 8], "big")
    data[offset + 4:offset + 8] = (length_word & 0xff000000 | 0x00ffffff).to_bytes(4, "big")
    destination.write_bytes(data)


def main() -> None:
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test", required=True, type=Path)
    parser.add_argument("--chdman", default="chdman")
    args = parser.parse_args()
    chdman = shutil.which(args.chdman)
    if not chdman:
        parser.error("chdman is required to build local CHD fixtures")

    with tempfile.TemporaryDirectory(prefix="pcsx-gnet-storage-") as temp:
        root = Path(temp)
        identify = bytearray((n * 29 + 7) & 0xff for n in range(512))
        identify[:2] = b"\x34\x12"  # Raw IDNT is not rewritten to 0x848a.
        identify[-2:] = b"\xfe\x01"
        custom = {
            "CIS ": b"\x01\x02\xfe\xff",
            "KEY ": b"\x10\x21\x32\x43\x54",
            "IDNT": bytes(identify),
        }
        complete = create(chdman, root, "complete", metadata=custom)
        missing = create(chdman, root, "missing")
        empty = create(chdman, root, "empty-cis",
                       metadata={"CIS ": b"x", "KEY ": custom["KEY "],
                                 "IDNT": custom["IDNT"]})
        set_metadata_empty(empty, b"CIS ")
        wrong_key = create(chdman, root, "wrong-key",
                           metadata={"CIS ": custom["CIS "],
                                     "KEY ": b"\x10\x21\x32\x43",
                                     "IDNT": custom["IDNT"]})
        wrong_identify = create(chdman, root, "wrong-identify",
                                metadata={"IDNT": bytes(511)})
        # Same valid geometry/media except for a non-512-byte sector size.
        create(chdman, root, "bad-geometry", sector_bytes=1024,
               hunk_bytes=2048)
        broken = root / "broken-cis.chd"
        make_broken_metadata(complete, broken, b"CIS ")

        images = list(root.glob("*.chd"))
        before = {image: hashlib.sha256(image.read_bytes()).hexdigest()
                  for image in images}
        subprocess.run([str(args.test.resolve()), str(root)], check=True)
        for image, digest in before.items():
            if image.exists():
                assert hashlib.sha256(image.read_bytes()).hexdigest() == digest, image
    print("G-Net storage synthetic CHD/metadata fixtures PASS")


if __name__ == "__main__":
    main()
