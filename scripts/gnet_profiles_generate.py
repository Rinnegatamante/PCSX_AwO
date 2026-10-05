#!/usr/bin/env python3
"""Generate source-faithful Taito G-Net media profiles from pinned MAME source."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_REL = Path("../mamedev/src/mame/sony/taitogn.cpp")
INVENTORY_REL = Path("scripts/mame_psx_systems.json")
OUTPUT_REL = Path("libpcsxcore/arcade/gnet_profiles.inc")
DOC_REL = Path("docs/gnet-media-profiles.md")
MAME_COMMIT = "deb2f54fe7300e7a192690318beb860159bfd52a"
SOURCE_SHA256 = "3d431ab3480c5a2dfd498a7cb3095f9800027e486d0a51203a913ab91d556e8c"
DRIVER = "sony/taitogn.cpp"


class ProfileError(ValueError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def cstr(value: str | None) -> str:
    if value is None:
        return "NULL"
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def split_args(text: str) -> list[str]:
    """Split a C macro argument list, respecting quoted strings and nesting."""
    out, current = [], []
    quote = False
    escape = False
    depth = 0
    for char in text:
        if quote:
            current.append(char)
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == '"':
                quote = False
        elif char == '"':
            quote = True
            current.append(char)
        elif char in "({[":
            depth += 1
            current.append(char)
        elif char in ")}]":
            depth -= 1
            current.append(char)
        elif char == "," and depth == 0:
            out.append("".join(current).strip())
            current = []
        else:
            current.append(char)
    if quote or depth:
        raise ProfileError("unbalanced MAME macro arguments")
    out.append("".join(current).strip())
    return out


def macro_calls(text: str, name: str) -> list[list[str]]:
    result = []
    pattern = re.compile(r"\b" + re.escape(name) + r"\s*\(")
    for match in pattern.finditer(text):
        start = match.end()
        depth, quote, escape = 1, False, False
        end = start
        while end < len(text) and depth:
            ch = text[end]
            if quote:
                if escape:
                    escape = False
                elif ch == "\\":
                    escape = True
                elif ch == '"':
                    quote = False
            elif ch == '"':
                quote = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            end += 1
        if depth:
            raise ProfileError(f"unterminated {name} macro")
        result.append(split_args(text[start:end - 1]))
    return result


def unquote(value: str) -> str:
    if len(value) < 2 or value[0] != '"' or value[-1] != '"':
        raise ProfileError(f"expected C string, got {value!r}")
    return bytes(value[1:-1], "utf-8").decode("unicode_escape")


def macro_region(source: str, macro: str) -> str:
    match = re.search(r"#define\s+" + macro + r"\s+\\\n(?P<body>.*?)(?=\n#define\s|\nROM_START\s*\()", source, re.S)
    if not match:
        raise ProfileError(f"missing {macro} macro")
    return match.group("body")


def parse_loads(body: str) -> dict[str, dict[str, str]]:
    loads = {}
    for args in macro_calls(body, "ROM_LOAD") + macro_calls(body, "ROMX_LOAD"):
        name = unquote(args[0])
        crc_match = re.search(r"CRC\(([0-9a-fA-F]+)\)", args[-1])
        sha_match = re.search(r"SHA1\(([0-9a-fA-F]+)\)", args[-1])
        if crc_match and sha_match:
            loads[name] = {"crc": crc_match.group(1).lower(), "sha1": sha_match.group(1).lower()}
    return loads


def bios_data(source: str) -> tuple[dict[str, dict], list[dict]]:
    roots = {}
    for macro, set_name, model in (("COH3002T_BIOS", "coh3002t", "COH-3002T"),
                                   ("COH1002T_BIOS", "coh1002t", "COH-1002T")):
        body = macro_region(source, macro)
        loads = parse_loads(body)
        main = re.search(r'ROM_LOAD\(\s*"([^"]+)"[^\n]*CRC\(([0-9a-f]+)\) SHA1\(([0-9a-f]+)\)', body)
        flash_region = re.search(r'ROM_REGION16_LE\(\s*0x200000,\s*"firm"', body)
        flash = re.search(r'ROM_LOAD\(\s*"([^"]+)"[^\n]*CRC\(([0-9a-f]+)\) SHA1\(([0-9a-f]+)\)', body[flash_region.start():]) if flash_region else None
        if not main or not flash:
            raise ProfileError(f"incomplete BIOS macro {macro}")
        cat1 = re.search(r'ROM_LOAD\(\s*"([^"]+)"[^\n]*CRC\(([0-9a-f]+)\) SHA1\(([0-9a-f]+)\)', body[body.find('"cat702_1"'):])
        cat2 = re.search(r'ROM_LOAD\(\s*"([^"]+)"[^\n]*CRC\(([0-9a-f]+)\) SHA1\(([0-9a-f]+)\)', body[body.find('"cat702_2"'):])
        if not cat1 or not cat2:
            raise ProfileError(f"missing CAT702 dump in {macro}")
        bios = {"set": set_name, "model": model, "macro": macro,
                "cpu_file": main.group(1), "cpu_crc": main.group(2).lower(), "cpu_sha1": main.group(3).lower(),
                "flash_file": flash.group(1), "flash_crc": flash.group(2).lower(), "flash_sha1": flash.group(3).lower(),
                "cat702_1_file": cat1.group(1), "cat702_1_crc": cat1.group(2), "cat702_1_sha1": cat1.group(3),
                "cat702_2_file": cat2.group(1), "cat702_2_crc": cat2.group(2), "cat702_2_sha1": cat2.group(3)}
        if set_name == "coh3002t":
            options = []
            for args in macro_calls(body, "ROMX_LOAD"):
                tail = ",".join(args)
                filename = unquote(args[0])
                option = re.search(r'ROM_BIOS\((\d+)\)', tail)
                crc_match = re.search(r"CRC\(([0-9a-f]+)\)", tail)
                sha_match = re.search(r"SHA1\(([0-9a-f]+)\)", tail)
                if option and crc_match and sha_match:
                    opts = re.findall(r'ROM_SYSTEM_BIOS\(\s*(\d+),\s*"([^"]+)"', body)
                    opt_no = option.group(1)
                    bios_name = next((name for number, name in opts if number == opt_no), None)
                    if bios_name is None:
                        raise ProfileError(f"unknown BIOS option index {opt_no}")
                    options.append({"name": bios_name, "file": filename,
                                    "crc": crc_match.group(1), "sha1": sha_match.group(1)})
            bios["options"] = options
        else:
            bios["options"] = []
        roots[set_name] = bios
    return roots, []


def parse_rom_blocks(source: str) -> dict[str, dict]:
    blocks = {}
    for match in re.finditer(r"ROM_START\(\s*(\w+)\s*\)(.*?)ROM_END", source, re.S):
        name, body = match.group(1), match.group(2)
        if name in blocks:
            raise ProfileError(f"duplicate ROM_START {name}")
        disk = re.search(r'DISK_REGION\(\s*"pccard:([^"]+)"\s*\)', body)
        image = re.search(r'DISK_IMAGE\(\s*"([^"]+)"\s*,\s*\d+\s*,\s*SHA1\(([0-9a-fA-F]{40})\)\s*\)', body)
        default = re.search(r'ROM_DEFAULT_BIOS\(\s*"([^"]+)"\s*\)', body)
        bios_macro = "COH1002T_BIOS" if re.search(r"\bCOH1002T_BIOS\b", body) else "COH3002T_BIOS" if re.search(r"\bCOH3002T_BIOS\b", body) else None
        if not bios_macro:
            raise ProfileError(f"ROM_START {name} does not select a known BIOS macro")
        blocks[name] = {"bios_macro": bios_macro,
                        "card": disk.group(1) if disk else None,
                        "chd": image.group(1) if image else None,
                        "sha1": image.group(2).lower() if image else None,
                        "bios_default": default.group(1) if default else ("v1" if bios_macro == "COH3002T_BIOS" else "fixed")}
        if bool(disk) != bool(image):
            raise ProfileError(f"ROM_START {name}: card region and DISK_IMAGE must occur together")
    return blocks


def parse_games_simple(source: str) -> dict[str, dict]:
    """Parse MAME GAME rows (macro has 11 arguments in taitogn.cpp)."""
    games = {}
    for args in macro_calls(source, "GAME"):
        if len(args) != 11:
            raise ProfileError(f"unexpected GAME arity {len(args)}: {args[:2]}")
        year = int(args[0])
        set_name, parent, machine, inputs, state, init, rotation = args[1:8]
        manufacturer, description, flags = unquote(args[8]), unquote(args[9]), args[10]
        if set_name in games:
            raise ProfileError(f"duplicate GAME {set_name}")
        games[set_name] = {"year": year, "parent": None if parent in ("0", "nullptr") else parent,
                           "machine": machine, "input": inputs, "state": state,
                           "init": init, "rotation": rotation, "manufacturer": manufacturer,
                           "description": description, "flags": flags}
    return games


def registry(source: str, inventory: dict) -> dict:
    source_hash = sha256(source.encode())
    if source_hash != SOURCE_SHA256:
        raise ProfileError(f"taitogn.cpp SHA256 mismatch: expected {SOURCE_SHA256}, got {source_hash}")
    if inventory.get("source", {}).get("mame_commit") != MAME_COMMIT:
        raise ProfileError("inventory MAME commit does not match the pinned source")
    system = next((item for item in inventory.get("systems", []) if item.get("driver") == DRIVER), None)
    if not system or system.get("set_count") != 41:
        raise ProfileError("inventory does not contain pinned 41-set Taito G-Net selection")
    inv_sets = {s["set"]: s for s in system["sets"]}
    blocks, games = parse_rom_blocks(source), parse_games_simple(source)
    if set(inv_sets) != set(games) or set(games) != set(blocks) or len(games) != 41:
        raise ProfileError(f"MAME/inventory set mismatch (inventory={len(inv_sets)}, GAME={len(games)}, ROM_START={len(blocks)})")
    bios, _ = bios_data(source)
    rows = []
    for set_name in sorted(games):
        game, rom, inv = games[set_name], blocks[set_name], inv_sets[set_name]
        # listxml descriptions can normalize source punctuation/capitalization;
        # the registry deliberately preserves the literal taitogn.cpp title.
        if game["parent"] and game["parent"] not in inv_sets:
            raise ProfileError(f"MAME parent is absent from inventory for {set_name}: {game['parent']}")
        root = "coh1002t" if rom["bios_macro"] == "COH1002T_BIOS" else "coh3002t"
        inventory_root = set_name if inv["isbios"] else inv["romof"]
        seen = set()
        while inventory_root in inv_sets and not inv_sets[inventory_root]["isbios"]:
            if inventory_root in seen:
                raise ProfileError(f"inventory parent cycle at {set_name}")
            seen.add(inventory_root)
            inventory_root = inv_sets[inventory_root]["romof"]
        if inventory_root != root:
            raise ProfileError(f"BIOS parent mismatch for {set_name}: {inventory_root} != {root}")
        if (rom["chd"] is None) != bool(inv["isbios"]):
            raise ProfileError(f"CHD/BIOS root mismatch for {set_name}")
        if rom["card"] and rom["card"] not in {"taitopccard1", "taitopccard2", "taitocf", "ataflash"}:
            raise ProfileError(f"unknown MAME card option for {set_name}: {rom['card']}")
        if rom["bios_macro"] == "COH1002T_BIOS" and rom["bios_default"] != "fixed":
            raise ProfileError(f"unexpected selectable BIOS on {set_name}")
        rows.append({**game, **rom, "set": set_name, "bios_root": root,
                     "bios_model": bios[root]["model"],
                     "card_kind": {"taitopccard1": "TYPE1", "taitopccard2": "TYPE2",
                                   "taitocf": "CF", "ataflash": "ATAFLASH"}.get(rom["card"], "NONE"),
                     "metadata_policy": ("CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512"
                                         if rom["card"] in ("taitopccard1", "taitopccard2", "taitocf") else
                                         "CIS_DEFAULT_IF_MISSING;KEY_UNUSED;IDNT_OPTIONAL_RAW_512"
                                         if rom["card"] == "ataflash" else "NONE")})
    return {"source_sha256": source_hash, "mame_commit": MAME_COMMIT,
            "bios": bios, "profiles": rows}


def emit(data: dict) -> str:
    lines = ["/* Generated by scripts/gnet_profiles_generate.py; do not edit.",
             f" * MAME {data['mame_commit']} taitogn.cpp SHA256 {data['source_sha256']}",
             " * Define GNET_BIOS_ROOT, GNET_BIOS_OPTION, GNET_METADATA_TAGS and GNET_PROFILE before inclusion.",
             " * Metadata tags are raw CHD tags; IDNT length is validated by the ATA backend.",
             " */"]
    for key in ("coh1002t", "coh3002t"):
        bios = data["bios"][key]
        lines.append("GNET_BIOS_ROOT(%s)" % ", ".join((
            cstr(key), cstr(bios["model"]), cstr(bios["macro"]), cstr(bios["cpu_file"]),
            cstr(bios["cpu_crc"]), cstr(bios["cpu_sha1"]), cstr(bios["flash_file"]),
            cstr(bios["flash_crc"]), cstr(bios["flash_sha1"]),
            cstr(bios["cat702_1_file"]), cstr(bios["cat702_1_crc"]), cstr(bios["cat702_1_sha1"]),
            cstr(bios["cat702_2_file"]), cstr(bios["cat702_2_crc"]), cstr(bios["cat702_2_sha1"]))))
        for option in bios["options"]:
            lines.append("GNET_BIOS_OPTION(%s, %s, %s, %s, %s)" % (
                cstr(key), cstr(option["name"]), cstr(option["file"]),
                cstr(option["crc"]), cstr(option["sha1"])))
    lines.append("GNET_METADATA_TAGS(%s, %s, %s)" % (cstr("CIS "), cstr("KEY "), cstr("IDNT")))
    for row in data["profiles"]:
        lines.append("GNET_PROFILE(%s)" % ", ".join((
            cstr(row["set"]), str(row["year"]), cstr(row["description"]),
            cstr(row["parent"]), cstr(row["bios_root"]), cstr(row["bios_model"]), cstr(row["bios_default"]),
            cstr(row["machine"]), cstr(row["input"]), cstr(row["state"]),
            cstr(row["init"]), cstr(row["rotation"]), cstr(row["manufacturer"]),
            row["flags"], cstr(row["card"]), cstr(row["card_kind"]),
            cstr(row["chd"]), cstr(row["sha1"]), cstr(row["metadata_policy"]))))
    return "\n".join(lines) + "\n"


def emit_docs(data: dict) -> str:
    lines = [
        "# Taito G-Net media profiles",
        "",
        "This registry records MAME source metadata for all 41 `taitogn.cpp` sets, including the two BIOS roots and 39 game entries. It is data for a future loader integration; it does not implement BIOS selection, input variants, game-specific I/O, boot, or media loading.",
        "",
        f"Source pin: MAME `{data['mame_commit']}`, `src/mame/sony/taitogn.cpp` SHA-256 `{data['source_sha256']}`. `scripts/gnet_profiles_generate.py --check` validates the pin, set inventory, BIOS ancestry, source macros, and deterministic generated outputs.",
        "",
        "## Card and metadata semantics",
        "",
        "The PC Card option comes from each `DISK_REGION` and the MAME machine configuration. `taitopccard1`, `taitopccard2`, and `taitocf` select the Taito Type 1, Type 2, and CompactFlash devices. Bootleg profiles use generic `ataflash`. MAME CHD tags are `CIS ` (CIS bytes), `KEY ` (card unlock key), and `IDNT` (ATA IDENTIFY). In `src/devices/bus/pccard/ataflash.cpp`, MAME pads a supplied CIS to 512 bytes and synthesizes its default CIS when absent; the three Taito card variants enter their locked state only for an exact five-byte KEY. Thus `KEY_OPTIONAL_EXACT5` records source behavior, not a claim that every disk has a KEY. Generic `ataflash` does not use the Taito key protocol. `IDNT` is raw ATA IDENTIFY metadata; a consumer must validate its 512-byte length. The registry does not assert that each CHD actually contains these tags.",
        "",
        "## Profiles",
        "",
        "`CHD SHA1` is the exact SHA-1 declared by MAME's `DISK_IMAGE`. BIOS is the per-ROM default, with `v1` the source's first/default option when not overridden; `fixed` denotes the COH-1002T ROM with no selectable EPROM BIOS. `GAME flags`, machine configuration, and input configuration are source fields and are recorded for future implementation; their runtime behavior is not claimed here.",
        "",
        "| Set | BIOS model / option | Machine config | Input config | Card | CHD name | CHD SHA1 | Metadata policy | Flags |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for row in data["profiles"]:
        if row["chd"]:
            cols = [row["set"], f"{row['bios_root']} ({row['bios_model']}) / {row['bios_default']}", row["machine"], row["input"],
                    row["card_kind"], row["chd"], row["sha1"], row["metadata_policy"], row["flags"]]
            lines.append("| " + " | ".join(str(col).replace("|", "\\|") for col in cols) + " |")
    lines += [
        "",
        "## BIOS roots and dumps",
        "",
        "| Root/model | Main BIOS ROM | CRC32 | SHA-1 | CAT702 1 | CAT702 2 | Firmware flash | CRC32 | SHA-1 | Selectable EPROM BIOSes |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for key in ("coh1002t", "coh3002t"):
        b = data["bios"][key]
        option_names = ", ".join(o["name"] for o in b["options"]) or "none (fixed EPROM fill)"
        cols = [f"{key} ({b['model']})", b["cpu_file"], b["cpu_crc"], b["cpu_sha1"],
                f"{b['cat702_1_file']} ({b['cat702_1_sha1']})",
                f"{b['cat702_2_file']} ({b['cat702_2_sha1']})",
                b["flash_file"], b["flash_crc"], b["flash_sha1"], option_names]
        lines.append("| " + " | ".join(cols) + " |")
    lines += ["", "| BIOS option (COH-3002T) | ROM file | CRC32 | SHA-1 |", "|---|---|---|---|"]
    for option in data["bios"]["coh3002t"]["options"]:
        lines.append("| %s | %s | %s | %s |" % (option["name"], option["file"], option["crc"], option["sha1"]))
    lines += [
        "",
        "The COH-1002T BIOS macro marks its uPD78081 MCU dump `NO_DUMP`; COH-3002T does too. Both BIOS roots share the same raw PC Card metadata tags at the driver level, but each root has its own CPU BIOS, CAT702 dumps, and firmware flash image.",
        "",
        "## Local availability snapshot",
        "",
        "On 2026-10-05, no G-Net BIOS archives, EPROM files, or game CHDs were found in the workspace `roms/`, the adjacent MAME checkout's `roms/`, `~/.mame/roms/`, or `~/Library/Application Support/mame/roms/`. The workspace `roms/` contained `soulclbr.zip`; the MAME ROM directory contained only `dir.txt` and an unrelated HD44780 ROM. No media was downloaded. This is a local snapshot, not a statement about media availability elsewhere.",
        "",
        "## Loader boundary",
        "",
        "No `board.ini`/native loader profile is added here. Runtime boot still requires BIOS/firmware loading, the real PC Card behavior, CHD CIS/KEY/IDNT consumption, ATA taskfile/media integration, and per-game input/config behavior. Profile completeness is source metadata coverage only; no G-Net boot claim follows from this registry.",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / SOURCE_REL)
    parser.add_argument("--inventory", type=Path, default=ROOT / INVENTORY_REL)
    parser.add_argument("--output", type=Path, default=ROOT / OUTPUT_REL)
    parser.add_argument("--docs-output", type=Path, default=ROOT / DOC_REL)
    parser.add_argument("--check", action="store_true", help="fail if output differs from deterministic regeneration")
    args = parser.parse_args()
    try:
        source = args.source.read_text(encoding="utf-8")
        inventory = json.loads(args.inventory.read_text(encoding="utf-8"))
        data = registry(source, inventory)
        result = emit(data)
        docs = emit_docs(data)
    except (OSError, json.JSONDecodeError, ProfileError) as exc:
        print(f"gnet profiles: {exc}", file=sys.stderr)
        return 2
    if args.check:
        try:
            existing = args.output.read_text(encoding="utf-8")
            existing_docs = args.docs_output.read_text(encoding="utf-8")
        except OSError as exc:
            print(f"gnet profiles: cannot read output: {exc}", file=sys.stderr)
            return 2
        if existing != result or existing_docs != docs:
            print("gnet profiles: generated output/docs differ", file=sys.stderr)
            return 1
        print(f"G-Net source registry verified: 41 sets; {sha256(result.encode())}")
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result, encoding="utf-8")
    args.docs_output.parent.mkdir(parents=True, exist_ok=True)
    args.docs_output.write_text(docs, encoding="utf-8")
    print(f"wrote {args.output} and {args.docs_output} ({len(data['profiles'])} profiles)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
