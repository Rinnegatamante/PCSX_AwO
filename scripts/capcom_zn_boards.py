#!/usr/bin/env python3
"""Extract Capcom ZN board/media profiles from the pinned local MAME sources."""
from __future__ import annotations

import argparse
from dataclasses import replace
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import prepare_mame_psx as mame

COMMIT = mame.PINNED_MAME_COMMIT
DRIVER = "sony/zn.cpp"
QSOUND_DRIVER = "devices/sound/qsoundhle.cpp"
DEFAULT_SOURCE = Path(__file__).resolve().parents[2] / "mamedev"
DEFAULT_INVENTORY = Path(__file__).with_name("mame_psx_systems.json")
DEFAULT_OUTPUT = Path(__file__).with_name("capcom_zn_boards.json")
REQUIRED_REGIONS = ["maincpu:rom", "cat702_1", "cat702_2", "countryrom",
                    "bankedroms", "audiocpu", "qsound"]
OPTIONAL_UNDUMPED_REGION = "upd78081"
EXPECTED_QSOUND_DSP = {
    "set": "qsound_hle", "region": "dsp", "filename": "dl-1425.bin",
    "size": 0x2000, "file_size": 0x6000, "ignored_bytes": 0x4000,
    "crc32": "d6cf5ef5",
    "sha1": "555f50fe5cdf127619da7d854c03f4a244a0c501",
}


class CapcomProfileError(Exception):
    """Pinned source or inventory does not support a complete profile set."""


