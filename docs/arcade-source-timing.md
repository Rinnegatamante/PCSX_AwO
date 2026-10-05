# Arcade PSX source timing

MAME's PSX CPU device converts four input clocks into one execution cycle, and
the CPU interpreter consumes one execution cycle per instruction. PCSX charges
two PSX cycles per interpreted instruction when `Config.cycle_multiplier` is
200. The source-cycle clock is therefore half the board's configured CPU input
clock: PCSX's two cycles per instruction at that rate match MAME's input/4
instruction rate. This is why the 67.7376 MHz boards use a 33.8688 MHz PCSX
clock, while a 100 MHz board uses 50 MHz.

With `PCSX_ARCADE_SOURCE_TIMING`, the core selects source clocks for native
profiles that currently expose or have a fixed MAME source clock:

| Native profile family | MAME CPU input clock | PCSX cycle clock |
| --- | ---: | ---: |
| ZN1, G-Net COH1002, Taito FX-1, Game Vector, Namco System 11 | 67.7376 MHz | 33.8688 MHz |
| ZN2, G-Net COH3002 | 100 MHz | 50 MHz |
| Namco System 10 | 101.4912 MHz | 50.7456 MHz |

System 12 remains controlled by `PCSX_SYSTEM12_SOURCE_TIMING`, independently
of the generic arcade flag. When enabled, System 12 uses its 100 MHz MAME CPU
clock and 50 MHz PCSX cycle clock. When disabled, its existing compatibility
clock behavior is retained. An inactive or unrecognized board always uses the
33.8688 MHz console clock. Timing selection is cached at `psxInit` and
`psxReset`; a source-timed board uses cycle multiplier 200, and the prior
configured multiplier is restored when returning to console timing.

The PSX GPU source refresh is selected from GPU status, independently of the
main CPU clock: PAL is nominally 50 Hz; NTSC 240-line mode is 59.8260978565 Hz;
NTSC 480-line mode is 59.9400523286 Hz. Those NTSC values are the pinned MAME
GPU's 53.693175 MHz-derived screen rates. Counter/frame timing and frontend FPS
use the live GPU status bits while a source profile is active.

The configured CPU rate is not a shared peripheral oscillator. MAME configures
SPU, C352, H8, MCU, SCSI, ATA and other devices at their own rates. Core board
bridges use the selected PCSX cycle clock only to convert elapsed main-CPU
cycles into wall time or secondary-device work; their own device rates remain
independent. For example, System 12's H8 remains 16.9344 MHz and C352 remains
88.2 kHz while the main CPU cycle clock is 50 MHz.

Clock inputs are grounded in the pinned MAME source (`deb2f54fe7300e7a192690318beb860159bfd52a`): `src/devices/cpu/psx/psx.h` defines the common CPU clock/cycle conversion; `src/mame/sony/zn.cpp` defines ZN1/ZN2 and Taito FX-1 base clocks; `src/mame/sony/taitogn.cpp` inherits ZN1/ZN2 for COH1002/COH3002; `src/mame/konami/konamigv.cpp` defines Game Vector; `src/mame/namco/namcos11.cpp`, `namcos12.cpp`, and `namcos10.cpp` define the three Namco families. Runtime profiles supply G-Net and ZN clocks; fixed clocks are used only where the source configurations establish a single CPU input rate.

The currently selected horizontal totals (263 NTSC / 314 PAL) remain the
existing PCSX counter model. MAME's GPU source here configures the screen from
refresh and visible area, but that alone does not establish exact total
horizontal scan counts. Matching those counts and validating timing against
runtime MAME traces remain follow-up work. Unsupported families without an
active native profile (including Twinkle, System 573, and GQ) retain the
console timing fallback; this does not claim those boards are source-timed.

The first System12 runtime vblank comparison now demonstrates that matching
refresh and blank duration does not match screen phase. Native source timing
places start 2.5 ms before its frame boundary; MAME's initial start occurs at
the frame-period boundary, with end 2.5 ms later. See the measured failing gate
in [system12-performance.md](system12-performance.md#measured-mac-vblank-discrepancy-2026-10-05).
Other families using the common counter bridge have not been validated by that
System12 capture; their screen/device contracts need corresponding fixtures.

Focused timing checks can be run without building a frontend:

```sh
for mode in default legacy arcade arcade-s12; do
  defs='-DPCSX_SYSTEM12 -DPCSX_SYSTEM_GNET -DPCSX_SYSTEM_GV'
  case "$mode" in
    legacy) defs="$defs -DPCSX_SYSTEM12_SOURCE_TIMING" ;;
    arcade) defs="$defs -DPCSX_ARCADE_SOURCE_TIMING" ;;
    arcade-s12) defs="$defs -DPCSX_ARCADE_SOURCE_TIMING -DPCSX_SYSTEM12_SOURCE_TIMING" ;;
  esac
  cc -std=c99 -O0 -ffunction-sections -fdata-sections -Iinclude -Ilibpcsxcore \
    $defs tests/test_arcade_timing.c libpcsxcore/psxcommon.c \
    -Wl,-dead_strip -lm -o "/tmp/pcsx-timing-$mode" && "/tmp/pcsx-timing-$mode"
done
```
