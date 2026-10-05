#!/usr/bin/env python3
"""Regenerate the standalone ATA source from the pinned local MAME checkout."""
from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path

PIN = "deb2f54fe7300e7a192690318beb860159bfd52a"
SOURCES = {
    "atahle.h": "e95c1b5dcf404d8aa2f1bd06cecb7b2921204cceddc236fe122c5a08097f780f",
    "atahle.cpp": "ef9335fccfa0bd3250170dd5771013fc2ce42dd0ef92197af8194717508f65a8",
    "atastorage.h": "34a0b8688555bbb190b2af8dd451a6aadefd631a1bcfbcfcb2cca72be3919336",
    "atastorage.cpp": "a56df2bf862ae95a89f8a44373baa49d56477b5afe7b37c411e075956ea59ac7",
}


def fail(message: str) -> None:
    raise SystemExit(message)


def replace_once(source: str, old: str, new: str, label: str) -> str:
    count = source.count(old)
    if count != 1:
        fail(f"expected exactly one {label} anchor, found {count}")
    return source.replace(old, new, 1)


def source_at(repo: Path, filename: str) -> str:
    path = f"src/devices/machine/{filename}"
    raw = subprocess.check_output(["git", "-C", str(repo), "show", f"{PIN}:{path}"])
    actual = hashlib.sha256(raw).hexdigest()
    if actual != SOURCES[filename]:
        fail(f"pinned source hash mismatch for {path}: {actual}")
    return raw.decode("utf-8")


def import_header(source: str, filename: str) -> str:
    if filename == "atahle.h":
        source = source.replace("#ifndef MAME_MACHINE_ATAHLE_H", "#ifndef PCSX_ATAHLE_PORT_H")
        source = source.replace("#define MAME_MACHINE_ATAHLE_H", "#define PCSX_ATAHLE_PORT_H")
        source = replace_once(source,
            "ata_hle_device_base(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock);",
            "ata_hle_device_base();", "ATA HLE constructor")
        source = source.replace("#endif // MAME_MACHINE_ATAHLE_H", "#endif // PCSX_ATAHLE_PORT_H")
        source = source.replace("#pragma once", "#pragma once\n\n#include \"ata_mame_compat.h\"\n#include <vector>")
        return source

    source = source.replace("#ifndef MAME_MACHINE_ATASTORAGE_H", "#ifndef PCSX_ATASTORAGE_PORT_H")
    source = source.replace("#define MAME_MACHINE_ATASTORAGE_H", "#define PCSX_ATASTORAGE_PORT_H")
    source = source.replace('#include "atahle.h"', '#include "atahle_port.h"')
    source = source.replace("#include \"imagedev/harddriv.h\"", "")
    source = source.replace("#include \"harddisk.h\"", "")
    source = replace_once(source,
        "ata_mass_storage_device_base(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock);",
        "ata_mass_storage_device_base();", "mass-storage constructor")
    source = replace_once(source,
        "class ide_hdd_device_base : public ata_mass_storage_device_base",
        "class ide_hdd_device_base : public ata_mass_storage_device_base", "IDE HDD wrapper")
    source = replace_once(source,
        "ide_hdd_device_base(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock);",
        "ide_hdd_device_base();", "IDE HDD constructor")
    source = source.replace("\tvirtual void device_add_mconfig(machine_config &config) override ATTR_COLD;\n", "")
    source = source.replace("\tvirtual int read_sector(uint32_t lba, void *buffer) override { return !m_image->exists() ? 0 : m_image->read(lba, buffer); }\n", "")
    source = source.replace("\tvirtual int write_sector(uint32_t lba, const void *buffer) override { return !m_image->exists() ? 0 : m_image->write(lba, buffer); }\n", "")
    source = replace_once(source, "\trequired_device<harddisk_image_device> m_image;\n", "", "IDE HDD image member")
    source = replace_once(source,
        "cf_device_base(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock);",
        "cf_device_base();", "CompactFlash constructor")
    source = source.replace("\tvirtual void device_add_mconfig(machine_config &config) override ATTR_COLD;\n", "")
    source = source.replace("#endif // MAME_MACHINE_ATASTORAGE_H", "#endif // PCSX_ATASTORAGE_PORT_H")
    source = source.replace("#pragma once", "#pragma once\n\n#include \"ata_mame_compat.h\"")
    return source