def _source_commit(source: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise CapcomProfileError(f"cannot identify MAME checkout at {source}") from exc


def _game_rows(text: str) -> dict[str, dict[str, Any]]:
    clean = mame._without_comments(text)
    rows: dict[str, dict[str, Any]] = {}
    for match in re.finditer(r"\bGAME\s*\(", clean):
        args_text, _ = mame._call_arguments(clean, match.end() - 1)
        args = mame._split_arguments(args_text)
        if len(args) < 11:
            raise CapcomProfileError("unrecognized GAME declaration in sony/zn.cpp")
        parent = args[2].strip()
        name = args[1].strip()
        if name in rows:
            raise CapcomProfileError(f"duplicate GAME entry: {name}")
        rows[name] = {
            "set": name, "year": int(args[0].strip()),
            "parent": None if parent in ("0", "nullptr") else parent,
            "machine": args[3].strip(), "input": args[4].strip(),
            "state": args[5].strip(), "init": args[6].strip(),
            "rotation": args[7].strip(), "manufacturer": args[8].strip().strip('"'),
            "description": args[9].strip().strip('"'), "flags": args[10].strip(),
        }
    return rows


def _function_body(text: str, pattern: str, name: str) -> str:
    match = re.search(pattern.format(name=re.escape(name)), text)
    if not match:
        raise CapcomProfileError(f"cannot find MAME machine configuration {name}")
    opening = text.find("{", match.end())
    depth = 1
    pos = opening + 1
    while pos < len(text) and depth:
        if text[pos] == "{": depth += 1
        elif text[pos] == "}": depth -= 1
        pos += 1
    if depth:
        raise CapcomProfileError(f"unterminated MAME machine configuration {name}")
    return text[opening + 1:pos - 1]


def _machine_profile(source: str, machine: str) -> dict[str, Any]:
    pattern = (r"void\s+" + re.escape(machine) +
               r"\s*\(\s*machine_config\s*&config\s*\)")
    body = _function_body(source, pattern, machine)
    config = next((entry for entry in ("zn1_1mb_vram", "zn1_2mb_vram", "zn2")
                   if re.search(r"\b" + entry + r"\s*\(\s*config\s*\)", body)), None)
    if config == "zn1_1mb_vram":
        return {"board_model": "COH1000C", "board_family": "ZN-1",
                "cpu_device": "CXD8530CQ", "cpu_clock_hz": 67737600,
                "gpu_device": "CXD8561Q", "gpu_clock_hz": 53693175,
                "main_ram_size": 0x400000, "vram_size": 0x100000}
    if config == "zn1_2mb_vram":
        return {"board_model": "COH1002C", "board_family": "ZN-1",
                "cpu_device": "CXD8530CQ", "cpu_clock_hz": 67737600,
                "gpu_device": "CXD8561Q", "gpu_clock_hz": 53693175,
                "main_ram_size": 0x400000, "vram_size": 0x200000}
    if config == "zn2":
        return {"board_model": "COH3002C", "board_family": "ZN-2",
                "cpu_device": "CXD8661R", "cpu_clock_hz": 100000000,
                "gpu_device": "CXD8654Q", "gpu_clock_hz": 53693175,
                "main_ram_size": 0x400000, "vram_size": 0x200000}
    raise CapcomProfileError(f"unknown Capcom ZN configuration {machine} in MAME source")


def _region_info(romset: mame.RomSet) -> list[dict[str, Any]]:
    result = []
    for region in romset.regions.values():
        files = []
        seen: set[int] = set()
        for op in region.operations:
            if not isinstance(op, mame.LoadOp) or op.file_index in seen:
                continue
            seen.add(op.file_index)
            spec = romset.files[op.file_index]
            files.append({"filename": spec.filename, "size": spec.expected_size,
                          "crc32": spec.crc32, "sha1": spec.sha1,
                          "status": spec.status, "bios_id": spec.bios_id})
        result.append({"tag": region.tag, "size": region.size, "width": region.width,
                       "endian": region.endian, "flags": region.flags,
                       "erase": region.erase, "files": files})
    return result


def _bios_parent(name: str, rows: dict[str, dict[str, Any]]) -> str | None:
    seen = {name}
    parent = rows[name].get("parent")
    while parent:
        if parent in seen:
            raise CapcomProfileError(f"cycle in GAME parent chain for {name}")
        seen.add(parent)
        if "MACHINE_IS_BIOS_ROOT" in rows.get(parent, {}).get("flags", ""):
            return parent
        parent = rows.get(parent, {}).get("parent")
    return None


def _qsound_profile(source: Path) -> dict[str, Any]:
    path = source / "src" / QSOUND_DRIVER
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise CapcomProfileError(f"cannot read QSound HLE ROM source {path}: {exc}") from exc
    # MAME's ROM_IGNORE extends the expected input file length but contributes
    # no bytes to the region.  Verify that exact source directive, remove it
    # for the generic parser, then restore the true complete-file length for
    # archive digest validation.
    ignore = re.findall(r"\bROM_IGNORE\s*\(\s*(0x[0-9a-fA-F]+|\d+)\s*\)",
                        mame._without_comments(text))
    if ignore != ["0x4000"]:
        raise CapcomProfileError("QSound DSP must have exactly one ROM_IGNORE(0x4000)")
    parsed_text = re.sub(r"\bROM_IGNORE\s*\(\s*0x4000\s*\)", "", text)
    sets, _, report = mame.parse_driver(parsed_text, QSOUND_DRIVER)
    romset = sets.get(EXPECTED_QSOUND_DSP["set"])
    if romset is None:
        reason = next((r["reason"] for r in report if r["set"] == EXPECTED_QSOUND_DSP["set"]), "missing")
        raise CapcomProfileError(f"cannot parse QSound DSP image declaration: {reason}")
    matches = [file for file in romset.files if file.filename == EXPECTED_QSOUND_DSP["filename"]]
    region = romset.regions.get(EXPECTED_QSOUND_DSP["region"])
    if len(matches) != 1 or region is None:
        raise CapcomProfileError("QSound DSP declaration must have one firmware file and dsp region")
    spec = matches[0]
    for key in ("size", "crc32", "sha1"):
        if getattr(spec, {"size": "expected_size", "crc32": "crc32", "sha1": "sha1"}[key]) != EXPECTED_QSOUND_DSP[key]:
            raise CapcomProfileError(f"QSound DSP {key} differs from pinned MAME declaration")
    if region.size != EXPECTED_QSOUND_DSP["size"] or region.width != 16 or region.endian != "little":
        raise CapcomProfileError("QSound DSP ROM region geometry differs from pinned declaration")
    if spec.expected_size != EXPECTED_QSOUND_DSP["size"]:
        raise CapcomProfileError("QSound load length differs from pinned declaration")
    spec = replace(spec, expected_size=EXPECTED_QSOUND_DSP["file_size"])
    romset.files[spec.index] = spec
    return {"source": QSOUND_DRIVER, **EXPECTED_QSOUND_DSP,
            "width": region.width, "endian": region.endian}


def _verify_optional_undumped_proof(source: str, romset: mame.RomSet,
                                   profile: dict[str, Any]) -> dict[str, Any] | None:
    region = romset.regions.get(OPTIONAL_UNDUMPED_REGION)
    if region is None:
        return None
    specs = [romset.files[op.file_index] for op in region.operations
             if isinstance(op, mame.LoadOp)]
    if region.size != 0x2000 or len(specs) != 1:
        raise CapcomProfileError(f"unreviewed {OPTIONAL_UNDUMPED_REGION} region layout")
    spec = specs[0]
    if (spec.filename != "78081g503.ic655" or spec.expected_size != 0x2000
            or spec.status != "no_dump" or spec.crc32 is not None or spec.sha1 is not None):
        raise CapcomProfileError(f"unreviewed NO_DUMP media in {OPTIONAL_UNDUMPED_REGION}")
    if profile["state"] != "capcom_zn_state":
        return None
    class_match = re.search(r"class\s+capcom_zn_state\s*:[^{]*\{", source)
    class_body = _function_body(source, r"class\s+capcom_zn_state\b", "capcom_zn_state")
    if not class_match or "upd78081" in class_body:
        raise CapcomProfileError("Capcom class now references the undumped upd78081")
    config = _function_body(source,
        r"void\s+" + re.escape(profile["machine"]) +
        r"\s*\(\s*machine_config\s*&config\s*\)", profile["machine"])
    if "capcom_zn(config)" not in config:
        raise CapcomProfileError("Capcom machine config no longer selects the reviewed board setup")
    capcom_setup = _function_body(source, r"void\s+capcom_zn\s*\(\s*machine_config\s*&config\s*\)", "capcom_zn")
    if "upd78081" in capcom_setup or "UPD78081" in capcom_setup:
        raise CapcomProfileError("Capcom board setup now instantiates upd78081")
    zn_base = _function_body(source, r"void\s+zn_state::zn_base\s*\(\s*machine_config\s*&config\s*\)", "zn_base")
    if "ZNMCU(config, m_znmcu)" not in zn_base:
        raise CapcomProfileError("MAME ZN base no longer instantiates the known ZNMCU")
    return {
        "tag": OPTIONAL_UNDUMPED_REGION, "size": region.size,
        "filename": spec.filename, "status": "NO_DUMP",
        "handling": "omitted_from_media_package_no_substitute",
        "source_proof": "sony/zn.cpp Capcom machine configs use capcom_zn_state + ZNMCU; no upd78081 device is instantiated or read by that class/config",
        "known_limitation": "the separate upd78081 firmware remains undumped; no MCU implementation claim is made by this package",
    }


def build_registry(source_dir: Path = DEFAULT_SOURCE,
                   inventory_path: Path = DEFAULT_INVENTORY) -> dict[str, Any]:
    driver_path = source_dir / "src/mame" / DRIVER
    try:
        source = mame._without_comments(driver_path.read_text(encoding="utf-8"))
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CapcomProfileError(f"cannot read Capcom ZN source/inventory: {exc}") from exc
    if _source_commit(source_dir) != COMMIT:
        raise CapcomProfileError("MAME checkout is not at the pinned source commit")
    systems = [system for system in inventory.get("systems", []) if system.get("driver") == DRIVER]
    if len(systems) != 1:
        raise CapcomProfileError("inventory must contain the Sony ZN family")
    inventory_sets = {row["set"] for row in systems[0].get("sets", [])}
    rows = _game_rows(source)
    if set(rows) != inventory_sets or len(rows) != 116:
        raise CapcomProfileError(f"sony/zn.cpp and inventory mismatch (GAME {len(rows)}, inventory {len(inventory_sets)})")
    all_romsets, parents, coverage = mame.parse_driver(source, DRIVER)
    if len(coverage) != 116 or any(row["status"] != "parsed" for row in coverage):
        raise CapcomProfileError("generic ROM parser did not classify all 116 Sony ZN declarations")

    family_rows = [row for row in rows.values()
                   if row["state"] in ("capcom_zn_state", "glpracr_state")]
    if len(family_rows) != 47:
        raise CapcomProfileError(f"expected 47 Capcom-family GAME entries, found {len(family_rows)}")
    bios_roots = [row["set"] for row in family_rows if "MACHINE_IS_BIOS_ROOT" in row["flags"]]
    if set(bios_roots) != {"coh1000c", "coh3002c"}:
        raise CapcomProfileError(f"unexpected Capcom BIOS roots: {bios_roots}")
    qsound = _qsound_profile(source_dir)

    profiles = []
    for game in family_rows:
        name = game["set"]
        romset = all_romsets[name]
        regions = _region_info(romset)
        region_by_tag = {region["tag"]: region for region in regions}
        board = _machine_profile(source, game["machine"])
        is_root = "MACHINE_IS_BIOS_ROOT" in game["flags"]
        state = game["state"]
        unsupported = []
        if state != "capcom_zn_state":
            unsupported.append(f"unsupported Capcom subclass {state}")
        if "MACHINE_NOT_WORKING" in game["flags"]:
            unsupported.append("MACHINE_NOT_WORKING")
        if "MACHINE_UNEMULATED_PROTECTION" in game["flags"]:
            unsupported.append("MACHINE_UNEMULATED_PROTECTION")
        missing = [tag for tag in REQUIRED_REGIONS if tag not in region_by_tag]
        if missing:
            unsupported.append("missing required regions: " + ", ".join(missing))
        required_no_dump = [file["filename"] for tag in REQUIRED_REGIONS
                            for file in region_by_tag.get(tag, {}).get("files", [])
                            if file["status"] == "no_dump"]
        if required_no_dump:
            unsupported.append("required NO_DUMP media: " + ", ".join(required_no_dump))
        extra_tags = [region["tag"] for region in regions
                      if region["tag"] not in REQUIRED_REGIONS and
                      region["tag"] != OPTIONAL_UNDUMPED_REGION]
        if extra_tags:
            unsupported.append("unsupported ROM regions: " + ", ".join(extra_tags))
        if romset.disks:
            unsupported.append("required disk/CHD media")
        if romset.bios and not romset.default_bios:
            unsupported.append("BIOS selection has no MAME default")
        if is_root:
            unsupported.append("BIOS root has no game program")
        optional_undumped = _verify_optional_undumped_proof(source, romset, game)
        bios_parent = _bios_parent(name, rows)
        if not is_root and bios_parent not in bios_roots:
            unsupported.append("no declared Capcom BIOS parent")
        profiles.append({
            **game, **board, "is_bios_root": is_root,
            "bios_parent": bios_parent,
            "bios_options": romset.bios,
            "default_bios": romset.default_bios,
            "rom_set": romset.name,
            "required_regions": REQUIRED_REGIONS,
            "rom_regions": regions,
            "disk_media": [disk.name for disk in romset.disks],
            "optional_undumped_regions": ([OPTIONAL_UNDUMPED_REGION]
                                           if OPTIONAL_UNDUMPED_REGION in region_by_tag else []),
            "optional_undumped_media_review": optional_undumped,
            "runtime_candidate": not unsupported,
            "unsupported_requirements": unsupported,
        })
    profiles.sort(key=lambda row: row["set"])
    return {
        "schema_version": 1, "mame_source_commit": COMMIT,
        "driver": DRIVER, "inventory_source": "scripts/mame_psx_systems.json",
        "qsound_dsp": qsound,
        "bios_roots": bios_roots, "family_game_count": len(profiles),
        "board_model_counts": {model: sum(p["board_model"] == model for p in profiles)
                               for model in ("COH1000C", "COH1002C", "COH3002C")},
        "profiles": profiles,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame-source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--inventory", type=Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args(argv)
    try:
        document = build_registry(args.mame_source, args.inventory)
        args.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    except (CapcomProfileError, mame.PreparationError, OSError) as exc:
        print(f"capcom_zn_boards: {exc}", file=sys.stderr)
        return 2
    print(f"wrote {document['family_game_count']} Capcom-family profiles to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
