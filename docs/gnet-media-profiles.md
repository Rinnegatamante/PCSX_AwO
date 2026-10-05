# Taito G-Net media profiles

This registry records MAME source metadata for all 41 `taitogn.cpp` sets, including the two BIOS roots and 39 game entries. It is data for a future loader integration; it does not implement BIOS selection, input variants, game-specific I/O, boot, or media loading.

Source pin: MAME `deb2f54fe7300e7a192690318beb860159bfd52a`, `src/mame/sony/taitogn.cpp` SHA-256 `3d431ab3480c5a2dfd498a7cb3095f9800027e486d0a51203a913ab91d556e8c`. `scripts/gnet_profiles_generate.py --check` validates the pin, set inventory, BIOS ancestry, source macros, and deterministic generated outputs.

## Card and metadata semantics

The PC Card option comes from each `DISK_REGION` and the MAME machine configuration. `taitopccard1`, `taitopccard2`, and `taitocf` select the Taito Type 1, Type 2, and CompactFlash devices. Bootleg profiles use generic `ataflash`. MAME CHD tags are `CIS ` (CIS bytes), `KEY ` (card unlock key), and `IDNT` (ATA IDENTIFY). In `src/devices/bus/pccard/ataflash.cpp`, MAME pads a supplied CIS to 512 bytes and synthesizes its default CIS when absent; the three Taito card variants enter their locked state only for an exact five-byte KEY. Thus `KEY_OPTIONAL_EXACT5` records source behavior, not a claim that every disk has a KEY. Generic `ataflash` does not use the Taito key protocol. `IDNT` is raw ATA IDENTIFY metadata; a consumer must validate its 512-byte length. The registry does not assert that each CHD actually contains these tags.

## Profiles

`CHD SHA1` is the exact SHA-1 declared by MAME's `DISK_IMAGE`. BIOS is the per-ROM default, with `v1` the source's first/default option when not overridden; `fixed` denotes the COH-1002T ROM with no selectable EPROM BIOS. `GAME flags`, machine configuration, and input configuration are source fields and are recorded for future implementation; their runtime behavior is not claimed here.

