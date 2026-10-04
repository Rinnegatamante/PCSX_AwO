#!/usr/bin/env python3
"""Generate the native System 10 profile include from the pinned registry."""
from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "scripts/system10_boards.json"
DEFAULT_MAME = Path(__file__).resolve().parents[2] / "mamedev"
OUTPUT = ROOT / "libpcsxcore/arcade/system10_profiles.inc"
PINNED = "deb2f54fe7300e7a192690318beb860159bfd52a"


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def source_nand(machine: str, source: str, board_type: str,
                region_count: int) -> tuple[str, int]:
    if board_type != "MEM(N)":
        return "NONE", 0
    suffix = machine.removeprefix("ns10_")
    match = re.search(
        rf"void\s+namcos10_memn_state::{re.escape('ns10_' + suffix)}\s*\([^)]*\)\s*\{{(.*?)\n\}}",
        source, re.S)
    if not match:
        raise ValueError(f"cannot find MEM(N) machine configuration {machine}")
    body = match.group(1)
    devices = re.findall(r"namcos10_nand_(k9f2808u0b|k9f5608u0d)\s*\(\s*config\s*,\s*(\d+)\s*\)", body)
    if len(devices) != 1:
        raise ValueError(f"unsupported/ambiguous NAND geometry for {machine}")
    nand_type, count_text = devices[0]
    count = int(count_text)
    if count != region_count:
        raise ValueError(f"NAND count mismatch for {machine}: source {count}, regions {region_count}")
    return nand_type.upper(), count


def build(source_dir: Path = DEFAULT_MAME) -> str:
    result = subprocess.run(["git", "-C", str(source_dir), "rev-parse", "HEAD"],
                            check=True, capture_output=True, text=True)
    if result.stdout.strip() != PINNED:
        raise ValueError("MAME checkout differs from the pinned System 10 source revision")
    doc = json.loads(REGISTRY.read_text(encoding="utf-8"))
    if (doc.get("schema_version") != 1 or doc.get("mame_source_commit") != PINNED or
            doc.get("source") != "namco/namcos10.cpp" or doc.get("set_count") != 45 or
            not isinstance(doc.get("profiles"), list) or len(doc["profiles"]) != 45):
        raise ValueError("System 10 registry is incomplete or does not match the pinned source")
    source = (source_dir / "src/mame/namco/namcos10.cpp").read_text(encoding="utf-8")
    rows = []
    names = set()
    for profile in doc["profiles"]:
        name = profile["set"]
        if name in names:
            raise ValueError(f"duplicate System 10 profile {name}")
        names.add(name)
        board_type = profile["board_type"]
        if board_type not in ("MEM(M)", "MEM(N)", "MEM(P3)", "MEM/IO"):
            raise ValueError(f"unknown board type for {name}: {board_type}")
        regions = profile.get("rom_regions", [])
        region_count = sum(r["tag"].startswith("nand") and r["tag"][4:].isdigit()
                           for r in regions)
        nand_type, nand_count = source_nand(profile["machine"], source,
                                             board_type, region_count)
        region_parts = []
        for region in regions:
            tag, size = region["tag"], region["size"]
            if tag == "maincpu:rom":
                filename = "main.bin"
            elif tag == "nand":
                filename = "rawflash.bin"
            elif tag == "data":
                filename = "datarom.bin"
            elif re.fullmatch(r"nand\d+", tag):
                filename = f"raw{tag}.bin"
            else:
                filename = "unsupported.bin"
            region_parts.append(f"{tag}|{size}|{filename}")
        values = [
            name, profile.get("parent") or "none", board_type,
            profile.get("crypto_profile") or "none", profile["machine"],
            profile["input"], profile["state"], profile["init"], profile["flags"],
            "|".join(profile.get("extensions", [])),
            "|".join(profile.get("no_dump_files", [])),
            "|".join(profile.get("unsupported_requirements", [])),
            "true" if profile.get("runtime_candidate") else "false",
            ";".join(region_parts), nand_type, str(nand_count),
        ]
        rows.append("{" + ", ".join(c_string(v) for v in values) + "},")
    if len(names) != 45:
        raise ValueError("registry does not contain 45 distinct profiles")
    return "/* Generated from scripts/system10_boards.json; do not edit. */\n" + "\n".join(rows) + "\n"


if __name__ == "__main__":
    OUTPUT.write_text(build(), encoding="ascii")
    print(f"wrote {OUTPUT}")
