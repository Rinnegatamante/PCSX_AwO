#!/usr/bin/env python3
"""Rebuild and test the reproducible Dynarmic A32 VADDHN patch for Vita3K."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

WORKSPACE = Path(__file__).resolve().parents[2]
PATCH_FILE = WORKSPACE / "tools/vita-harness/patches/vita3k-neon.patch"
TEST = WORKSPACE / "tools/vita-harness/test_vaddhn.cpp"
FILES = (
    "src/dynarmic/frontend/A32/decoder/asimd.inc",
    "src/dynarmic/frontend/A32/translate/impl/a32_translate_impl.h",
    "src/dynarmic/frontend/A32/translate/impl/asimd_three_regs.cpp",
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run(argv: list[str], *, cwd: Path | None = None, timeout: int | None = None,
        env: dict[str, str] | None = None) -> None:
    print("+", " ".join(map(str, argv)), flush=True)
    subprocess.run(argv, cwd=cwd, check=True, timeout=timeout, env=env)


def get_git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], check=True, text=True,
                          capture_output=True).stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vita3k", type=Path, default=WORKSPACE.parent / "vita3k",
                        help="Vita3K checkout (default: sibling ../vita3k)")
    parser.add_argument("--build-dir", type=Path, default=None,
                        help="configured macOS CMake build dir (default: <vita3k>/build/macos)")
    parser.add_argument("--output-dir", type=Path, default=WORKSPACE / "build/dynarmic-vaddhn",
                        help="destination for the tested archive and manifest")
    args = parser.parse_args()

    vita3k = args.vita3k.resolve()
    build = (args.build_dir or (vita3k / "build/macos")).resolve()
    output = args.output_dir.resolve()
    repo = vita3k / "external/dynarmic"
    build_sources = build / "external/dynarmic-patched/src/dynarmic"
    archive = build / "external/dynarmic/src/dynarmic/libdynarmic.a"
    if not repo.is_dir() or not build_sources.is_dir():
        raise SystemExit("Expected a Dynarmic checkout and configured patched build tree")

    base_contents: dict[str, bytes] = {}
    patched_contents: dict[str, bytes] = {}
    stage = output / "patch-stage"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for relative in FILES:
        data = subprocess.run(["git", "-C", str(repo), "show", "HEAD:" + relative],
                              check=True, capture_output=True).stdout
        base_contents[relative] = data
        destination = stage / "external/dynarmic" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)

    patch_env = os.environ.copy()
    patch_env.pop("GIT_DIR", None)
    patch_env.pop("GIT_WORK_TREE", None)
    patch_env["GIT_CEILING_DIRECTORIES"] = str(stage.parent)
    run(["git", "apply", "--check", str(PATCH_FILE)], cwd=stage, env=patch_env)
    run(["git", "apply", str(PATCH_FILE)], cwd=stage, env=patch_env)
    for relative in FILES:
        patched_contents[relative] = (stage / "external/dynarmic" / relative).read_bytes()

    # The configured source copy includes Vita3K's local backend changes. Only replace
    # these three frontend files, and refuse to overwrite a third-party local edit.
    for relative in FILES:
        target = build_sources / relative.removeprefix("src/dynarmic/")
        current = target.read_bytes()
        if current not in (base_contents[relative], patched_contents[relative]):
            raise SystemExit(f"Refusing to overwrite unrecognized build-tree change: {target}")
    for relative in FILES:
        target = build_sources / relative.removeprefix("src/dynarmic/")
        if target.read_bytes() != patched_contents[relative]:
            target.write_bytes(patched_contents[relative])

    run(["cmake", "--build", str(build), "--target", "dynarmic", "-j4"])
    output.mkdir(parents=True, exist_ok=True)
    harness = output / "test_vaddhn"
    inc = build / "external/dynarmic-patched"
    command = [
        os.environ.get("CXX", "clang++"), "-std=c++20", "-O2",
        "-I" + str(inc / "src"),
        "-I" + str(inc / "externals/oaknut/include"),
        "-I" + str(vita3k / "external/fmt/include"),
        "-I" + str(inc / "externals/mcl/include"),
        str(TEST), str(archive),
        str(build / "external/dynarmic/externals/mcl/src/libmcl.a"),
        str(build / "external/fmt/libfmt.a"), "-pthread", "-o", str(harness),
    ]
    run(command)
    run([str(harness)], timeout=15)

    tested_archive = output / "libdynarmic.a"
    shutil.copy2(archive, tested_archive)
    patch_bytes = PATCH_FILE.read_bytes()
    manifest = {
        "dynarmic_commit": get_git(repo, "rev-parse", "HEAD"),
        "patch": str(PATCH_FILE),
        "patch_sha256": sha256(patch_bytes),
        "source_sha256": {relative: sha256(patched_contents[relative]) for relative in FILES},
        "archive": str(tested_archive),
        "archive_sha256": sha256(tested_archive.read_bytes()),
        "test": str(TEST),
        "test_result": "PASS",
        "test_timeout_seconds": 15,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"validated archive: {tested_archive}")
    print(f"sha256: {manifest['archive_sha256']}")
    print(f"manifest: {output / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
