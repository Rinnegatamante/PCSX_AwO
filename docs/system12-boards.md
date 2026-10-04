# Namco System 12 profiles

`scripts/system12_boards.json` records all 58 game aliases declared by the
pinned `namcos12.cpp` at MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`. Each entry records its machine
configuration, input profile, C++ state class, init callback, main RAM size,
reset-copy boot hack, alternate ROM bank behavior, Tag DMA behavior, and any
external hardware the native board adapter does not implement.

The current runtime supports the ordinary COH-700 board state for the
`coh700`, `coh700b`, and `coh716` configurations. It handles 4 MiB and 16 MiB
main RAM, the 12-byte reset copy used by `coh700b` and `coh716`, and the
alternate 2 MiB ROM banking used by `namcos12_altbank_state`. Main ROM, banked
ROM, sub CPU ROM, and C352 wave ROM allocations follow their exact MAME region
sizes; the game ROM window stays 2 MiB. Normal Soul Calibur region folders
without `board.cfg` retain the original COH-700 defaults.

To package a directory produced by `prepare_mame_psx.py`, run:

```sh
python3 scripts/prepare_system12_board.py assembled-system12 \
  --output-dir artifacts/system12-board
```

The output contains `main.bin`, `banked.bin`, `sub.bin`, `wave.bin`,
`board.cfg`, and `system12_manifest.json`. The packager verifies the generic
manifest's pinned MAME revision and every required assembled region's length,
CRC32, and SHA1 before copying. It preserves variable banked-ROM lengths and
sets the runtime profile from the registry rather than guessing from file
sizes. Unsupported profiles can be packaged for inventory, but the runtime
rejects their `board.cfg` before guest execution.

Profiles that need the Cyberlead JVS board, gun I/O, custom Technodrive I/O,
Tekken Tag protection, CDXA/local disk media, Truck Kyosokyoku I/O, or Kart
Duel link hardware are marked explicitly in the registry and refused by the
runtime. The Tekken Tag DMA registers are represented for board mapping, but
its game-specific protection remains unimplemented, so the set is not
advertised as bootable. The alternate-bank and Tekken Tag special-DMA paths
are based on the pinned MAME callbacks; only the supported alt-bank path is
covered by the runnable synthetic harness profile.

Validation includes all 58 source aliases and profile declarations, strict
region digest checks, a generic-manifest repack of the local Soul Calibur ROM
set byte-for-byte against the established raw-region output, the default
COH-700 harness, and a synthetic COH-716 16 MiB RAM / alt-bank / reset-copy
profile. This validates board memory and mapping behavior, not complete
gameplay coverage for every profile.
