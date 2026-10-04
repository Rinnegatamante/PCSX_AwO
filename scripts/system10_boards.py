#!/usr/bin/env python3
"""Extract auditable Namco System 10 profiles from the pinned MAME source."""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import prepare_mame_psx as mame

COMMIT = mame.PINNED_MAME_COMMIT
DRIVER = "namco/namcos10.cpp"
DEFAULT_SOURCE = Path(__file__).resolve().parents[2] / "mamedev"
DEFAULT_INVENTORY = Path(__file__).with_name("mame_psx_systems.json")
DEFAULT_OUTPUT = Path(__file__).with_name("system10_boards.json")


class System10ProfileError(Exception):
    """Pinned source or inventory does not support a complete registry."""


def _game_rows(source: str) -> dict[str, dict[str, str]]:
    clean = mame._without_comments(source)
    rows: dict[str, dict[str, str]] = {}
    for match in re.finditer(r"\bGAME\s*\(", clean):
        args_text, _ = mame._call_arguments(clean, match.end() - 1)
        args = mame._split_arguments(args_text)
        if len(args) < 11:
            raise System10ProfileError("unrecognized GAME declaration in namcos10.cpp")
        name = args[1].strip()
        if name in rows:
            raise System10ProfileError(f"duplicate System 10 GAME declaration: {name}")
        parent = args[2].strip()
        rows[name] = {
            "year": args[0].strip(),
            "set": name,
            "parent": None if parent in ("0", "nullptr") else parent,
            "machine": args[3].strip(),
            "input": args[4].strip(),
            "state": args[5].strip(),
            "init": args[6].strip(),
            "rotation": args[7].strip(),
            "manufacturer": args[8].strip().strip('"'),
            "description": args[9].strip().strip('"'),
            "flags": args[10].strip(),
        }
    return rows


def _function_body(source: str, pattern: str, name: str) -> str:
    match = re.search(pattern.format(name=re.escape(name)), source)
    if not match:
        raise System10ProfileError(f"cannot find source function for {name}")
    opening = source.find("{", match.end())
    depth = 1
    pos = opening + 1
    while pos < len(source) and depth:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1
    if depth:
        raise System10ProfileError(f"unterminated source function for {name}")
    return source[opening + 1:pos - 1]


def _machine_facts(source: str, machine: str, board_type: str) -> tuple[list[str], str | None]:
    function = "ns10_" + machine.removeprefix("ns10_")
    pattern = r"void\s+namcos10_\w+_state::" + re.escape(function) + r"\s*\(\s*machine_config\s*&config\s*\)"
    try:
        body = _function_body(source, pattern, function)
    except System10ProfileError:
        # MEM(M) has a single board setup but its ROM unscrambler is selected by init.
        if board_type != "MEM(M)":
            raise
        body = ""
    helpers = set(re.findall(r"\b(namcos10_(?:exio|mgexio|exfinalio))\s*\(\s*config\s*\)", body))
    extensions = []
    for helper, label in (("namcos10_exio", "EXIO"), ("namcos10_mgexio", "MGEXIO"),
                          ("namcos10_exfinalio", "EXFINALIO")):
        if helper in helpers:
            extensions.append(label)
    if board_type == "MEM(P3)":
        extensions.append("MEM(P3) MP3/analog ROM board")
    elif board_type == "MEM/IO":
        extensions.append("MEM/IO Taito ROM board")
    elif board_type not in ("MEM(M)", "MEM(N)"):
        extensions.append(board_type)

    crypto_profile = None
    if board_type == "MEM(M)":
        crypto_profile = "mrdrilr2"
    elif function.startswith("ns10_"):
        crypto_profile = function.removeprefix("ns10_")
    return extensions, crypto_profile


def _region_info(romset: mame.RomSet) -> list[dict[str, Any]]:
    result = []
    for region in romset.regions.values():
        records = []
        seen: set[int] = set()
        for operation in region.operations:
            if not isinstance(operation, mame.LoadOp) or operation.file_index in seen:
                continue
            seen.add(operation.file_index)
            spec = romset.files[operation.file_index]
            records.append({
                "filename": spec.filename, "size": spec.expected_size,
                "crc32": spec.crc32, "sha1": spec.sha1, "status": spec.status,
            })
        result.append({
            "tag": region.tag, "size": region.size, "width": region.width,
            "endian": region.endian, "flags": region.flags, "erase": region.erase,
            "files": records,
        })
    return result


