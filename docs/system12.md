# Soul Calibur / Namco System 12 bring-up

The supplied `roms/soulclbr.zip` is **Soul Calibur (Asia, SOC14/VER.C)**,
MAME driver **`soulclbrac`**. It is not the World A2 parent `soulclbr`.
The original archive is left intact. No ROM data is included in source control.

## Reproduce

From the repository root on native macOS Apple Silicon with Xcode command-line
tools and Python 3:

```sh
# Baseline: requires an installed MAME with Lua support.
python3 tests/check_mame_soulcalibur.py
python3 tests/check_mame_sharedram.py
python3 tests/check_mame_boot_state.py
python3 tests/check_mame_h8.py
python3 tests/check_mame_gpu_vram.py

# Native PCSX interpreter + existing GPU_NEON intrinsic rasterizer.
make -f Makefile.mac-harness -j8
make -f Makefile.mac-harness smoke
make -f Makefile.mac-harness board-test devices-test
python3 -m unittest tests/test_prepare_system12.py -v

# Verify every required ROM's size, CRC32 and SHA1, identify the actual revision,
# and assemble ROM regions. Uses the bundled MAME declarations by default.
python3 scripts/prepare_system12.py roms/soulclbr.zip -o artifacts/system12
build/mac-harness/pcsx-harness --system12 artifacts/system12 \
  --board-test --frames 3600 --output artifacts/system12/boot.ppm
```

`--mame-source /path/to/mame` optionally reads declarations from a MAME checkout.
The bundled snapshot covers eight Soul Calibur revisions, but only Asia C has
been booted locally. The region loader checks sizes; ROM identity/hashes are
verified by the preparation script, so rerun that script after changing ROMs.

To reproduce coin/start and character selection, create an input CSV:

```csv
frame,control,pressed
2000,coin1,1
2020,coin1,0
2100,p1start,1
2120,p1start,0
2300,p1button1,1
2320,p1button1,0
```

Save it as `artifacts/system12/match.csv`, then run:

```sh
build/mac-harness/pcsx-harness --system12 artifacts/system12 --frames 3600 \
  --inputs artifacts/system12/match.csv --audio artifacts/system12/match.wav \
  --output artifacts/system12/match.ppm
```

The harness supports bounded block traces for investigating boot:

```sh
build/mac-harness/pcsx-harness --system12 artifacts/system12 --frames 1200 \
  --trace artifacts/system12/boot.csv --trace-blocks 1000 \
  --output artifacts/system12/boot.ppm
```

The executable is headless, with PPM framebuffer output and stderr diagnostics.
It uses the interpreter, null console-SPU output and optional board C352 WAV capture. See
[the harness README](../tools/mac-harness/README.md) for execution limits and
capture behavior.

## Verified on 2026-10-04

MAME **0.287 arm64** audits all nine supplied ROMs successfully as `soulclbrac`.
With clean NVRAM it reaches the Namco logo and then the 3D attract sequence.
The 60-second emulated run exits successfully; screenshots at 30, 45 and 60
seconds show Kilik. Audio and player interaction were not tested.
Generated evidence is in `artifacts/mame/`, including `run.log`,
`soulclbrac-verify.log` and `attract-030s.png`.

The native harness's generated PS-X EXE writes GPU commands through emulated
MIPS MMIO and renders a yellow triangle on a blue background. Exact framebuffer
pixels and a CPU-written RAM sentinel pass with `BOARD=0` and `BOARD=1`.
The ARM64 rasterizer object contains Advanced SIMD instructions (`smull.4s`,
`smlsl.4s`, `add.4s`, `abs.4s`).

PCSX now executes both real CPU ROMs and reaches the **3D attract sequence**
with Kilik. The 3600-frame native run captured a 512 x 240 stage/character image
in `artifacts/system12/devices-3600.png`. It completed with these diagnostics:

```text
cycles=2032128027 pc=80054f74 flips=2898
DMA5=1421 bytes=1695744 truncations=0
H8 pc=0016d0 cycles=1016063912 instructions=175425333
H8 halted=0 illegal=000000:0000 unsupported=0
C352 rate=88200 frames=5291999 peak=3933
unmapped reads=0 writes=0
```

