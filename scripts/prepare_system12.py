#!/usr/bin/env python3
"""Verify and assemble Namco System 12 Soul Calibur ROM regions.

Bundled MAME ROM declarations are used by default.  A local MAME checkout can
override them when refreshing or checking the snapshot.  The input archive is
never modified; output files are raw memory regions for a board-level harness.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path


DRIVER_RELATIVE_PATH = Path("src/mame/namco/namcos12.cpp")
DEFAULT_MANIFEST = Path(__file__).with_name("soulclbr_roms.json")
ROM_START_RE = re.compile(r"\bROM_START\s*\(\s*(soulclbr\w*)\s*\)")
CALL_RE = re.compile(r"\b(ROM_REGION(?:32_LE)?|ROM_LOAD(?:16_BYTE|16_WORD_SWAP)?|ROM_RELOAD)\s*\(")
CRC_RE = re.compile(r"\bCRC\s*\(\s*([0-9a-fA-F]+)\s*\)")
SHA1_RE = re.compile(r"\bSHA1\s*\(\s*([0-9a-fA-F]{40})\s*\)")


class PreparationError(Exception):
    """An input ROM, MAME source, or region layout failed validation."""


@dataclass(frozen=True)
class RomLoad:
    filename: str
    offset: int
    length: int
    load_type: str
    crc32: str
    sha1: str


@dataclass(frozen=True)
class RomSet:
    name: str
    regions: dict[str, int]
    loads: dict[str, tuple[RomLoad, ...]]


def _without_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def _call_arguments(source: str, open_paren: int) -> tuple[str, int]:
    depth = 1
    quoted = False
    escaped = False
    for index in range(open_paren + 1, len(source)):
        char = source[index]
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            continue
        if char == '"':
            quoted = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return source[open_paren + 1:index], index + 1
    raise PreparationError("unterminated MAME ROM declaration")


def _split_arguments(args: str) -> list[str]:
    parts: list[str] = []
    start = depth = 0
    quoted = escaped = False
    for index, char in enumerate(args):
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(args[start:index].strip())
            start = index + 1
    parts.append(args[start:].strip())
    return parts


def _integer(value: str) -> int:
    try:
        return int(value.strip(), 0)
    except ValueError as exc:
        raise PreparationError(f"unsupported number in MAME ROM declaration: {value}") from exc


def parse_rom_sets(driver_text: str) -> list[RomSet]:
    """Parse Soul Calibur ROM_START blocks from namcos12.cpp."""
    source = _without_comments(driver_text)
    starts = list(ROM_START_RE.finditer(source))
    parsed: list[RomSet] = []
    for start_match in starts:
        end_match = re.search(r"\bROM_END\b", source[start_match.end():])
        if end_match is None:
            raise PreparationError(f"missing ROM_END for {start_match.group(1)}")
        block_start = start_match.end()
        block_end = block_start + end_match.start()
        block = source[block_start:block_end]
        regions: dict[str, int] = {}
        loads: dict[str, list[RomLoad]] = {}
        active_region: str | None = None
        position = 0
        for call in CALL_RE.finditer(block):
            args_text, position = _call_arguments(block, call.end() - 1)
            args = _split_arguments(args_text)
            macro = call.group(1)
            if macro.startswith("ROM_REGION"):
                if len(args) < 2:
                    raise PreparationError(f"malformed region in {start_match.group(1)}")
                size, tag = _integer(args[0]), args[1].strip().strip('"')
                if tag in regions:
                    raise PreparationError(f"duplicate ROM region {tag} in {start_match.group(1)}")
                regions[tag] = size
                loads[tag] = []
                active_region = tag
            elif macro.startswith("ROM_LOAD"):
                if active_region is None or len(args) < 4:
                    raise PreparationError(f"load without a region in {start_match.group(1)}")
                crc_match = CRC_RE.search(args[3])
                sha_match = SHA1_RE.search(args[3])
                if not crc_match or not sha_match:
                    raise PreparationError(f"load missing CRC/SHA1: {args[0]}")
                loads[active_region].append(RomLoad(
                    filename=args[0].strip().strip('"'),
                    offset=_integer(args[1]),
                    length=_integer(args[2]),
                    load_type=macro.removeprefix("ROM_LOAD"),
                    crc32=crc_match.group(1).lower().zfill(8),
                    sha1=sha_match.group(1).lower(),
                ))
            elif macro == "ROM_RELOAD":
                raise PreparationError(f"ROM_RELOAD is not supported in Soul Calibur set {start_match.group(1)}")
        if not regions:
            raise PreparationError(f"no regions found in {start_match.group(1)}")
        parsed.append(RomSet(start_match.group(1), regions,
                             {tag: tuple(items) for tag, items in loads.items()}))
    if not parsed:
        raise PreparationError("no Soul Calibur ROM_START blocks found")
    return parsed


def read_static_manifest(path: Path = DEFAULT_MANIFEST) -> tuple[list[RomSet], str | None]:
    """Load bundled ROM declarations so normal use does not need a MAME checkout."""
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PreparationError(f"cannot read ROM manifest {path}: {exc}") from exc
    if manifest.get("schema_version") != 1 or not isinstance(manifest.get("sets"), list):
        raise PreparationError(f"unsupported Soul Calibur ROM manifest: {path}")
    romsets: list[RomSet] = []
    try:
        for item in manifest["sets"]:
            romsets.append(RomSet(
                name=item["name"],
                regions={tag: int(size) for tag, size in item["regions"].items()},
                loads={tag: tuple(RomLoad(**load) for load in items)
                       for tag, items in item["loads"].items()},
            ))
    except (KeyError, TypeError, ValueError) as exc:
        raise PreparationError(f"malformed Soul Calibur ROM manifest: {path}") from exc
    return romsets, manifest.get("source_commit")


def _digest(data: bytes) -> tuple[str, str]:
    return f"{zlib.crc32(data) & 0xffffffff:08x}", hashlib.sha1(data).hexdigest()


def _read_and_match_romset(archive: zipfile.ZipFile, romset: RomSet) -> dict[str, bytes] | None:
    names = archive.namelist()
    if len(names) != len(set(names)):
        raise PreparationError("ROM archive contains duplicate member names")
    member_names = set(names)
    data_by_name: dict[str, bytes] = {}
    for region_loads in romset.loads.values():
        for load in region_loads:
            if load.filename not in member_names:
                return None
            data = archive.read(load.filename)
            crc, sha1 = _digest(data)
            if len(data) != load.length or crc != load.crc32 or sha1 != load.sha1:
                return None
            data_by_name[load.filename] = data
    return data_by_name


def _assemble_region(size: int, loads: tuple[RomLoad, ...], data: dict[str, bytes], tag: str) -> bytes:
    region = bytearray(size)
    for load in loads:
        src = data[load.filename]
        if load.load_type == "16_BYTE":
            end = load.offset + max(0, len(src) - 1) * 2 + 1
            if end > size:
                raise PreparationError(f"{load.filename} exceeds region {tag}")
            region[load.offset:load.offset + len(src) * 2:2] = src
        elif load.load_type == "16_WORD_SWAP":
            if load.offset + len(src) > size:
                raise PreparationError(f"{load.filename} exceeds region {tag}")
            if len(src) & 1:
                raise PreparationError(f"odd-length WORD_SWAP ROM {load.filename}")
            swapped = bytearray(len(src))
            swapped[0::2] = src[1::2]
            swapped[1::2] = src[0::2]
            region[load.offset:load.offset + len(src)] = swapped
        else:
            if load.offset + len(src) > size:
                raise PreparationError(f"{load.filename} exceeds region {tag}")
            region[load.offset:load.offset + len(src)] = src
    return bytes(region)


def prepare(rom_zip: Path, output_dir: Path, mame_source: Path | None = None,
            manifest_path: Path = DEFAULT_MANIFEST) -> dict[str, object]:
    if not rom_zip.is_file():
        raise PreparationError(f"ROM archive not found: {rom_zip}")
    if mame_source is None:
        romsets, source_commit = read_static_manifest(manifest_path)
        source_description = str(manifest_path.resolve())
    else:
        driver_path = mame_source / DRIVER_RELATIVE_PATH
        if not driver_path.is_file():
            raise PreparationError(f"MAME driver not found: {driver_path}")
        romsets = parse_rom_sets(driver_path.read_text(encoding="utf-8"))
        source_description = str(driver_path.resolve())
        source_commit = None
        try:
            source_commit = subprocess.run(
                ["git", "-C", str(mame_source), "rev-parse", "HEAD"],
                check=True, capture_output=True, text=True,
            ).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            pass
    try:
        archive = zipfile.ZipFile(rom_zip)
    except (OSError, zipfile.BadZipFile) as exc:
        raise PreparationError(f"cannot read ROM archive: {exc}") from exc
    with archive:
        matched: tuple[RomSet, dict[str, bytes]] | None = None
        for romset in romsets:
            verified = _read_and_match_romset(archive, romset)
            if verified is not None:
                matched = (romset, verified)
                break
    if matched is None:
        raise PreparationError("archive does not exactly match a Soul Calibur MAME set (filename, size, CRC32, SHA1)")
    romset, rom_data = matched
    required_regions = ("maincpu:rom", "bankedroms", "sub", "c352")
    for tag in required_regions:
        if tag not in romset.regions:
            raise PreparationError(f"{romset.name} does not define required region {tag}")

    region_files = {
        "main.bin": _assemble_region(romset.regions["maincpu:rom"], romset.loads["maincpu:rom"], rom_data, "maincpu:rom"),
        "banked.bin": _assemble_region(romset.regions["bankedroms"], romset.loads["bankedroms"], rom_data, "bankedroms"),
        "sub.bin": _assemble_region(romset.regions["sub"], romset.loads["sub"], rom_data, "sub"),
        "wave.bin": _assemble_region(romset.regions["c352"], romset.loads["c352"], rom_data, "c352"),
    }
    expected_sizes = {"main.bin": 0x400000, "banked.bin": 0x2000000,
                      "sub.bin": 0x80000, "wave.bin": 0x1000000}
    for name, expected_size in expected_sizes.items():
        if len(region_files[name]) != expected_size:
            raise PreparationError(f"{romset.name} {name} is {len(region_files[name]):#x} bytes, expected {expected_size:#x}")

    output_dir.mkdir(parents=True, exist_ok=True)
    region_manifest: dict[str, dict[str, str | int]] = {}
    for name, content in region_files.items():
        (output_dir / name).write_bytes(content)
        crc, sha1 = _digest(content)
        region_manifest[name] = {"size": len(content), "sha1": sha1, "crc32": crc}

    manifest: dict[str, object] = {
        "system": "Namco System 12 / Soul Calibur",
        "mame_set": romset.name,
        "rom_archive": str(rom_zip.resolve()),
        "mame_driver_or_manifest": source_description,
        "mame_source_commit": source_commit,
        "output_regions": region_manifest,
        "roms": [
            {"filename": load.filename, "size": load.length,
             "crc32": load.crc32, "sha1": load.sha1}
            for tag in required_regions for load in romset.loads[tag]
        ],
    }
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom_zip", type=Path, help="local Soul Calibur ROM ZIP archive")
    parser.add_argument("-o", "--output-dir", type=Path, default=Path("artifacts/system12"))
    parser.add_argument("--mame-source", type=Path,
                        help="optional MAME checkout to override the bundled ROM declarations")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                        help=f"bundled ROM manifest (default: {DEFAULT_MANIFEST})")
    args = parser.parse_args(argv)
    try:
        manifest = prepare(args.rom_zip, args.output_dir, args.mame_source, args.manifest)
    except (PreparationError, OSError, zipfile.BadZipFile) as exc:
        print(f"prepare_system12: error: {exc}", file=sys.stderr)
        return 1
    print(f"Verified {manifest['mame_set']} from {args.rom_zip}; regions written to {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
