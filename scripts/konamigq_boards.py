#!/usr/bin/env python3
"""Extract the pinned System GQ hardware and media declarations, without a boot claim."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import prepare_mame_psx as mame

COMMIT = mame.PINNED_MAME_COMMIT
DRIVER = "konami/konamigq.cpp"
DEFAULT_SOURCE = Path(__file__).resolve().parents[2] / "mamedev"
DEFAULT_OUTPUT = Path(__file__).with_name("konamigq_boards.json")
INVENTORY = Path(__file__).with_name("mame_psx_systems.json")


class ProfileError(Exception):
    pass


def source_facts(raw: str) -> dict[str, Any]:
    source = mame._without_comments(raw)
    required = (
        "CXD8530BQ(config, m_maincpu, XTAL(67'737'600))",
        'set_default_size("4M")',
        'CXD8538Q(config, "gpu", XTAL(53\'693\'175), 0x200000',
        "M68000(config, m_soundcpu, XTAL(32'000'000)/4)",
        "TMS57002(config, m_dasp, XTAL(48'000'000)/2)",
        "MB89371(config, m_duart, 4_MHz_XTAL)",
        'EEPROM_93C46_16BIT(config, "eeprom").default_data(konamigq_def_eeprom, 128)',
        'NSCSI_HARDDISK(config, "harddisk")',
        "scsi.set_external_device(0, hd)",
        "scsi.set_external_device(7, m_ncr53cf96)",
        "NCR53CF96(config, m_ncr53cf96, 32_MHz_XTAL/2)",
        "K056800(config, m_k056800, XTAL(18'432'000))",
        'K054539(config, "k054539_1", XTAL(18\'432\'000))',
        'K054539(config, "k054539_2", XTAL(18\'432\'000))',
        'map(0x000000, 0x07ffff).rom().region("k054539", 0)',
        'map(0x080000, 0x3fffff).ram().share("pcmram")',
    )
    for token in required:
        if token not in source:
            raise ProfileError(f"reviewed GQ source declaration changed: {token}")
    match = re.search(r"konamigq_def_eeprom\[64\]\s*=\s*\{([^}]+)\}", source)
    if not match:
        raise ProfileError("missing source default EEPROM table")
    values = [value.strip() for value in match.group(1).split(",") if value.strip()]
    if len(values) != 64 or any(not re.fullmatch(r"0x[0-9a-fA-F]{4}", v) for v in values):
        raise ProfileError("default EEPROM must contain exactly 64 source 16-bit words")
    words = [int(value, 16) for value in values]
    storage = b"".join(word.to_bytes(2, "little") for word in words)
    return {
        "cpu_device": "CXD8530BQ", "cpu_clock_hz": 67737600,
        "main_ram_size": 0x400000, "gpu_device": "CXD8538Q",
        "gpu_clock_hz": 53693175, "gpu_model": 1, "vram_size": 0x200000,
        "sound_cpu_device": "M68000", "sound_cpu_clock_hz": 8000000,
        "effects_device": "TMS57002", "effects_clock_hz": 24000000,
        "duart_device": "MB89371", "duart_clock_hz": 4000000,
        "sound_interface": "K056800", "sound_interface_clock_hz": 18432000,
        "pcm_device": "K054539", "pcm_device_count": 2,
        "pcm_clock_hz": 18432000, "pcm_ram_base": 0x80000,
        "pcm_ram_size": 0x380000,
        "scsi_device": "NCR53CF96", "scsi_clock_hz": 16000000,
        "scsi_initiator_id": 7, "scsi_target_id": 0,
        "scsi_target_device": "NSCSI_HARDDISK",
        "eeprom_device": "93C46 16-bit", "eeprom_size": 128,
        "eeprom_initialization": "MAME driver default_data; no EEPROM ROM region",
        "eeprom_default_words": words,
        "eeprom_storage_byte_order": "little-endian 16-bit words",
        "eeprom_default_sha256": hashlib.sha256(storage).hexdigest(),
    }


def build_registry(source_dir: Path = DEFAULT_SOURCE,
                   inventory_path: Path = INVENTORY) -> dict[str, Any]:
    try:
        commit = subprocess.run(["git", "-C", str(source_dir), "rev-parse", "HEAD"],
                                check=True, capture_output=True, text=True).stdout.strip()
        raw = (source_dir / "src/mame" / DRIVER).read_text(encoding="utf-8")
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        raise ProfileError(f"cannot read pinned GQ source/inventory: {exc}") from exc
    if commit != COMMIT or inventory.get("source", {}).get("mame_commit") != COMMIT:
        raise ProfileError("source and inventory must match the pinned MAME commit")
    clean = mame._without_comments(raw)
    matches = list(re.finditer(r"\bGAME\s*\(", clean))
    if len(matches) != 1:
        raise ProfileError("System GQ must contain exactly one GAME declaration")
    arguments, _ = mame._call_arguments(clean, matches[0].end() - 1)
    args = mame._split_arguments(arguments)
    if len(args) != 11 or [a.strip() for a in args[1:7]] != [
            "cryptklr", "0", "konamigq", "konamigq", "konamigq_state", "empty_init"]:
        raise ProfileError("unreviewed System GQ GAME configuration")
    rows = [row for family in inventory.get("systems", []) if family.get("driver") == DRIVER
            for row in family.get("sets", [])]
    description = mame._string_arg(args[9])
    if len(rows) != 1 or rows[0].get("set") != "cryptklr" or rows[0].get("description") != description:
        raise ProfileError("pinned listxml does not match the complete GQ source family")
    romsets, _, coverage = mame.parse_driver(clean, DRIVER)
    if set(romsets) != {"cryptklr"} or len(coverage) != 1 or coverage[0]["status"] != "parsed":
        raise ProfileError("GQ ROM source was not parsed completely")
    romset = romsets["cryptklr"]
    if {tag: region.size for tag, region in romset.regions.items()} != {
            "soundcpu": 0x80000, "k054539": 0x80000, "maincpu:rom": 0x80000}:
        raise ProfileError("reviewed GQ ROM region geometry changed")
    if len(romset.disks) != 1 or romset.disks[0].region != "harddisk" or romset.disks[0].readonly:
        raise ProfileError("GQ requires one source-declared writable hard disk")
    regions = []
    for region in romset.regions.values():
        seen = set()
        files = []
        for op in region.operations:
            if isinstance(op, mame.LoadOp) and op.file_index not in seen:
                seen.add(op.file_index)
                spec = romset.files[op.file_index]
                files.append({"filename": spec.filename, "size": spec.expected_size,
                              "crc32": spec.crc32, "sha1": spec.sha1, "status": spec.status})
        regions.append({"tag": region.tag, "size": region.size, "width": region.width,
                        "endian": region.endian, "files": files,
                        "loads": [{"filename": romset.files[op.file_index].filename,
                                   "offset": op.offset, "length": op.length,
                                   "group": op.group, "stride": op.stride, "swap": op.swap,
                                   "macro": op.macro}
                                  for op in region.operations if isinstance(op, mame.LoadOp)]})
    disk = romset.disks[0]
    profile = {
        "set": "cryptklr", "parent": None, "year": int(args[0]),
        "machine": "konamigq", "state": "konamigq_state", "input": "konamigq",
        "description": description, "manufacturer": mame._string_arg(args[8]),
        "flags": args[10].strip(), "rom_regions": regions,
        "disk_media": [{"name": disk.name, "region": disk.region, "index": disk.index,
                        "sha1": disk.sha1, "readonly": disk.readonly}],
        **source_facts(raw), "game_execution_verified": False,
        "unsupported_board_requirements": ["native GQ adapter", "68000 sound CPU",
                                            "TMS57002 effects DSP", "mounted writable HDD target",
                                            "three lightguns and board I/O"],
    }
    return {"schema_version": 1, "mame_source_commit": COMMIT, "mame_driver": DRIVER,
            "source_sha256": hashlib.sha256(raw.encode()).hexdigest(),
            "family_game_count": 1, "source_complete_rom_start_count": 1,
            "profiles": [profile]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame-source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--inventory", type=Path, default=INVENTORY)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        expected = json.dumps(build_registry(args.mame_source, args.inventory), indent=2) + "\n"
        if args.check:
            if args.output.read_text(encoding="utf-8") != expected:
                raise ProfileError("GQ registry is stale; regenerate it from pinned source")
        else:
            args.output.write_text(expected, encoding="utf-8")
    except (ProfileError, OSError, mame.PreparationError) as exc:
        print(f"konamigq_boards: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