def _source_commit(source_dir: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(source_dir), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise System10ProfileError(f"cannot identify MAME checkout at {source_dir}") from exc


def _crypto_profile_names() -> set[str]:
    path = Path(__file__).resolve().parents[1] / "libpcsxcore/arcade/namcos10_crypto_profiles.inc"
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise System10ProfileError(f"cannot read native crypto profile table: {exc}") from exc
    names = set(re.findall(r"static const Profile profile_(\w+)", text))
    names.update(re.findall(r'\{"(\w+)",&profile_\w+\}', text))
    return names


def _source_crypto_profile_names(source: str) -> set[str]:
    names = set()
    for match in re.finditer(r"void\s+namcos10_\w+_state::(ns10_(\w+)|init_mrdrilr2)\s*\(", source):
        function = match.group(1)
        profile = match.group(2) or "mrdrilr2"
        body = _function_body(source, r"void\s+namcos10_\w+_state::" +
                              re.escape(function) + r"\s*\(", function)
        if "m_unscrambler" in body:
            names.add(profile)
    return names


def build_registry(source_dir: Path = DEFAULT_SOURCE,
                   inventory_path: Path = DEFAULT_INVENTORY) -> dict[str, Any]:
    driver_path = source_dir / "src/mame" / DRIVER
    try:
        source = mame._without_comments(driver_path.read_text(encoding="utf-8"))
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise System10ProfileError(f"cannot read System 10 source/inventory: {exc}") from exc
    if _source_commit(source_dir) != COMMIT:
        raise System10ProfileError("local MAME source is not at the pinned commit")
    systems = [row for row in inventory.get("systems", []) if row.get("driver") == DRIVER]
    if len(systems) != 1:
        raise System10ProfileError("inventory must contain one Namco System 10 family")
    inventory_sets = {row["set"] for row in systems[0].get("sets", [])}
    games = _game_rows(source)
    romsets, _, coverage = mame.parse_driver(source, DRIVER)
    if set(games) != inventory_sets or len(games) != 45:
        raise System10ProfileError(
            f"System 10 GAME/source inventory mismatch (source {len(games)}, inventory {len(inventory_sets)})")
    if set(games) != set(romsets):
        raise System10ProfileError("System 10 GAME entries and ROM_START declarations do not match")
    if len(coverage) != 45 or any(item.get("status") != "parsed" for item in coverage):
        raise System10ProfileError("the generic MAME parser did not classify all 45 ROM blocks")

    crypto_profiles = _crypto_profile_names()
    source_crypto_profiles = _source_crypto_profile_names(source)
    rows = []
    for name in games:
        row = games[name]
        state = row["state"]
        board_type = {
            "namcos10_memm_state": "MEM(M)",
            "namcos10_memn_state": "MEM(N)",
            "namcos10_memp3_state": "MEM(P3)",
            "namcos10_memio_state": "MEM/IO",
        }.get(state)
        if not board_type:
            raise System10ProfileError(f"unclassified board state {state} for {name}")
        extensions, crypto_profile = _machine_facts(source, row["machine"], board_type)
        if crypto_profile and (crypto_profile not in crypto_profiles or
                               crypto_profile not in source_crypto_profiles):
            crypto_profile = None
        crypto_profile_source = (f"machine:{row['machine']}" if board_type == "MEM(N)" else
                                 f"init:{row['init']}" if board_type == "MEM(M)" else None)
        romset = romsets[name]
        region_tags = [region.tag for region in romset.regions.values()]
        unsupported = list(extensions)
        if any(file.status == "no_dump" for file in romset.files):
            unsupported.append("NO_DUMP media")
        if romset.disks:
            unsupported.append("CHD/disc media")
        if "MACHINE_NOT_WORKING" in row["flags"]:
            unsupported.append("MACHINE_NOT_WORKING")
        if "MACHINE_UNEMULATED_PROTECTION" in row["flags"]:
            unsupported.append("MACHINE_UNEMULATED_PROTECTION")
        base_regions = ({"maincpu:rom", "nand", "data"} if board_type == "MEM(M)"
                        else {"maincpu:rom", "nand0"} if board_type == "MEM(N)" else set())
        if not base_regions.issubset(region_tags):
            unsupported.append("missing base board ROM regions")
        if board_type in ("MEM(M)", "MEM(N)") and not crypto_profile:
            unsupported.append("no native source-derived crypto profile")
        allowed_region = (lambda tag: tag in base_regions) if board_type == "MEM(M)" else (
            lambda tag: tag == "maincpu:rom" or re.fullmatch(r"nand\d+", tag) is not None)
        extra_regions = [tag for tag in region_tags if not allowed_region(tag)]
        if extra_regions:
            unsupported.append("extra ROM regions: " + ", ".join(extra_regions))
        rows.append({
            **row, "board_type": board_type, "extensions": extensions,
            "crypto_profile": crypto_profile, "crypto_profile_source": crypto_profile_source,
            "rom_set": romset.name,
            "rom_regions": _region_info(romset), "rom_files": len(romset.files),
            "disk_media": [disk.name for disk in romset.disks],
            "no_dump_files": [file.filename for file in romset.files if file.status == "no_dump"],
            "extra_rom_regions": extra_regions,
            "runtime_candidate": not unsupported,
            "unsupported_requirements": unsupported,
        })
    rows.sort(key=lambda item: item["set"])
    return {
        "schema_version": 1,
        "mame_source_commit": COMMIT,
        "source": DRIVER,
        "inventory_source": "scripts/mame_psx_systems.json",
        "set_count": len(rows),
        "board_types": {board: sum(row["board_type"] == board for row in rows)
                        for board in ("MEM(M)", "MEM(N)", "MEM(P3)", "MEM/IO")},
        "profiles": rows,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame-source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--inventory", type=Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args(argv)
    try:
        data = build_registry(args.mame_source, args.inventory)
        args.output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    except (System10ProfileError, mame.PreparationError, OSError) as exc:
        print(f"system10_boards: {exc}", file=sys.stderr)
        return 2
    print(f"wrote {len(data['profiles'])} profiles to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