| Set | BIOS model / option | Machine config | Input config | Card | CHD name | CHD SHA1 | Metadata policy | Flags |
|---|---|---|---|---|---|---|---|---|
| aerofgtsg | coh3002t (COH-3002T) / mb2011 | coh3002t_bl | aerofgtsg | ATAFLASH | aerofsp | 2e768dc83d10e38b4814734f0c748cf55bb2b3ca | CIS_DEFAULT_IF_MISSING;KEY_UNUSED;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| brvbladeg | coh3002t (COH-3002T) / mb2011 | coh3002t_bl | taitogn_jp1 | ATAFLASH | braveblade | 63d6de481eb34b261b4f91d1127dbadae01210be | CIS_DEFAULT_IF_MISSING;KEY_UNUSED;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| chaoshea | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | chaosheat | c13b7d7025eee05f1f696d108801c7bafb3f1356 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| chaosheaj | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | chaosheatj | 2f211ac08675ea8ec33c7659a13951db94eaa627 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| flamegung | coh3002t (COH-3002T) / mb2011 | coh3002t_bl | taitogn_jp1 | ATAFLASH | flamegun | 4f011a88985620d4d5b696340c48d9ea2454b647 | CIS_DEFAULT_IF_MISSING;KEY_UNUSED;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| flipmaze | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | flipmaze | 423b6c06f4f2d9a608ce20b61a3ac11687d22c40 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| ftimpactg | coh3002t (COH-3002T) / mb2011 | coh3002t_bl | taitogn_jp1 | ATAFLASH | ftimpact | aa6a0c1002c02b4a4ddb41eed64f9e47ac6adfa8 | CIS_DEFAULT_IF_MISSING;KEY_UNUSED;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| gdariusg | coh3002t (COH-3002T) / mb2011 | coh3002t_bl | taitogn_jp1 | ATAFLASH | gdarius | 2888fb3989c6628ad09307258ae78c175a8867c6 | CIS_DEFAULT_IF_MISSING;KEY_UNUSED;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| gobyrc | coh3002t (COH-3002T) / v1 | coh3002t_t2 | gobyrc | TYPE2 | gobyrc | 0bee1f495fc8b033fd56aad9260ae94abb35eb58 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| kollon | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | kollon | d8ea5b5b0ee99004b16ef89883e23de6c7ddd7ce | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| kollonc | coh3002t (COH-3002T) / v2 | coh3002t_cf | taitogn_jp1 | CF | kollonc | ce62181659701cfb8f7c564870ab902be4d8e060 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| mahjngoh | coh3002t (COH-3002T) / v1 | coh3002t_t1 | mahjngoh | TYPE1 | mahjngoh | 3ef1110d15582d7c0187438d7ad61765dd121cff | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| mawasunda | coh1002t (COH-1002T) / fixed | mawasunda | mawasunda | CF | e83-01__ | ae5b14186905c93401ca65231dec43724df7957a | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| nightrai | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | nightrai | 74d0458f851cbcf10453c5cc4c47bb4388244cdf | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| otenamhf | coh3002t (COH-3002T) / v2 | coh3002t_cf | taitogn_jp1 | CF | otenamhf | 5b15c33bf401e5546d78e905f538513d6ffcf562 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| otenamih | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | otenamih | b3babe3a1876c43745616ee1e7d87276ce7dad0b | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| otenki | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | otenki | 7e745ca4c4570215f452fd09cdd56a42c39caeba | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| psyvaria | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | psyvaria | 3c7fca5180356190a8bf94b22a847fdd2e6a4e13 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| psyvarij | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | psyvarij | b981a42a10069322b77f7a268beae1d409b4156d | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| psyvarrv | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | psyvarrv | 277c4f52502bcd7acc1889840962ec80d56465f3 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| raycris | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | raycris | 9d255710c87c3286542d357820d828807cc6ca07 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| raycrisj | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | raycrisj | 015cb0e6c4421cc38809de28c4793b4491386aee | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| raystormg | coh3002t (COH-3002T) / mb2011 | coh3002t_bl | raystormg | ATAFLASH | raystorm | 3ff1979c45670d813f292a88ada1a0e9586718e7 | CIS_DEFAULT_IF_MISSING;KEY_UNUSED;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| rcdego | coh3002t (COH-3002T) / v1 | coh3002t_t1 | gobyrc | TYPE1 | rcdego | 9e177f2a3954cfea0c8c5a288e116324d10f5dd1 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| shanghss | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | shanghss | 7964f71ec5c81d2120d83b63a82f97fbad5a8e6d | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| shangtou | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | shanghaito | 9901db5a9aae77e3af4157aa2c601eaab5b7ca85 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| shikigam | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | shikigam | fa49a0bc47f5cb7c30d7e49e2c3696b21bafb840 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| shikigama | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | shikigama | a6fe194c86730963301be9710782ca4ac1bf3e8d | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| shngmtkbg | coh3002t (COH-3002T) / mb2011 | coh3002t_bl | taitogn_jp1 | ATAFLASH | shangmtkb | ba4067698b92fa83202ea10b2f836c094cbfc357 | CIS_DEFAULT_IF_MISSING;KEY_UNUSED;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| sianniv | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | sianniv | 1e08b813190a9e1baf29bc16884172d6c8da7ae3 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| soutenry | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | soutenry | 9204d0be833d29f37b8cd3fbdf09da69b622254b | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| spuzbobl | coh3002t (COH-3002T) / v1 | coh3002t_t2 | taitogn | TYPE2 | spuzbobl | 1b1c72fb7e5656021485fefaef8f2ba48e2b4ea8 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| spuzboblj | coh3002t (COH-3002T) / v1 | coh3002t_t2 | taitogn | TYPE2 | spuzbobj | dac433cf88543d2499bf797d7406b82ae4338726 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| tblkkuzug | coh3002t (COH-3002T) / mb2011 | coh3002t_bl | taitogn_jp1 | ATAFLASH | tblkkuzushi | 2801a8b1c789a8b0ea73f083d4bd3db1ef6971d8 | CIS_DEFAULT_IF_MISSING;KEY_UNUSED;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| usagi | coh3002t (COH-3002T) / v1 | coh3002t_t2 | usagi | TYPE2 | usagi | edf9dd271957f6cb06feed238ae21100514bef8e | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| xiistag | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | xiistag | 586e37c8d926293b2bd928e5f0d693910cfb05a2 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| zokuoten | coh3002t (COH-3002T) / v1 | coh3002t_t2 | taitogn | TYPE2 | zokuoten | 116e58c90f39a3c18ca6fe0216c998ba02c58814 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| zokuotena | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | zokuotena | 5ce13db00518f96af64935176c71ec68d2a51938 | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |
| zooo | coh3002t (COH-3002T) / v1 | coh3002t_t1 | taitogn | TYPE1 | zooo | e275b3141b2bc49142990e6b497a5394a314a30b | CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512 | MACHINE_SUPPORTS_SAVE |

