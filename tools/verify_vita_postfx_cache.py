#!/usr/bin/env python3
"""Check or refresh the precompiled Vita post-processing shaders used by CI."""

import argparse
import hashlib
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "assets/postfx/postfx_embedded.c"
GENERATED = ROOT / "build-vita/postfx_embedded.c"
MANIFEST = ROOT / "assets/postfx/postfx_embedded.sha256"


def file_digest(path):
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def manifest_contents():
    inputs = sorted((ROOT / "assets/postfx").glob("*/*.cg"))
    inputs.extend((ROOT / "tools/embed_vita_postfx.py", CACHE))
    return "".join(
        f"{file_digest(path)}  {path.relative_to(ROOT).as_posix()}\n"
        for path in inputs
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--refresh", action="store_true",
        help="Copy shaders from a local psp2cgc build and update their source hashes",
    )
    args = parser.parse_args()

    if args.refresh:
        if not GENERATED.is_file():
            parser.error("Build shaders locally with psp2cgc before --refresh")
        shutil.copyfile(GENERATED, CACHE)
        MANIFEST.write_text(manifest_contents(), encoding="utf-8")
        print("Refreshed precompiled shaders and source hashes.")
        return

    if not CACHE.is_file() or not MANIFEST.is_file():
        parser.error("Precompiled shaders missing; build locally and run --refresh")
    if MANIFEST.read_text(encoding="utf-8") != manifest_contents():
        parser.error("Shader sources/cache have changed; regenerate with psp2cgc and run --refresh")
    print("Verified precompiled shaders match the tracked source hashes.")


if __name__ == "__main__":
    main()