A second 3600-frame run injected real JAMMA coin/start inputs, entered
character select and then selected Kilik with button 1. The resulting
`artifacts/system12/match-3600.png` shows **Stage 1, Kilik versus Sophitia**,
health bars and timer. The CSV and complete log are retained beside that image.
The controls run records a stereo PCM16 C352 WAV at 88.2 kHz with 5,291,999
nonzero-output sample frames (`controls-3600.wav`). This validates navigation
into a match; sustained play, all controls, auditory quality and timing still
require further comparison. The current tested
runtime still uses console main-CPU timing; the 3600 frames represent 60
emulated seconds under that model. Capture retains the last rendered flip.

Earlier versions reached STEP 1 with no H8 execution, then an ADC completion
wait at H8 PC 0x990. The independent MAME oracle established that the board must
connect vblank both to H8 IRQ1 and to the ADC external-trigger pin. Connecting
the falling trigger edge allows the real firmware to complete its conversion
and the main CPU to continue; no shared-memory responses are fabricated.

The upper-VRAM selftest uploads indexed texture data at row 512, a CLUT at row
800 and renders into row 600. It verifies 4bpp, 8bpp, odd 8bpp cache parity and
16bpp textures. The equivalent MMIO commands pass on MAME in
`tests/mame_gpu_vram_oracle.lua`; console row zero remains independent.

## Implemented board behavior

`PCSX_SYSTEM12` enables `libpcsxcore/system12.c` in the separate harness build.
Ordinary builds keep their fixed console memory sizes. The adapter requires
the interpreter and memory lookup tables; it is not wired into libretro/Vita.
The table below describes the verified Soul Calibur COH-700 configuration.
The [58-set profile registry](system12-boards.md) also selects variable ROM
sizes, 16 MiB COH-716 RAM, alternate banks and the upstream reset-copy behavior.
Profiles with additional unimplemented hardware are rejected before execution.

| Component | Current behavior |
| --- | --- |
| Main program | 4 MiB, two ROM_LOAD16_BYTE lanes; physical and KSEG ROM views |
| Main RAM | 4 MiB, 8 MiB mirrored address window and KSEG views |
| Data ROM | 32 MiB; sixteen 2 MiB banks at `1fa00000` |
| Bank select | 16-bit register at `1f000000`, safe open-bus invalid banks |
| ROM DMA | Channel 5, source offset at `1f700000`; main/data selection, bounds truncation, IRQ |
| GPU DMA | Core and NEON linked-list paths preserve upper 2 MiB RAM addresses |
| Shared RAM | 64 KiB backing, PSX 16 KiB window at `1f080000`, both 16-bit lanes |
| EEPROM | AT28C16 2 KiB data + 32-byte ID; byte lanes at `1f140000`; 200 μs write/erase busy and D7 polling |
| Reset | `bfc00000`, arcade ROM boot without console BIOS/HLE or boot patches |
| H8/3002 | Standalone 264-instruction-definition interpreter, internal RAM, GPIO, IRQ/timer/SCI/ADC; 16.9344 MHz scheduling |
| H8 external bus | Logical BE sub ROM, shared RAM byte-lane conversion, atomic C352 words |
| Vblank | Both IRQ1 and ADC ADTRG, including the falling-edge conversion trigger |
| C352 | 32 voices, samples/loops/mu-law/noise/filter/mix; four-channel 88.2 kHz generation |
| RTC/settings | MAME-derived CE/CLK/DATA protocols; deterministic RTC, byte-completion SCI wiring |
| GPU VRAM | 2 MiB, upper type-2 TPAGE bit, 10-bit CLUT/drawing/transfer/display rows; 64 texture-page masks |
| JAMMA/JVS | Frame-indexed JAMMA coin/service/test/player inputs on H8 bus; coin/start/selection reaches a match; no connected JVS endpoint |

The AT28C16 keeps its image and pending write across PSX board resets. A newly
created board starts erased; host persistence can import/export the complete
2080-byte data+ID image through `system12_eeprom_import` and
`system12_eeprom_export`. The bus drives the chip's low-byte lanes, returns
open-bus `0xff` on the other byte lanes, and exposes D7 polling at every mapped
address while a write or erase is busy. EEPROM time advances from PSX cycles
with a retained sub-microsecond fraction, so reset re-anchors the PSX cycle
counter without rewinding the EEPROM clock.

DMA5 supports the two block-read modes used for this bring-up,
`01000000` and `01000200`. Like the MAME ROM callback, its destination is masked
to main RAM and source/destination lengths are bounded; MADR is preserved.
The block count of zero in request mode means 65536 blocks. Completion is
synchronous, and timing remains approximate.

