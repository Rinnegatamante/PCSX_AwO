# H8/3002 execution port

The Namco System 12 adapter executes the actual H8/300H sub CPU ROM through
`libpcsxcore/arcade/h8.cpp`. ISA bodies and arithmetic flags derive from
Olivier Galibert's BSD-3-Clause MAME H8 implementation. The local generator
includes every opcode definition enabled for H8/300H, and illegal encodings
halt with a diagnostic. No shared-memory boot responses are synthesized.

`h8.h` exposes an opaque C interface: create/destroy/reset, run a cycle budget,
set external IRQ/NMI levels, drive ADTRG, deliver SCI receive bytes, and obtain
CPU diagnostics. Config callbacks provide logical big-endian byte accesses,
optional atomic word accesses, GPIO input/output, serial transmit, and raw
10-bit analog input. Prepared `sub.bin` already has logical H8 byte order;
its reset vector is 0x100. Word callbacks are needed for C352 word operations.

The board must independently drive IRQ1 and `h8_adc_trigger` from vblank.
H8 ADC conversion starts on the falling ADTRG edge when ADCR bit7 enables
external triggering. This is required by the real Soul Calibur routine at
0x990, which waits for the ADC completion flag. Soul Calibur's unused analog
inputs default to zero in MAME. The System 12 sub CPU clock is 16,934,400 Hz.

Implemented internal devices include RAM, GPIO, IRQ priority/latching, five
internal-clock timers, two SCI byte interfaces and ADC software/external/scan
conversion. Instruction costs follow MAME bus/internal cycle accounting, with
whole-instruction completion and budget overshoot carried to the next call.
Peripherals advance between instructions; execution is not cycle-exact.

DMA and watchdog execution remain unimplemented and accesses set diagnostic
bits. External timer clock pins, exact SCI bit/clock waveforms and external
clock rates, timer output/synchronization, ADC power/standby and save-state
serialization also remain incomplete. RTC/settings/JVS endpoint behavior is
the board adapter's responsibility. No complete-peripheral claim is made.

`make -f tests/Makefile.h8 test` verifies instructions/addressing/flags,
subroutines/IRQ return, atomic word access, timers, SCI, ADC trigger/scan,
illegal diagnostics and scheduler slicing. Synthetic tests and 120 frames of
the actual sub ROM passed ASan/UBSan with no illegal opcode or unsupported
DMA/watchdog access. The sub ROM completes RTC/settings initialization and
reaches its 0x16ca..0x16d2 idle routine.

`python3 tests/check_mame_h8.py` captures an independent installed-MAME oracle
at 0.1/1/2/10/20 seconds. MAME observes the same idle routine and shared H8
identification words at 0x080400: `5130 324e 3136 3132`; the PSX sees those words
at 0x1f080400. CPU samples vary with scheduling and main-CPU progress. The
oracle records registers, SCI/IRQ/timers, stack, and shared 0x3078/0x3140 regions
in `artifacts/mame/h8-oracle.log`. Early captures found and verified the SCI
status-clear requirement and external ADC-trigger wiring.

See `libpcsxcore/arcade/h8_README.md` for regeneration and details, and
`libpcsxcore/arcade/h8_LICENSE.txt` for attribution and license terms.
