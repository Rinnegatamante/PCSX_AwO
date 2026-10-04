# Namco System 10 media profiles

The registry and packager are pinned to MAME commit `deb2f54fe7300e7a192690318beb860159bfd52a` and `src/mame/namco/namcos10.cpp`. `scripts/system10_boards.py` extracts the 45 `GAME` rows, `ROM_START` regions, board state, machine configuration, input profile, initialization function, flags, ROM filenames/digests, disk declarations, and attached board helpers. It cross-checks the set names against the Namco System 10 family in `scripts/mame_psx_systems.json`; profile generation fails if either source is incomplete or differs.

The source contains 3 MEM(M), 37 MEM(N), 3 MEM(P3), and 2 MEM/IO sets. MEM(P3) provides MP3 and analog hardware, MEM/IO is the Taito board, while MEM(N) machine configurations may add EXIO, MGEXIO, or EXFINALIO hardware. The registry records those additions and extra ROM regions instead of treating all MEM(N) configurations as identical. Game clones inherit the crypto profile selected by the actual MAME machine configuration; MEM(M) selects its profile by the `init_mrdrilr2` function. These names are checked against the existing native crypto profile table. They identify decryption logic, not a per-game invented key.

`prepare_system10_board.py` only packages base MEM(M)/MEM(N) profiles with supported layouts. It refuses declared add-on boards, `MACHINE_NOT_WORKING`, `MACHINE_UNEMULATED_PROTECTION`, CHD media, `NO_DUMP` files, and unexpected regions. It verifies every local ROM member against source size, CRC32, and SHA1 through the generic assembler. No ROM media is downloaded. MEM(M) output is `main.bin`, `rawflash.bin` (MAME `nand` region), and `datarom.bin`; MEM(N) output is `main.bin` and one `rawnandN.bin` per declared chip. Each raw NAND output preserves the complete 528-byte page stream, including OOB bytes. It does not invent missing chips or silently drop undeclared device images.

`runtime_candidate` means the source-declared media layout matches the standalone MEM(M)/MEM(N) adapter and lacks the above blockers. It does not mean the game boots or that the whole arcade board is emulated. The output manifest sets `board_execution_verified` to false.

Regenerate the registry and package verified local media with:

```sh
python3 scripts/system10_boards.py
python3 scripts/prepare_system10_board.py --set chocovdr --mame-source ../mamedev --rompath /path/to/mame/roms --output-dir artifacts/system10/chocovdr
```

## All 45 MAME sets

