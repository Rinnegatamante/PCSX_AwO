#!/usr/bin/env python3
"""Verify local System GQ ROM/HDD media and package its source defaults.

No ROMs or CHDs are downloaded. A package is a verified input bundle, not a
claim that the System GQ board or Crypt Killer executes in this project.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Iterable

import prepare_mame_psx as mame
import konamigq_boards as boards

DRIVER = boards.DRIVER
REGION_FILES = {"maincpu:rom": "bios.bin", "soundcpu": "soundcpu.bin",
                "k054539": "pcmrom.bin"}
EEPROM_LICENSE = Path(__file__).with_name("konamigq_LICENSE.txt")
EEPROM_LICENSE_FILE = "default_eeprom_LICENSE.txt"
DISK_NAME = "420uaa04"
PACKAGE_FILES = set(REGION_FILES.values()) | {
    "default_eeprom.bin", EEPROM_LICENSE_FILE, f"{DISK_NAME}.chd"}


class KonamiGQPreparationError(Exception):
    """A local GQ media input or package layout is invalid/unsupported."""


def load_registry(path: Path = boards.DEFAULT_OUTPUT) -> dict[str, Any]:
    try:
        registry = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise KonamiGQPreparationError(f"cannot read System GQ registry {path}: {exc}") from exc
    if not isinstance(registry, dict):
        raise KonamiGQPreparationError("System GQ registry root must be a JSON object")
    profiles = registry.get("profiles")
    if (registry.get("schema_version") != 1 or
            registry.get("mame_source_commit") != mame.PINNED_MAME_COMMIT or
            registry.get("mame_driver") != DRIVER or registry.get("family_game_count") != 1 or
            not isinstance(profiles, list) or len(profiles) != 1 or
            not isinstance(profiles[0], dict) or profiles[0].get("set") != "cryptklr"):
        raise KonamiGQPreparationError("System GQ registry is incomplete or not pinned to MAME")
    _validate_profile(profiles[0])
    return registry


def _source_commit(source: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise KonamiGQPreparationError(f"cannot identify MAME checkout at {source}") from exc


def _disk_geometry(info: str, path: Path) -> dict[str, int]:
    # MAME's harddisk_image_device reads GDDD using
    # HARD_DISK_METADATA_FORMAT = CYLS:%d,HEADS:%d,SECS:%d,BPS:%d.
    # chdman -v prints that CHD metadata in its info report.
    rows = re.findall(r"Metadata:\s+Tag='GDDD'[^\n]*\n\s*CYLS:(\d+),HEADS:(\d+),SECS:(\d+),BPS:(\d+)",
                      info, re.I)
    if len(rows) != 1:
        raise KonamiGQPreparationError(f"{path.name} must contain exactly one recognized GDDD hard-disk geometry")
    cylinders, heads, sectors, bps = map(int, rows[0])
    if min(cylinders, heads, sectors, bps) <= 0:
        raise KonamiGQPreparationError(f"{path.name} contains zero/negative hard-disk geometry")
    logical_match = re.search(r"^\s*Logical size:\s*([0-9][0-9,]*)\s+bytes\s*$", info, re.M | re.I)
    if not logical_match:
        raise KonamiGQPreparationError(f"{path.name} has no CHD logical size")
    logical_size = int(logical_match.group(1).replace(",", ""))
    if cylinders * heads * sectors * bps != logical_size:
        raise KonamiGQPreparationError(
            f"{path.name} GDDD geometry ({cylinders}x{heads}x{sectors}x{bps}) "
            f"does not match its {logical_size}-byte logical size")
    return {"cylinders": cylinders, "heads": heads, "sectors_per_track": sectors,
            "bytes_per_sector": bps, "logical_size": logical_size}


def _verify_chd(path: Path, disk: dict[str, Any], chdman: str | None = None) -> dict[str, Any]:
    tool = chdman or shutil.which("chdman")
    if not tool:
        raise KonamiGQPreparationError("chdman is required to validate CHD header, GDDD geometry, and SHA1")
    try:
        info = subprocess.run([tool, "info", "-i", str(path), "-v"],
                              check=True, capture_output=True, text=True).stdout
        subprocess.run([tool, "verify", "-i", str(path)], check=True,
                       capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = getattr(exc, "stderr", "") or str(exc)
        raise KonamiGQPreparationError(f"chdman rejected {path.name}: {detail.strip()}") from exc

    def field(name: str) -> str | None:
        match = re.search(r"^\s*" + re.escape(name) + r":\s*(\S.*?)\s*$", info, re.M | re.I)
        return match.group(1).strip() if match else None

    version, sha = field("File Version"), field("SHA1")
    if version not in ("3", "4", "5"):
        raise KonamiGQPreparationError(f"{path.name} is not a supported CHD version")
    if not sha or sha.lower() != disk.get("sha1", "").lower():
        raise KonamiGQPreparationError(
            f"{path.name} CHD header SHA1 does not match MAME {disk.get('sha1')}")
    geometry = _disk_geometry(info, path)
    return {"file_version": int(version), "header_sha1": sha.lower(), **geometry}


def _load_regions(set_name: str, source: Path, rompath: Path,
                  archive: Path | None) -> tuple[dict[str, bytes], dict[str, Any]]:
    try:
        romset, parents, _ = mame._find_romset(source, set_name)
        media, origins = mame._load_rom_files(
            romset, rompath, mame._lineage(set_name, parents), None, archive)
        regions = mame.assemble_regions(romset, media)
    except mame.PreparationError as exc:
        raise KonamiGQPreparationError(str(exc)) from exc
    return regions, {"rom_set": romset.name, "parent_chain": mame._lineage(set_name, parents),
                     "rom_files": [{"filename": spec.filename, "size": spec.expected_size,
                                    "crc32": spec.crc32, "sha1": spec.sha1,
                                    "status": spec.status, "provided_by": origins.get(spec.index)}
                                   for spec in romset.files]}


def _disk_file(disk: dict[str, Any], set_name: str, rompath: Path,
               archive: Path | None) -> tuple[Path, bool]:
    name = disk.get("name")
    if not isinstance(name, str) or not name or Path(name).name != name or "/" in name or "\\" in name:
        raise KonamiGQPreparationError("unsafe HDD name in source profile")
    candidates = [rompath / set_name / f"{name}.chd", rompath / f"{name}.chd",
                  rompath / set_name / name, rompath / name]
    for candidate in candidates:
        if candidate.is_file():
            return candidate, False
    if archive and archive.is_file():
        try:
            with zipfile.ZipFile(archive) as zf:
                matches = [entry for entry in zf.namelist()
                           if Path(entry).name.lower() in (f"{name}.chd".lower(), name.lower())]
                if len(matches) > 1:
                    raise KonamiGQPreparationError(f"multiple HDD entries for {name} in archive")
                if matches:
                    fd, tmp_name = tempfile.mkstemp(prefix="system-gq-", suffix=".chd")
                    os.close(fd)
                    extracted = Path(tmp_name)
                    try:
                        with zf.open(matches[0]) as src, extracted.open("wb") as dst:
                            shutil.copyfileobj(src, dst)
                    except Exception:
                        extracted.unlink(missing_ok=True)
                        raise
                    return extracted, True
        except (OSError, zipfile.BadZipFile) as exc:
            raise KonamiGQPreparationError(f"cannot read HDD archive {archive}: {exc}") from exc
    raise KonamiGQPreparationError(f"missing local writable HDD {name}.chd")


def _safe_component(value: Any) -> bool:
    return (isinstance(value, str) and value not in ("", ".", "..") and
            Path(value).name == value and "/" not in value and "\\" not in value)


def _digest(data: bytes) -> dict[str, Any]:
    import zlib
    return {"size": len(data), "crc32": f"{zlib.crc32(data) & 0xffffffff:08x}",
            "sha1": hashlib.sha1(data).hexdigest(), "sha256": hashlib.sha256(data).hexdigest()}


def _default_eeprom(profile: dict[str, Any]) -> bytes:
    words = profile.get("eeprom_default_words")
    if not isinstance(words, list) or len(words) != 64 or any(not isinstance(x, int) or not 0 <= x <= 0xffff for x in words):
        raise KonamiGQPreparationError("GQ source default EEPROM must contain 64 16-bit words")
    data = b"".join(word.to_bytes(2, "little") for word in words)
    if len(data) != profile.get("eeprom_size") or hashlib.sha256(data).hexdigest() != profile.get("eeprom_default_sha256"):
        raise KonamiGQPreparationError("GQ EEPROM defaults do not match pinned source provenance")
    return data


def _eeprom_license() -> bytes:
    try:
        data = EEPROM_LICENSE.read_bytes()
    except OSError as exc:
        raise KonamiGQPreparationError(f"cannot read EEPROM defaults license {EEPROM_LICENSE}: {exc}") from exc
    if b"BSD-3-Clause" not in data or b"scripts/konamigq_boards.json" not in data:
        raise KonamiGQPreparationError("EEPROM defaults license/provenance is not the reviewed BSD notice")
    return data


def _validate_profile(profile: dict[str, Any]) -> None:
    if not isinstance(profile, dict):
        raise KonamiGQPreparationError("System GQ profile must be a JSON object")
    expected_fields = {
        "set": "cryptklr", "parent": None, "year": 1995,
        "machine": "konamigq", "state": "konamigq_state", "input": "konamigq",
        "cpu_device": "CXD8530BQ", "cpu_clock_hz": 67737600,
        "main_ram_size": 0x400000, "gpu_device": "CXD8538Q", "gpu_clock_hz": 53693175,
        "gpu_model": 1, "vram_size": 0x200000, "sound_cpu_device": "M68000",
        "sound_cpu_clock_hz": 8000000, "effects_device": "TMS57002",
        "effects_clock_hz": 24000000, "scsi_device": "NCR53CF96",
        "scsi_clock_hz": 16000000, "scsi_initiator_id": 7, "scsi_target_id": 0,
        "scsi_target_device": "NSCSI_HARDDISK", "eeprom_size": 128,
        "eeprom_device": "93C46 16-bit",
    }
    if any(type(profile.get(key)) is not type(value) or profile.get(key) != value
           for key, value in expected_fields.items()):
        raise KonamiGQPreparationError("GQ profile differs from reviewed source/year/board facts")
    if profile.get("game_execution_verified") is not False:
        raise KonamiGQPreparationError("GQ source profile must retain unverified execution status")
    rows = profile.get("rom_regions")
    if not isinstance(rows, list) or len(rows) != 3 or any(not isinstance(row, dict) for row in rows):
        raise KonamiGQPreparationError("GQ profile ROM regions must be a list of objects")
    if any(not isinstance(row.get("tag"), str) or type(row.get("size")) is not int
           for row in rows):
        raise KonamiGQPreparationError("GQ ROM tags and sizes must be strings and integers")
    region_sizes = {row.get("tag"): row.get("size") for row in rows}
    if region_sizes != {"maincpu:rom": 0x80000, "soundcpu": 0x80000, "k054539": 0x80000}:
        raise KonamiGQPreparationError("GQ ROM region geometry differs from reviewed MAME source")
    disks = profile.get("disk_media")
    if (not isinstance(disks, list) or len(disks) != 1 or not isinstance(disks[0], dict) or
            type(disks[0].get("index")) is not int or disks[0].get("readonly") is not False or
            disks[0] != {"name": DISK_NAME, "region": "harddisk", "index": 0,
                         "sha1": "67cb1418fc0de2a89fc61847dc9efb9f1bebb347", "readonly": False}):
        raise KonamiGQPreparationError("GQ HDD identity/access mode differs from reviewed MAME source")


def package_regions(set_name: str, profile: dict[str, Any], regions: dict[str, bytes],
                    output_dir: Path, source_metadata: dict[str, Any],
                    disk_bytes: bytes, disk_report: dict[str, Any]) -> dict[str, Any]:
    _validate_profile(profile)
    if set_name != profile["set"]:
        raise KonamiGQPreparationError("package set does not match its GQ profile")
    expected = {row["tag"]: row["size"] for row in profile.get("rom_regions", [])}
    if set(expected) != set(REGION_FILES) or set(regions) != set(expected):
        raise KonamiGQPreparationError("GQ ROM region tags differ from source profile")
    if any(len(regions[tag]) != size for tag, size in expected.items()):
        raise KonamiGQPreparationError("GQ assembled ROM region size differs from MAME")
    disks = profile.get("disk_media", [])
    validation = disk_report.get("validation", {})
    geometry_fields = ("cylinders", "heads", "sectors_per_track", "bytes_per_sector", "logical_size")
    geometry = [validation.get(key) for key in geometry_fields]
    if (len(disks) != 1 or disks[0].get("readonly") is not False or
            not _safe_component(disks[0].get("name")) or
            disk_report.get("region") != disks[0].get("region") or
            disk_report.get("index") != disks[0].get("index") or
            disk_report.get("readonly") is not False or
            disk_report.get("package_file") != f"{disks[0]['name']}.chd" or
            validation.get("file_version") not in (3, 4, 5) or
            disk_report.get("name") != disks[0].get("name") or
            disk_report.get("sha1") != disks[0].get("sha1") or
            validation.get("header_sha1") != disks[0].get("sha1") or
            any(type(value) is not int or value <= 0 for value in geometry) or
            geometry[0] * geometry[1] * geometry[2] * geometry[3] != geometry[4] or
            disk_report.get("local_sha256") != hashlib.sha256(disk_bytes).hexdigest()):
        raise KonamiGQPreparationError("GQ writable HDD report/geometry is missing or differs from source media")
    eeprom = _default_eeprom(profile)
    eeprom_license = _eeprom_license()
    payloads = {REGION_FILES[tag]: data for tag, data in regions.items()}
    payloads["default_eeprom.bin"] = eeprom
    payloads[EEPROM_LICENSE_FILE] = eeprom_license
    payloads[f"{disks[0]['name']}.chd"] = disk_bytes

    lines = ["format=pcsx-aow-konamigq-v1", f"mame_set={set_name}",
             f"mame_parent={profile.get('parent') or 'none'}", f"mame_machine={profile['machine']}",
             f"mame_input={profile['input']}", f"mame_state={profile['state']}",
             f"cpu_device={profile['cpu_device']}", f"cpu_clock_hz={profile['cpu_clock_hz']}",
             f"main_ram_size={profile['main_ram_size']}", f"gpu_device={profile['gpu_device']}",
             f"gpu_model={profile['gpu_model']}", f"gpu_clock_hz={profile['gpu_clock_hz']}",
             f"vram_size={profile['vram_size']}", f"sound_cpu_device={profile['sound_cpu_device']}",
             f"sound_cpu_clock_hz={profile['sound_cpu_clock_hz']}",
             f"effects_device={profile['effects_device']}", f"effects_clock_hz={profile['effects_clock_hz']}",
             f"scsi_device={profile['scsi_device']}", f"scsi_clock_hz={profile['scsi_clock_hz']}",
             f"scsi_initiator_id={profile['scsi_initiator_id']}",
             f"scsi_target_id={profile['scsi_target_id']}",
             f"scsi_target_device={profile['scsi_target_device']}",
             f"eeprom_device={profile['eeprom_device']}", "eeprom_default=default_eeprom.bin",
             "eeprom_default_only=true", f"duart_device={profile['duart_device']}",
             f"pcm_device={profile['pcm_device']}", f"pcm_device_count={profile['pcm_device_count']}",
             f"pcm_clock_hz={profile['pcm_clock_hz']}",
             f"pcm_ram_base={profile['pcm_ram_base']}", f"pcm_ram_size={profile['pcm_ram_size']}",
             "game_execution_verified=false"]
    for tag, size in expected.items():
        lines += [f"region.{tag}={REGION_FILES[tag]}", f"region.{tag}.size={size}"]
    lines += [f"disk.harddisk={disks[0]['name']}.chd", f"disk.harddisk.sha1={disks[0]['sha1']}",
              "disk.harddisk.readonly=false", "",]
    board_ini = "\n".join(lines)
    manifest = {"system": "Konami System GQ", "mame_driver": DRIVER,
                "mame_source_commit": boards.COMMIT, "mame_set": set_name,
                "profile": {key: profile[key] for key in (
                    "year", "parent", "machine", "state", "input", "flags", "cpu_device",
                    "cpu_clock_hz", "main_ram_size", "gpu_device", "gpu_model", "gpu_clock_hz",
                    "vram_size", "scsi_device", "scsi_target_id", "eeprom_size",
                    "unsupported_board_requirements", "game_execution_verified")},
                "source_media": source_metadata, "disk_media": [disk_report],
                "eeprom_defaults": {"file": "default_eeprom.bin",
                                    "sha256": hashlib.sha256(eeprom).hexdigest(),
                                    "license_file": EEPROM_LICENSE_FILE,
                                    "license_sha256": hashlib.sha256(eeprom_license).hexdigest(),
                                    "source": "konamigq_def_eeprom in pinned MAME driver"},
                "regions": {}, "game_execution_verified": False}
    for name, data in payloads.items():
        manifest["regions"][name] = _digest(data)

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="prepare-system-gq-", dir=output_dir.parent) as temp:
        stage = Path(temp) / "package"
        stage.mkdir()
        if output_dir.exists():
            if not output_dir.is_dir():
                raise KonamiGQPreparationError("output path exists and is not a directory")
            shutil.copytree(output_dir, stage, dirs_exist_ok=True)
            old_manifest = stage / "manifest.json"
            if old_manifest.exists():
                try:
                    previous = json.loads(old_manifest.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError) as exc:
                    raise KonamiGQPreparationError("existing output manifest is invalid; refusing merge") from exc
                old_regions = previous.get("regions") if isinstance(previous, dict) else None
                if (not isinstance(previous, dict) or previous.get("system") != "Konami System GQ" or
                        previous.get("mame_driver") != DRIVER or
                        previous.get("mame_source_commit") != boards.COMMIT or
                        previous.get("mame_set") != "cryptklr" or
                        not isinstance(old_regions, dict) or not set(old_regions).issubset(PACKAGE_FILES)):
                    raise KonamiGQPreparationError(
                        "existing package manifest is not a valid GQ package; refusing merge")
                for name in old_regions:
                    (stage / name).unlink(missing_ok=True)
        for name, data in payloads.items():
            (stage / name).write_bytes(data)
        (stage / "board.ini").write_text(board_ini, encoding="utf-8")
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        backup = Path(temp) / "previous"
        had_output = output_dir.exists()
        try:
            if had_output:
                os.replace(output_dir, backup)
            os.replace(stage, output_dir)
        except OSError:
            if had_output and backup.exists() and not output_dir.exists():
                os.replace(backup, output_dir)
            raise
    return manifest


def prepare(set_name: str, source: Path, rompath: Path, output_dir: Path,
            archive: Path | None = None, profiles_path: Path = boards.DEFAULT_OUTPUT,
            chdman: str | None = None) -> dict[str, Any]:
    registry = load_registry(profiles_path)
    if _source_commit(source) != registry["mame_source_commit"]:
        raise KonamiGQPreparationError("MAME checkout differs from the pinned registry")
    profile = registry["profiles"][0]
    if set_name != profile["set"]:
        raise KonamiGQPreparationError(f"no System GQ profile for {set_name}")
    fresh = boards.build_registry(source, boards.INVENTORY)
    if fresh != registry:
        raise KonamiGQPreparationError("GQ registry differs from pinned source/listxml facts")
    regions, source_metadata = _load_regions(set_name, source, rompath, archive)
    if set(regions) != {row["tag"] for row in profile["rom_regions"]}:
        raise KonamiGQPreparationError("assembled GQ ROM tags differ from the reviewed profile")
    disk = profile["disk_media"][0]
    disk_path, temporary = _disk_file(disk, set_name, rompath, archive)
    try:
        validation = _verify_chd(disk_path, disk, chdman)
        disk_bytes = disk_path.read_bytes()
    finally:
        if temporary:
            disk_path.unlink(missing_ok=True)
    disk_report = {**disk, "package_file": f"{disk['name']}.chd", "validation": validation,
                   "local_sha256": hashlib.sha256(disk_bytes).hexdigest()}
    return package_regions(set_name, profile, regions, output_dir,
                           source_metadata, disk_bytes, disk_report)


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--set", required=True, dest="set_name")
    parser.add_argument("--mame-source", required=True, type=Path)
    parser.add_argument("--rompath", required=True, type=Path)
    parser.add_argument("--archive", type=Path, help="optional local ZIP with ROM and/or HDD files")
    parser.add_argument("--profiles", type=Path, default=boards.DEFAULT_OUTPUT)
    parser.add_argument("--chdman", help="path to chdman (defaults to PATH lookup)")
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts/system-gq"))
    args = parser.parse_args(argv)
    try:
        manifest = prepare(args.set_name, args.mame_source, args.rompath,
                           args.output_dir, args.archive, args.profiles, args.chdman)
    except (KonamiGQPreparationError, boards.ProfileError,
            mame.PreparationError, OSError) as exc:
        print(f"prepare_konamigq_board: {exc}", file=sys.stderr)
        return 2
    print(f"prepared {args.set_name}: {len(manifest['regions'])} verified files; game execution remains unverified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