## BIOS roots and dumps

| Root/model | Main BIOS ROM | CRC32 | SHA-1 | CAT702 1 | CAT702 2 | Firmware flash | CRC32 | SHA-1 | Selectable EPROM BIOSes |
|---|---|---|---|---|---|---|---|---|---|
| coh1002t (COH-1002T) | m534002c-14.ic353 | e3f23b6e | e18907cf8c6ba54d96edba0a9a00487a90219e0d | tt01.ic652 (96d94f2b06fb7ecd91454969f4959a583ffb9d5a) | tt99.u17 (89981ec68d30729c27d31f820cbfd0c23ed14a03) | flash.u30 | 9100dbf2 | 9ce888bd2a7e575c8ca71d978cb85de22afb5660 | none (fixed EPROM fill) |
| coh3002t (COH-3002T) | m534002c-60.ic353 | 03967fa7 | 0e17fec2286e4e25deb23d40e41ce0986f373d49 | tt10.ic652 (2cc02113207a8f0b078152d31ce6503411750e70) | tt16.u17 (9dcba08f10775a9adf2b1f382c947460edd3d239) | flash.u30 | c48c8236 | c6dad60266ce2ff635696bc0d91903c543273559 | v1, v2, mb2009, mb2011 |

| BIOS option (COH-3002T) | ROM file | CRC32 | SHA-1 |
|---|---|---|---|
| v1 | f35-01_m27c800_v1.bin | cd15cc30 | 78361f46fa7186d5058937c86c66247a86b1257f |
| v2 | f35-01_m27c800.bin | 6225ec11 | 047852d456b6ff85f8e640887caa03cf3e63ffad |
| mb2009 | f35-01_m27c800_modbios_mb-2009_dump_by_aje_fr.bin | 6367aedf | 8400775e0c974c8b017e26ef9339144cad457c63 |
| mb2011 | f35-01_m27c800_modbios_mb-2011.bin | f98ae118 | c6918dfbb6fcf686dde4b731e84f740177ab31f0 |

The COH-1002T BIOS macro marks its uPD78081 MCU dump `NO_DUMP`; COH-3002T does too. Both BIOS roots share the same raw PC Card metadata tags at the driver level, but each root has its own CPU BIOS, CAT702 dumps, and firmware flash image.

## Local availability snapshot

On 2026-10-05, no G-Net BIOS archives, EPROM files, or game CHDs were found in the workspace `roms/`, the adjacent MAME checkout's `roms/`, `~/.mame/roms/`, or `~/Library/Application Support/mame/roms/`. The workspace `roms/` contained `soulclbr.zip`; the MAME ROM directory contained only `dir.txt` and an unrelated HD44780 ROM. No media was downloaded. This is a local snapshot, not a statement about media availability elsewhere.

## Loader boundary

No `board.ini`/native loader profile is added here. Runtime boot still requires BIOS/firmware loading, the real PC Card behavior, CHD CIS/KEY/IDNT consumption, ATA taskfile/media integration, and per-game input/config behavior. Profile completeness is source metadata coverage only; no G-Net boot claim follows from this registry.
