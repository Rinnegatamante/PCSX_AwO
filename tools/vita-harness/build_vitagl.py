#!/usr/bin/env python3
"""Build an isolated local VitaGL checkout with the required PCSX flags."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
FLAGS = {name: "1" for name in ("HAVE_SHADER_CACHE", "NO_DEBUG",
                               "READBACKS_SPEEDHACK", "CIRCULAR_POOL_SPEEDHACK")}


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT.parent / "vitaGL")
    parser.add_argument("--output", type=Path, default=ROOT / "build/vita-deps/vitaGL")
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if source == output or source in output.parents or output in source.parents:
        parser.error("source and build copy must be separate directories")
    if git(source, "status", "--porcelain", "--untracked-files=no"):
        parser.error("local VitaGL source has tracked changes; commit them before cloning")
    commit = git(source, "rev-parse", "HEAD")
    if not output.exists():
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--no-hardlinks", str(source), str(output)], check=True)
    if git(output, "rev-parse", "HEAD") != commit:
        parser.error("build copy differs from source; select a fresh output directory")
    if git(output, "status", "--porcelain", "--untracked-files=no"):
        parser.error("build copy has tracked changes; select a fresh output directory")
    archive = output / "libvitaGL.a"
    record = output / "pcsx-build.json"
    if archive.is_file() and record.is_file():
        facts = json.loads(record.read_text())
        if (facts.get("commit") == commit and facts.get("flags") == FLAGS and
                facts.get("archive_sha256") == hashlib.sha256(archive.read_bytes()).hexdigest()):
            print("Reusing verified VitaGL build: " + str(archive))
            return
    sdk = Path(os.environ.get("VITASDK", "/usr/local/vitasdk"))
    env = dict(os.environ, VITASDK=str(sdk), PATH=str(sdk / "bin") + os.pathsep + os.environ["PATH"])
    subprocess.run(["make", "-B", "-j4", *(f"{k}={v}" for k, v in FLAGS.items())],
                   cwd=output, env=env, check=True)
    facts = {"source": str(source), "commit": commit, "flags": FLAGS,
             "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
    record.write_text(json.dumps(facts, indent=2) + "\n")
    print(json.dumps(facts, indent=2))


if __name__ == "__main__":
    main()
