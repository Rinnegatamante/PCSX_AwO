# YM2610 and YM2610B sound core

This standalone wrapper embeds the pinned MAME YMFM engine and exposes its
YM2610 and YM2610B variants through an explicit model selection. It models the
chip only. A host supplies the input clock, ADPCM sample-ROM reads, and IRQ
consumer; Z80 execution, Taito command latches, board pan wiring, and game media
remain separate.

The engine uses the upstream `ymfm::ym2610` or `ymfm::ym2610b` constructor.
The B model changes the FM channel mask from `0x36` to `0x3f`; it is not
approximated by changing a register in the non-B chip. The wrapper sets
`OPN_FIDELITY_MED`, matching MAME's OPN wrapper. MAME allocates its stream at
the integer sample rate `clock_hz / 144`; a rational phase accumulator preserves
that rate when advancing input clocks. At 8 MHz, one second produces 55,555
frames and the first frame completes at clock 145. Samples are clamped to
signed 16-bit. Output ordering follows
MAME's SSG wrapper: SSG mono, FM left, FM right.

`ym2610_run_cycles` is the virtual clock. It advances both output generation
and the engine's timers in the chip's input-clock domain. Timer durations come
from YMFM's register engine; the wrapper expires them at the requested cycle
deadline, updates status, and forwards IRQ transitions. Busy state ends at its
YMFM deadline. Bus reads/writes occur at the current virtual cycle, so a host
must run cycles to the bus timestamp before accessing the chip. Reads preserve
the upstream register semantics, including the ID byte and extended status.
Reset resets the YMFM chip and cancels its timers while preserving the host's
absolute cycle count and output phase.

ADPCM-A and ADPCM-B use separate borrowed byte regions or an optional host read
callback. The pointers are never copied or freed. If only A is configured, the
B address space aliases A as MAME's wrapper does. Addresses outside a borrowed
region return zero, matching MAME's default unmapped memory-space read. Both
spaces mask addresses to 24 bits. No synthetic
sample data is installed by the device. ADPCM-B recording writes are not
provided, matching the read-only ROM memory maps used by the YM2610 boards in
scope.

The Taito FX-1S machine in pinned `src/mame/sony/zn.cpp` uses YM2610B at
8 MHz and its Z80 at 4 MHz. MAME routes SSG mono to both speaker channels and
the two FM outputs to left and right. Select `YM2610_MODEL_YM2610B` for that
board. The non-B model remains available for hardware that actually uses
YM2610. This chip port alone does not establish complete FX-1S audio or game
emulation.

The wrapper follows MAME `src/devices/sound/ymopn.cpp/.h` (Aaron Giles,
BSD-3-Clause) and embeds the unmodified YMFM engine sources in
`libpcsxcore/arcade/ymfm/` (Aaron Giles, BSD-3-Clause; full terms in
`ymfm/LICENSE`). Only the OPN, FM, ADPCM, and SSG source units needed by the
YM2610 family are included. Synthetic standalone tests cover FM and SSG tones,
variant-specific channel masks, timer/IRQ/busy timing, borrowed and callback
ADPCM regions, reset, and cycle-slice equivalence:
`make -f tests/Makefile.ym2610 test`.
