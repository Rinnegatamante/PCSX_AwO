# Konami System GQ local media bundle

`cryptklr` is the sole System GQ set in the pinned MAME registry. This preparer
assembles the source-declared ROM regions, checks the local writable hard-disk
CHD, and writes the driver's 93C46 default EEPROM bytes. It does not download
media or claim board/game execution is working. The registry and facts come
from MAME commit `deb2f54fe7300e7a192690318beb860159bfd52a`, driver
`src/mame/konami/konamigq.cpp`.

| Output | MAME source region/device | Size | Notes |
| --- | --- | ---: | --- |
| `bios.bin` | `maincpu:rom` | 512 KiB | GQ PSX BIOS ROM region |
| `soundcpu.bin` | `soundcpu` | 512 KiB | M68000 program, source word-swapped load assembled |
| `pcmrom.bin` | `k054539` | 512 KiB | K054539 sample ROM |
| `420uaa04.chd` | `harddisk` | source CHD size | Writable SCSI target ID 0 image, SHA1 checked against MAME |
| `default_eeprom.bin` | `konamigq_def_eeprom` | 128 bytes | 64 source 16-bit words serialized little-endian; factory defaults only |
| `default_eeprom_LICENSE.txt` | MAME driver license | text | Full BSD-3-Clause notice for the bundled source-owned default bytes |

The CHD verifier invokes `chdman info -v` and `chdman verify`. It requires the
MAME hard-disk `GDDD` metadata record (`CYLS`, `HEADS`, `SECS`, `BPS`), checks
that the dimensions are positive and their product equals the logical byte
size, and verifies the CHD header SHA1 against the `DISK_IMAGE` declaration.
This follows MAME's `harddisk_image_device` geometry format and the SCSI hard
disk's use of those image parameters. The local writable image is copied into
the package; the preparer does not create a blank disk or persistence overlay.

Example, using only local files:

```sh
python3 scripts/prepare_konamigq_board.py \
  --set cryptklr \
  --mame-source ../mamedev \
  --rompath /path/to/mame/roms \
  --output-dir artifacts/system-gq
```

The ROM ZIP may be found under `cryptklr.zip`; the HDD is searched as
`420uaa04.chd` in the set directory or ROM path. `--archive` accepts an
explicit local ZIP containing ROM members and/or the CHD. A failed source,
CRC/SHA1, CHD verification, geometry, or profile check exits without replacing
the output package. Existing unrelated files in the output directory are
preserved.

The package records MAME source provenance and media hashes in `manifest.json`
and hardware facts in `board.ini`. It identifies `default_eeprom.bin` as a
factory default image, not imported or persisted user NVRAM. Board/game
execution remains unverified; the system still needs its native adapter,
M68000 sound CPU, TMS57002 DSP, NCR/SCSI hard-disk command implementation,
three lightguns, and GQ-specific I/O.

On a later preparation, an existing top-level `nvram.bin` is preserved as
user-persisted state. Only a valid prior GQ manifest can authorize replacement
of the preparer's own known output files; unrelated files and unrecognized
manifest paths are retained or cause a refusal. The EEPROM defaults and their
license are included in the package manifest with SHA-256 provenance.

Run `python3 -m unittest tests/test_prepare_konamigq_board.py -v`. The HDD test
creates only a temporary synthetic CHD with `chdman`; no synthetic media is
committed or presented as a game image.