The board selftest uses synthetic regions without game ROMs to check RAM
independence/mirrors, ROM fetch, all banks and bank limits, shared byte lanes,
EEPROM lanes and 199/200 μs busy completion with D7 polling, DMA bounds and deferred DPCR enable, zero block count, DMA IRQ,
and a GPU linked-list packet in upper RAM. The lane semantics were independently
confirmed by writing values through MAME's PSX and H8 address spaces; see
`tests/mame_sharedram_oracle.lua`. The `offset*4` diagnostic text in the MAME
driver does not describe the actual contiguous 16-bit lane mapping.

## Remaining validation and porting

- Compare sustained gameplay, all player controls and captured C352 audio with
  MAME. The console SPU does not provide System 12 audio.
- Match CPU/GPU/peripheral timing and compare additional frames with MAME.
  H8 instructions carry cycle budgets, but serial pin transitions are grouped
  at byte completion; exact waveforms, external serial/timer clocks, DMA and
  watchdog remain incomplete. See [H8 details](h8-port.md).
- Extend the fixed COH-700 loader/map to the other System 12 board variants,
  bank schemes, larger ROM/RAM sizes, link/gun/JVS/CDXA devices and persistence.
- Add 2 MiB arcade save-state serialization. Existing console GPU freeze
  storage still describes 1 MiB; the harness does not offer arcade save states.
- Keep dynarec disabled until arcade mappings and invalidation are supported.
  The current adapter is not wired into the libretro/Vita frontends.

The broader goal covers every machine in
[the pinned MAME PSX inventory](mame-psx-systems.md), rather than only Soul
Calibur or Namco System 12. Standalone CAT702 security tests are available for
future ZN integration; a tested chip is not a verified arcade board.

Reference driver: [MAME namcos12.cpp at the pinned source revision](https://github.com/mamedev/mame/blob/deb2f54fe7300e7a192690318beb860159bfd52a/src/mame/namco/namcos12.cpp).
The board configuration, ROM declarations, banking and DMA are derived from
that BSD-3-Clause file by smf; attribution and license are retained in
`THIRD_PARTY_NOTICES.md`. MAME's PSX DMA implementation is also useful for
matching the register behavior (`src/devices/cpu/psx/dma.cpp`).

## Vertical timing audit

The pinned `coh700` and `coh716` source configurations both supply a
100,000,000 Hz clock to the main PSX CPU. The MAME PSX interpreter converts
four input clocks to one execution cycle and retires one instruction per
execution cycle in its main loop. Its nominal instruction budget is therefore
25,000,000 instructions per second under that source model.

The default native harness retains the PCSX console counter clock of
33,868,800 Hz and the default interpreter cycle multiplier of 175 (1.75
counter cycles per instruction). This gives about 19,353,600 instructions per
second before exceptions and other execution effects. These are two different
emulation timing models; the earlier match demonstration does not establish
that CPU-to-video timing matches MAME. H8 and C352 clocks are scheduled against
the PCSX counter timebase, so replacing only the CPU instruction multiplier
would require a deliberate comparison of the entire board clock model.

An opt-in `SYSTEM12_TIMING=1` build now selects a 50 MHz counter timebase and
cycle multiplier 200 for System 12, giving the source's nominal 25 million
instructions/second. It derives frame duration from the active GPU PAL/NTSC
and interlace bits, with MAME's 2.5 ms vblank pulse. This remains an experiment;
it does not claim cycle-accurate CPU stalls, GPU timing or serial waveforms.
Both default and experimental builds completed a 12,000-frame Soul Calibur
replay with visible gameplay, C352 output and zero reported H8 unsupported
instructions or board unmapped accesses. The experimental frame, H8 and audio
counts follow MAME's active 59.8260978565 Hz interval, but the captures do not
show identical game states at equal frames. See the repeatable
[gameplay comparison](system12-gameplay-test.md) for counts and limitations.

Vertical System 12 work now prioritizes a repeatable sustained-gameplay replay,
control coverage and CPU/GPU/H8/C352 timing comparison with MAME. Other board
ports remain active independently. The next timing change must preserve the
console and other arcade board configurations and revalidate the authentic
Soul Calibur replay, rather than infer fidelity from boot success.