| Set | PCB family | Machine / input | Declared add-on | Media packager scope |
|---|---|---|---|---|
| `ballpom` | MEM(N) | `ns10_ballpom` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working |
| `chocovdr` | MEM(N) | `ns10_chocovdr` / `namcos10` | none | package candidate |
| `g13jnr` | MEM(P3) | `ns10_g13jnr` / `g13jnr` | MEM(P3) MP3/analog ROM board | MEM(P3) MP3/analog ROM board |
| `gahaha` | MEM(N) | `ns10_gahaha` / `gahaha` | EXIO | EXIO |
| `gahaha2` | MEM(N) | `ns10_gahaha2` / `gahaha` | EXIO | EXIO |
| `gamshara` | MEM(N) | `ns10_gamshara` / `gamshara` | none | package candidate |
| `gamsharao` | MEM(N) | `ns10_gamshara` / `gamshara` | none | package candidate |
| `gegemdb` | MEM(N) | `ns10_gegemdb` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working, unemulated protection, extra regions: nvram |
| `gjspace` | MEM(N) | `ns10_gjspace` / `gjspace` | none | package candidate |
| `gunbalina` | MEM(N) | `ns10_ptblank3` / `ptblank3` | EXIO | EXIO, unemulated protection, extra regions: decrypter:nonlinear_table |
| `ippo` | MEM/IO | `ns10_ippo` / `ippo2` | MEM/IO Taito ROM board | MEM/IO Taito ROM board, NO_DUMP, MAME not working, extra regions: plds |
| `ippo2` | MEM/IO | `ns10_ippo2` / `ippo2` | MEM/IO Taito ROM board | MEM/IO Taito ROM board, NO_DUMP, MAME not working, extra regions: plds |
| `kd2001` | MEM(N) | `ns10_kd2001` / `namcos10` | none | MAME not working, unemulated protection, extra regions: pdrivecpu |
| `keroro` | MEM(N) | `ns10_keroro` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working |
| `knpuzzle` | MEM(N) | `ns10_knpuzzle` / `namcos10` | none | package candidate |
| `konotako` | MEM(N) | `ns10_konotako` / `konotako` | none | package candidate |
| `medalnt` | MEM(N) | `ns10_medalnt` / `namcos10` | EXFINALIO | EXFINALIO, MAME not working |
| `medalnt2` | MEM(N) | `ns10_medalnt2` / `namcos10` | EXFINALIO | EXFINALIO, MAME not working |
| `medalnta` | MEM(N) | `ns10_medalnt` / `namcos10` | EXFINALIO | EXFINALIO, MAME not working |
| `mrdrilr2` | MEM(M) | `ns10_mrdrilr2` / `mrdrilr2` | none | package candidate |
| `mrdrilr2j` | MEM(M) | `ns10_mrdrilr2` / `mrdrilr2` | none | package candidate |
| `mrdrilr2u` | MEM(M) | `ns10_mrdrilr2` / `mrdrilr2` | none | package candidate |
| `mrdrilrg` | MEM(N) | `ns10_mrdrilrg` / `mrdrilr2` | none | package candidate |
| `nflclsfb` | MEM(N) | `ns10_nflclsfb` / `nflclsfb` | EXIO | EXIO, MAME not working |
| `nicetsuk` | MEM(P3) | `ns10_nicetsuk` / `nicetsuk` | MEM(P3) MP3/analog ROM board | MEM(P3) MP3/analog ROM board |
| `pacmball` | MEM(N) | `ns10_pacmball` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working |
| `panikuru` | MEM(N) | `ns10_panikuru` / `namcos10` | none | package candidate |
| `ptblank3` | MEM(N) | `ns10_ptblank3` / `ptblank3` | EXIO | EXIO, unemulated protection, extra regions: decrypter:nonlinear_table |
| `puzzball` | MEM(N) | `ns10_puzzball` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working |
| `puzzballa` | MEM(N) | `ns10_puzzball` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working |
| `sekaikh` | MEM(N) | `ns10_sekaikh` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working, extra regions: exio:nvram |
| `sekaikha` | MEM(N) | `ns10_sekaikh` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working, extra regions: exio:nvram |
| `slotouji` | MEM(N) | `ns10_slotouji` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working, unemulated protection |
| `slotoujia` | MEM(N) | `ns10_slotouji` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working, unemulated protection |
| `squizchs` | MEM(P3) | `ns10_squizchs` / `squizchs` | MEM(P3) MP3/analog ROM board | MEM(P3) MP3/analog ROM board |
| `startrgn` | MEM(N) | `ns10_startrgn` / `startrgn` | none | package candidate |
| `sugorotc` | MEM(N) | `ns10_sugorotic` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working |
| `sugorotca` | MEM(N) | `ns10_sugorotic` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working |
| `sugorotcb` | MEM(N) | `ns10_sugorotic` / `mgexio_medal` | MGEXIO | MGEXIO, MAME not working |
| `taiko2` | MEM(N) | `ns10_taiko2` / `taiko` | EXIO | EXIO, CD image, MAME not working |
| `taiko3` | MEM(N) | `ns10_taiko3` / `taiko` | EXIO | EXIO, CD image, MAME not working |
| `taiko4` | MEM(N) | `ns10_taiko4` / `taiko` | EXIO | EXIO, CD image, MAME not working |
| `taiko5` | MEM(N) | `ns10_taiko5` / `taiko` | EXIO | EXIO, CD image, MAME not working |
| `taiko6` | MEM(N) | `ns10_taiko6` / `taiko` | EXIO | EXIO, CD image, MAME not working |
| `taikort` | MEM(N) | `ns10_taikort` / `taiko` | EXIO | EXIO, CD image, MAME not working |

Run focused tests with `python3 -m unittest tests.test_system10_boards -v`. Synthetic tests cover strict MEM(M)/MEM(N) region mapping, OOB preservation, parent archive lookup by verified member hash, output digest manifests, and fail-closed behavior for unsupported board/media requirements.
