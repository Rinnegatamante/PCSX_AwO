#!/usr/bin/env python3
"""Build vitaGL for the Vita frontend: a pinned upstream commit, PCSX's patch
series on top (tools/vita-harness/patches/vitagl-*.patch) and the required
flags, in an isolated copy (build/vita-deps/vitaGL by default).

The commit is pinned here so that builds do not depend on whatever a sibling
checkout holds. To move to a newer vitaGL, change VITAGL_COMMIT, check that
the series still applies, and rebase the patches that do not. --source takes
a local clone (or mirror) holding that commit instead of fetching it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
VITAGL_REPO = "https://github.com/Rinnegatamante/vitaGL"
# 2026-10-07: GLSL translator fixes (negative mod, comparisons), precomputed
# draws for clears and framebuffer blits, scissor refresh.
VITAGL_COMMIT = "dca4b9d143290d78ec043131a19be36c78cdc7b5"
FLAGS = {name: "1" for name in ("HAVE_SHADER_CACHE", "NO_DEBUG",
                               "READBACKS_SPEEDHACK", "CIRCULAR_POOL_SPEEDHACK")}
PATCHES = (
    ROOT / "tools/vita-harness/patches/vitagl-shutdown.patch",
    ROOT / "tools/vita-harness/patches/vitagl-shader-cache.patch",
    ROOT / "tools/vita-harness/patches/vitagl-readback.patch",
    ROOT / "tools/vita-harness/patches/vitagl-finish.patch",
    ROOT / "tools/vita-harness/patches/vitagl-fbo-last-frame.patch",
    ROOT / "tools/vita-harness/patches/vitagl-fbo-stencil-free.patch",
    ROOT / "tools/vita-harness/patches/vitagl-fbo-resize-reattach.patch",
    ROOT / "tools/vita-harness/patches/vitagl-fbo-msaa.patch",
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
    parser.add_argument("--source", type=Path,
                        help="local clone holding the pinned commit (default: fetch " + VITAGL_REPO + ")")
    parser.add_argument("--commit", default=VITAGL_COMMIT,
                        help="override the pinned commit (to try a newer vitaGL)")
    parser.add_argument("--output", type=Path, default=ROOT / "build/vita-deps/vitaGL")
    parser.add_argument("--trace-sampler-bindings", action="store_true",
                        help="Build bounded diagnostic logs for fragment sampler selection")
    parser.add_argument("--trace-shutdown", action="store_true",
                        help="Build bounded diagnostic logs for vitaGL shutdown phases")
    parser.add_argument("--trace-allocator-failures", action="store_true",
                        help="Build a private archive with bounded mapped-allocation retry logs")
    args = parser.parse_args()
    output = args.output.resolve()
    source = str(args.source.resolve()) if args.source else VITAGL_REPO
    if args.source and (args.source.resolve() == output or args.source.resolve() in output.parents
                        or output in args.source.resolve().parents):
        parser.error("source and build copy must be separate directories")
    commit = args.commit
    patch_series = []
    optional_patches = ()
    if args.trace_sampler_bindings:
        optional_patches += (ROOT / "tools/vita-harness/patches/vitagl-sampler-trace.patch",)
    if args.trace_shutdown:
        optional_patches += (ROOT / "tools/vita-harness/patches/vitagl-shutdown-trace.patch",)
    if args.trace_allocator_failures:
        optional_patches += (ROOT / "tools/vita-harness/patches/vitagl-allocator-failure-trace.patch",)
    patches = PATCHES + optional_patches
    for patch in patches:
        if not patch.is_file():
            parser.error("tracked VitaGL patch is missing: " + str(patch))
        patch_bytes = patch.read_bytes()
        patch_series.append((patch, patch, hashlib.sha256(patch_bytes).hexdigest()))
    patch_facts = [{"name": patch.name, "sha256": patch_sha256}
                   for patch, _, patch_sha256 in patch_series]
    if not output.exists():
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--no-checkout", "--no-hardlinks", source, str(output)], check=True)
        if subprocess.run(["git", "-C", str(output), "checkout", "-q", "--detach", commit]).returncode:
            parser.error(f"{source} has no commit {commit}; fetch it there or drop --source")
    if git(output, "rev-parse", "HEAD") != git(output, "rev-parse", commit + "^{commit}"):
        parser.error("build copy is at another vitaGL commit; delete it or select a fresh output directory")
    commit = git(output, "rev-parse", "HEAD")
    try:
        apply_patch_series(output, patch_series)
    except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
        parser.error("cannot prepare isolated VitaGL build copy: " + str(exc))
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
    facts = {"source": source, "commit": commit, "flags": FLAGS,
             "patches": patch_facts,
             "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
    record.write_text(json.dumps(facts, indent=2) + "\n")
    print(json.dumps(facts, indent=2))


if __name__ == "__main__":
    main()
