# MAME ROM-region preparation

`scripts/prepare_mame_psx.py` verifies local ROM media against MAME and writes
raw ROM regions. It does not configure a board, run a game, or claim arcade
machine support. The declarations are pinned to MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a` and cover the ten PlayStation-family
drivers in [`prepare_mame_psx.py`](../scripts/prepare_mame_psx.py).

For example, this prepares System 12 Soul Calibur using a locally owned ZIP
whose filename differs from the MAME set name:

```sh
python3 scripts/prepare_mame_psx.py \
  --set soulclbrac --mame-source ../mamedev --rompath roms \
  --archive roms/soulclbr.zip --output-dir artifacts/mame-psx/soulclbrac
```

`--archive` is optional. Without it, the tool checks set and parent ZIPs, then
other ZIPs in `--rompath`; archive names do not establish identity. Every
selected member must match MAME's declared filename, size, CRC32, and SHA1.
Loose files in `--rompath` are also accepted. Inputs are read only; the tool
does not download media. BIOS variants use the source default unless `--bios`
selects another declared BIOS. Required `NO_DUMP` files and missing or
mismatched ROMs stop preparation.

The assembler supports standard regions (including endian and erase flags),
linear loads, `16_BYTE`, `16_WORD`, `16_WORD_SWAP`, `32_BYTE`, `32_WORD`, and
`32_WORD_SWAP` layouts, plus `ROM_CONTINUE`, `ROM_RELOAD`, `ROM_FILL`, and
`ROM_COPY`. It checks bounds and write collisions. Unknown ROM operations or
load layouts fail the affected source block; they are not silently skipped.
`--coverage-report path.json` writes the per-set parse classification.

Disk declarations are listed in `pending_disk_media` in the manifest with
their MAME SHA1 and a pending status; CHDs are not opened or assembled. A
manifest's `rom_regions_assembled` describes only region output,
`disk_media_complete` tracks pending disk media, and
`board_execution_verified` remains false.

At this pinned source revision, the parser classified all **491 of 491**
`ROM_START` blocks across the ten inventoried drivers as parsed (zero unknown
or failed blocks). This is a syntax and declaration coverage audit, not a
claim that every set's media is present or that every board runs. The synthetic
unit tests exercise digest checking, archive lookup, BIOS selection, layout
interleaving, continuation/reload/fill/copy, pending disks, and fail-closed
errors. For the Soul Calibur set `soulclbrac`, the four generated regions were
also compared byte-for-byte by SHA1 with the existing System 12 assembler:
`maincpu:rom`, `bankedroms`, `sub`, and `c352` all matched.
