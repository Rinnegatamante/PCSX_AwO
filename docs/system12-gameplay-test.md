# System 12 gameplay replay

`tests/check_system12_gameplay.py` runs the local Soul Calibur Asia SOC14/VER.C
package (`soulclbrac`) through MAME and the native System 12 harness with one
generated input timeline. It checks that both runs complete, that the board
input adapters see the final pressed button, and that the H8 and C352 make
progress without reported unmapped accesses or unsupported H8 opcodes. It also
captures milestone frames and WAV files for manual review. These checks are
useful gameplay-path evidence; they do not establish general game correctness
or full playability.

The replay adds a coin and start press at frames 1800/1810, then alternates
movement and attack presses through frame 12000. Both adapters receive the same
frame/control/value rows: MAME through the `:JVS_*` input fields and PCSX through
the System 12 JAMMA input adapter. The final `p1button3` press provides a stable
input-delivery check. MAME runs with isolated configuration, NVRAM, and input
directories; the local ROM archive is copied into a temporary directory under
the source set name. The script verifies the archive and every packaged-region
digest against the source-pinned profile before running.

Run the full reference comparison from the repository root:

```sh
python3 tests/check_system12_gameplay.py --frames 12000 \
  --output-dir /tmp/system12-gameplay-evidence
```

The default profile and ROM paths are `artifacts/system12-profile` and
`roms/soulclbr.zip`. A full run needs the local `mame` executable, the pinned
MAME source checkout at `../mamedev`, the packaged regions, and the harness at
`build/mac-harness/pcsx-harness`. The output directory must not already exist
and must be outside the worktree. It contains the replay CSV/JSON, generated
MAME Lua, command logs, MAME PNG milestones and WAV, PCSX final PPM and WAV, and
a machine-readable `report.json`. Use `--skip-mame` to replay a native harness
alone or `--harness PATH` to select another build.

To build and run the separate source-timing experiment:

```sh
make -f Makefile.mac-harness BUILD=build/mac-harness/system12-timing SYSTEM12_TIMING=1
python3 tests/check_system12_gameplay.py --skip-mame --frames 12000 \
  --harness build/mac-harness/system12-timing/pcsx-harness \
  --output-dir /tmp/system12-gameplay-timing-evidence
```

The local profile records MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`; the replay verifies that commit
and the profile digests. MAME's `screen.refresh` property remains the configured
60 Hz default, so it is not used as the active display rate. The generated Lua
logs `1 / screen.frame_period`, which measured 59.8260978565 Hz at the captured
240-line NTSC mode. `src/devices/video/psx.cpp::updatevisiblearea` derives that
mode's rate from GPU status; its interlaced NTSC mode is 59.9400523286 Hz and
PAL is 50 Hz. `device_config_complete` sets a fallback 60 Hz mode and a 2.5 ms
vblank only when no refresh is configured.

The source clock audit and the replay results show why the native timing build
is kept distinct from the default console-timing build:

| Measure | Pinned MAME source / run | Default native build, 12,000 frames | Source-timing native build, 12,000 frames |
| --- | --- | ---: | ---: |
| Active display | `updatevisiblearea`: 59.8260978565 Hz, 240 lines; Lua runtime reports the same | fixed 60 Hz; 564,480 PSX cycles/frame | active GPU-derived rate; 835,755 PSX cycles/frame, equivalent to 50 MHz at 59.8261 Hz |
| H8/3002 | `namcos12.cpp`: 16.9344 MHz | 3,386,879,954 cycles; about 282,240/frame at 60 Hz | 3,396,722,242 cycles; about 283,060/frame at 59.8261 Hz |
| C352 | Configured at 25.4016 MHz with divider 288; `c352.cpp` sets stream rate to `clock / divider` = 88.2 kHz | 17,639,999 samples at 88.2 kHz | 17,691,261 samples at 88.2 kHz |
| H8 diagnostics | H8 executes and receives the board's periodic/vblank events | PC `0016d0`; 596,281,141 instructions; no illegal/unsupported opcodes | PC `0016ca`; 599,193,161 instructions; no illegal/unsupported opcodes |
| Board I/O | MAME JVS player input ends at `00008000` | JAMMA `ff/bf/ff/ff`; unmapped reads/writes `0/0` | JAMMA `ff/bf/ff/ff`; unmapped reads/writes `0/0` |
| Captured output | 240-line PNG milestones; WAV recorded by the host at 48 kHz | 512x240 PPM; WAV at 88.2 kHz; C352 peak 6000 | 512x240 PPM; WAV at 88.2 kHz; C352 peak 5404 |

The MAME machine configuration in `src/mame/namco/namcos12.cpp` clocks the
COH700 main device at 100 MHz and the H8 at 16.9344 MHz. The PSX CPU device
converts source clocks to its internal cycle count in
`src/devices/cpu/psx/psx.h::execute_clocks_to_cycles`. The native source-timing
experiment selects the 50 MHz System 12 CPU timing path and derives display
and frame-counter timing from active GPU mode; the default build keeps the
console timing path. The H8 and C352 counts closely follow each build's frame
duration. MAME's WAV is resampled by its host audio output to 48 kHz, so its
frame count is not a direct C352 chip-rate measurement.

The 12,000-frame baseline replay completed in both MAME and PCSX. The MAME
milestone at frame 6000 shows an active Stage 1 round; the frame-12000 capture
shows the game transitioning to character selection. The default native final
frame shows a Stage 1 `YOU LOSE` screen, while the source-timing native final
frame shows the Stage 1 continue prompt. This confirms that the tested input
path reaches live gameplay in these runs, while also showing that the observed
game states do not match at the same nominal frame. The runner does not assert
pixel equivalence, correct round outcomes, or audio equivalence. Review the
saved images and logs when changing board timing or device behavior.

The test therefore validates a concrete subset: local media provenance,
repeatable input delivery, long-run execution, H8 activity, C352 output,
zero-reported H8 unsupported instructions and board-unmapped accesses, and
visible gameplay in manually reviewed captures. It does not validate every
game state, all controls, timing-sensitive software, or the complete System 12
library.
