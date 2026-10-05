# Standalone H8/3002 core

`h8.cpp` executes the H8/300H ISA in advanced 24-bit address mode, behind the
C interface in `h8.h`. All instruction definitions enabled by MAME's H8/300H
opcode generator are included. Illegal encodings halt with an address/opcode
in `H8Status`; they are never treated as successful shared-memory commands.

The opcode source and generator, instruction semantics, arithmetic flags,
register organization and reset/interrupt sequences derive from Olivier
Galibert's BSD-3-Clause MAME implementation. See `h8_LICENSE.txt`. Run
`python3 libpcsxcore/arcade/h8_generate.py` to regenerate instruction files.

Bus callbacks see big-endian logical H8 bytes. The prepared System 12
`sub.bin` is already in that order: its reset vector is 0x000100.
Optional atomic external 16-bit callbacks preserve word-only peripheral
operations such as the C352 execution register. Internal RAM and H8 register
accesses remain handled by the core.

Implemented peripherals:

- H8/3002 512-byte internal RAM, GPIO data/direction/reserved bits, SYSCR,
  IRQ0..5/NMI controller with edge/level latching and CCR/ICR priority filters.
- Five 16-bit timers with internal clock divisors, compare A/B flags, overflow,
  automatic counter clearing and interrupt generation.
- Two SCI channels with status read/clear semantics, cycle-timed byte transmit,
  receive, receive-overrun and interrupt events. Board code supplies serial
  endpoints and external device behavior.
- H8 ADC3337 software/external trigger, 10-bit input sampling, channel scanning,
  conversion delays, result registers and completion interrupt. System 12
  vblank must drive `h8_adc_trigger` as well as IRQ1; falling ADTRG starts an
  externally configured conversion.

Execution accounts for MAME instruction bus/internal cycle costs.
`h8_run_cycles` completes execution states and carries scheduling overshoot
to the next call. `h8_run_until` uses MAME-generated partial states to suspend
and resume at external absolute deadlines, including within EEPMOV loops.
Checkpoints follow bus operations, so one operation can cross the deadline;
its remaining charge is carried forward without repeating the access. The
caller then delivers external edges before resuming the state.
Peripherals advance between execution checkpoints, including intermediate
prefix decoder and exception states. Internal timer/SCI/ADC deadlines do not
yet establish their own MAME-style partial execution boundaries.

Remaining limitations: DMA and watchdog execution, external timer clock pins,
SCI bit/clock waveforms and external serial clock rates, hardware timer output
waveforms/synchronization, ADC analog power/standby behavior, and save-state
serialization. DMA/watchdog accesses set diagnostic bits in `H8Status`.
Unconnected analog channels return zero; device responses belong in the board
adapter. Timer output/mode reads follow MAME's current placeholder behavior.

Validation:

```
make -f tests/Makefile.h8 test
make -f tests/Makefile.h8 deadline-ubsan-test
make -f tests/Makefile.h8 rom-test
python3 tests/check_mame_h8.py
```

Synthetic tests cover 8/16/32-bit instructions, memory addressing, stack and
subroutine return, IRQ/RTE, atomic word callbacks, timer compare, SCI transmit/
receive, ADC software/external/scan conversions, illegal opcodes and scheduler
slice equivalence, external deadlines, post-access overshoot and partial
continuation without repeated bus accesses. The actual Soul Calibur sub ROM runs through RTC/settings
initialization and enters the same 0x16ca..0x16d2 idle routine observed in MAME;
its shared-memory identification at 0x080400 matches MAME. The independent MAME
oracle captures H8/PSX CPU state, internal registers, shared RAM and stack at
0.1, 1, 2, 10 and 20 seconds. Full system behavior also depends on the main CPU,
board I/O and scheduling, so PC samples are not asserted cycle-identical.
