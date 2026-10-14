#!/usr/bin/env python3
"""Pack the loose Vita ROM selector covers into a standalone PSARC archive."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def locate_tool(explicit):
    candidates = [
        explicit,
        shutil.which("psp2psarc"),
        str(Path(os.environ.get("VITASDK", "C:/vitasdk")) / "bin" / "psp2psarc.exe"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(Path(candidate).resolve())
    raise SystemExit("psp2psarc.exe not found; install the VitaSDK or pass --tool.")


def run_tool(command):
    process = subprocess.run(command, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True,
                             encoding="utf-8", errors="replace")
    if process.returncode:
        raise RuntimeError(f"{' '.join(command[:3])} failed: {process.stdout.strip()}")
    return process.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("covers"))
    parser.add_argument("--output", type=Path, default=Path("covers.psarc"))
    parser.add_argument("--tool", type=str)
    args = parser.parse_args()

    source = args.source.resolve()
    output = args.output.resolve()
    if not source.is_dir():
        raise SystemExit(f"Missing source folder: {source}")
    files = sorted((p for p in source.iterdir()
                    if p.is_file() and p.suffix.lower() == ".png"),
                   key=lambda p: (p.name.casefold(), p.name))
    if not files:
        raise SystemExit("No PNG covers in source directory.")
    if len({p.name.casefold() for p in files}) != len(files):
        raise SystemExit("Case-insensitive duplicate filenames in source directory.")
    if any(p.name in ("..", ".") for p in files):
        raise SystemExit("Unsafe filename.")

    tool = locate_tool(args.tool)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="psarc-covers-", dir=output.parent) as tmpdir:
        workdir = Path(tmpdir)
        listfile = workdir / "files.txt"
        indexfile = workdir / "__covers_index.txt"
        archive = workdir / "covers.psarc"
        indexfile.write_bytes(("\n".join(p.name for p in files) + "\n").encode("utf-8"))
        listfile.write_text("\n".join(str(p) for p in files + [indexfile]) + "\n",
                            encoding="utf-8")
        print(f"Packing {len(files)} covers with {tool}...", flush=True)
        run_tool([tool, "create", "-q", "-N", "-C", "-S", "-o",
                  str(archive), "-I", str(listfile)])
        print("Verifying PSARC with VitaSDK...", flush=True)
        run_tool([tool, "verify", "-q", str(archive)])
        listing = run_tool([tool, "list", "-q", str(archive)])
        count = sum(line.rstrip().endswith("%)") for line in listing.splitlines())
        if count != len(files) + 1:
            raise RuntimeError(f"PSARC contains {count} entries, expected {len(files) + 1}")
        os.replace(archive, output)

    source_bytes = sum(p.stat().st_size for p in files)
    print(f"OK: {output} ({output.stat().st_size:,} bytes, "
          f"{len(files)} covers, source {source_bytes:,} bytes)", flush=True)


if __name__ == "__main__":
    main()
