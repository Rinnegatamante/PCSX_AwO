#!/usr/bin/env python3
"""Build an isolated local VitaGL checkout with the required PCSX flags."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FLAGS = {name: "1" for name in ("HAVE_SHADER_CACHE", "NO_DEBUG",
                               "READBACKS_SPEEDHACK", "CIRCULAR_POOL_SPEEDHACK")}
PATCHES = (
    ROOT / "tools/vita-harness/patches/vitagl-shutdown.patch",
    ROOT / "tools/vita-harness/patches/vitagl-shader-cache.patch",
)


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def patch_paths(patch_bytes):
    if isinstance(patch_bytes, Path):
        patch_bytes = patch_bytes.read_bytes()
    paths = set()
    for line in patch_bytes.decode("utf-8").splitlines():
        if line.startswith("+++ b/"):
            paths.add(line[6:])
    return paths


def apply_patch_series(output, patch_series):
    actual_diff = subprocess.check_output(
        ["git", "-C", str(output), "diff", "--binary", "HEAD"])
    if actual_diff:
        changed_paths = set(subprocess.check_output(
            ["git", "-C", str(output), "diff", "--name-only", "HEAD"], text=True).splitlines())
        expected_paths = set().union(*(patch_paths(patch) for _, patch, _ in patch_series))
        if changed_paths != expected_paths:
            raise RuntimeError("build copy has changes outside the tracked VitaGL patch series")
        with tempfile.TemporaryDirectory(prefix="vitagl-patch-check-") as temp_name:
            expected = Path(temp_name)
            for path in expected_paths:
                baseline = subprocess.check_output(["git", "-C", str(output), "show", "HEAD:" + path])
                target = expected / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(baseline)
            for _, patch, _ in patch_series:
                subprocess.run(["git", "apply", str(patch)], cwd=expected, check=True)
            if any((expected / path).read_bytes() != (output / path).read_bytes()
                   for path in expected_paths):
                raise RuntimeError("build copy has extra or modified hunks in a patched file")
        return

    for _, patch, _ in patch_series:
        subprocess.run(["git", "-C", str(output), "apply", "--check", str(patch)], check=True)
        subprocess.run(["git", "-C", str(output), "apply", str(patch)], check=True)
    changed_paths = set(subprocess.check_output(
        ["git", "-C", str(output), "diff", "--name-only", "HEAD"], text=True).splitlines())
    expected_paths = set().union(*(patch_paths(patch) for _, patch, _ in patch_series))
    if changed_paths != expected_paths:
        raise RuntimeError("applied VitaGL patch series has unexpected changed files")


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
    patch_series = []
    for patch in PATCHES:
        if not patch.is_file():
            parser.error("tracked VitaGL patch is missing: " + str(patch))
        patch_bytes = patch.read_bytes()
        patch_series.append((patch, patch, hashlib.sha256(patch_bytes).hexdigest()))
    patch_facts = [{"name": patch.name, "sha256": patch_sha256}
                   for patch, _, patch_sha256 in patch_series]
    if not output.exists():
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--no-hardlinks", str(source), str(output)], check=True)
    if git(output, "rev-parse", "HEAD") != commit:
        parser.error("build copy differs from source; select a fresh output directory")
    try:
        apply_patch_series(output, patch_series)
    except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
        parser.error("cannot prepare isolated shutdown build copy: " + str(exc))
    archive = output / "libvitaGL.a"
    record = output / "pcsx-build.json"
    if archive.is_file() and record.is_file():
        facts = json.loads(record.read_text())
        if (facts.get("commit") == commit and facts.get("flags") == FLAGS and
                facts.get("patches") == patch_facts and
                facts.get("archive_sha256") == hashlib.sha256(archive.read_bytes()).hexdigest()):
            print("Reusing verified VitaGL build: " + str(archive))
            return
    sdk = Path(os.environ.get("VITASDK", "/usr/local/vitasdk"))
    env = dict(os.environ, VITASDK=str(sdk), PATH=str(sdk / "bin") + os.pathsep + os.environ["PATH"])
    subprocess.run(["make", "-B", "-j4", *(f"{k}={v}" for k, v in FLAGS.items())],
                   cwd=output, env=env, check=True)
    facts = {"source": str(source), "commit": commit, "flags": FLAGS,
             "patches": patch_facts,
             "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
    record.write_text(json.dumps(facts, indent=2) + "\n")
    print(json.dumps(facts, indent=2))


if __name__ == "__main__":
    main()