def import_cpp(source: str, filename: str) -> str:
    source = source.replace('#include "emu.h"', '#include "ata_mame_compat.h"')
    source = source.replace('#include "atahle.h"', '#include "atahle_port.h"')
    source = source.replace('#include "atastorage.h"', '#include "atastorage_port.h"')
    source = source.replace('#include "multibyte.h"', '#include "ata_mame_compat.h"')
    source = source.replace('#include "logmacro.h"', '')
    if filename == "atahle.cpp":
        start = source.index("ata_hle_device_base::ata_hle_device_base(")
        end = source.index("void ata_hle_device_base::device_start()", start)
        constructor = """ata_hle_device_base::ata_hle_device_base() :
\tdevice_t(),
\tm_buffer_offset(0),
\tm_buffer_size(0),
\tm_error(0),
\tm_feature(0),
\tm_sector_count(0),
\tm_sector_number(0),
\tm_cylinder_low(0),
\tm_cylinder_high(0),
\tm_device_head(0),
\tm_status(0),
\tm_command(0),
\tm_device_control(0),
\tm_revert_to_defaults(true),
\tm_8bit_data_transfers(false),
\tm_csel(0),
\tm_daspin(0),
\tm_daspout(0),
\tm_dmack(0),
\tm_dmarq(0),
\tm_irq(0),
\tm_pdiagin(0),
\tm_pdiagout(0),
\tm_single_device(0),
\tm_resetting(0), m_busy_timer(nullptr), m_buffer_empty_timer(nullptr)
{
}

"""
        source = source[:start] + constructor + source[end:]
        return source

    start = source.index("ata_mass_storage_device_base::ata_mass_storage_device_base(")
    end = source.index("/*************************************", start)
    ctor = """ata_mass_storage_device_base::ata_mass_storage_device_base() :
\tata_hle_device_base(),
\tm_can_identify_device(0),
\tm_num_cylinders(0),
\tm_num_sectors(0),
\tm_num_heads(0), m_cur_lba(0), m_block_count(0), m_sectors_until_int(0), m_master_password_enable(0), m_user_password_enable(0),
\tm_master_password(nullptr),
\tm_user_password(nullptr),
\tm_dma_transfer_time(attotime::zero)
{
}

"""
    source = source[:start] + ctor + source[end:]
    # Keep the source IDE-HDD class boundary and status algorithm, replacing
    # only image-backed constructor/reset/configuration with callback plumbing.
    old_ctor = """ide_hdd_device_base::ide_hdd_device_base(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock) :
\tata_mass_storage_device_base(mconfig, type, tag, owner, clock),
\tm_image(*this, \"image\")
{
}
"""
    new_ctor = """ide_hdd_device_base::ide_hdd_device_base() :
\tata_mass_storage_device_base(),
\tm_last_status_timer(nullptr)
{
}
"""
    source = replace_once(source, old_ctor, new_ctor, "IDE HDD constructor body")
    source = replace_once(source,
        "m_last_status_timer = machine().scheduler().timer_alloc(timer_expired_delegate());",
        "m_last_status_timer = timer_alloc(timer_expired_delegate());",
        "IDE HDD status timer allocation")
    reset_start = source.index("void ide_hdd_device_base::device_reset()")
    reset_end = source.index("uint8_t ide_hdd_device_base::calculate_status()", reset_start)
    reset = """void ide_hdd_device_base::device_reset()
{
\tata_mass_storage_device_base::device_reset();
}

"""
    source = source[:reset_start] + reset + source[reset_end:]
    config_start = source.index("void ide_hdd_device_base::device_add_mconfig(")
    config_end = source.index("//**************************************************************************\n//  ATA COMPACTFLASH CARD DEVICE", config_start)
    source = source[:config_start] + source[config_end:]
    source = replace_once(source,
        "cf_device_base::cf_device_base(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock) :\n\tide_hdd_device_base(mconfig, type, tag, owner, clock)",
        "cf_device_base::cf_device_base() :\n\tide_hdd_device_base()", "CompactFlash constructor")
    start = source.index("//-------------------------------------------------\n//  device_add_mconfig", source.index("cf_device_base::cf_device_base"))
    end = source.index("//-------------------------------------------------\n//  ide_build_identify_device", start)
    source = source[:start] + source[end:]
    return source


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-repo", type=Path, default=Path("../mamedev"))
    parser.add_argument("--output", type=Path, default=Path("libpcsxcore/arcade/ata_mame"))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    repo = args.source_repo.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    imported = {name: source_at(repo, name) for name in SOURCES}
    generated = {
        "atahle_port.h": import_header(imported["atahle.h"], "atahle.h"),
        "atahle_port.cpp": import_cpp(imported["atahle.cpp"], "atahle.cpp"),
        "atastorage_port.h": import_header(imported["atastorage.h"], "atastorage.h"),
        "atastorage_port.cpp": import_cpp(imported["atastorage.cpp"], "atastorage.cpp"),
    }
    if args.check:
        bad = [name for name, content in generated.items()
               if not (args.output / name).exists() or (args.output / name).read_text() != content]
        if bad:
            print("generated ATA port differs: " + ", ".join(bad), file=sys.stderr)
            return 1
    else:
        for name, content in generated.items():
            (args.output / name).write_text(content)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
