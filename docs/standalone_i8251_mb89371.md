# Standalone i8251 and MB89371

These modules provide a C implementation of the MAME i8251 USART and the
MB89371 dual-UART wrapper for board drivers. They are standalone peripherals;
they do not create a game system, attach media, or imply that any title boots.

The implementation follows MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`, principally
`src/devices/machine/i8251.cpp`, `i8251.h`, `mb89371.cpp`, `mb89371.h`,
`src/emu/diserial.cpp`, `diserial.h`, and `src/devices/machine/clock.cpp`.
Source files retain the original MAME BSD-3-Clause license markers and
copyright-holder notices; the combined full terms and all upstream credits
are in `libpcsxcore/arcade/i8251_mb89371_LICENSE.txt`. The source credits also
include Carl, Miodrag Milanovic, Vas Crabb, and hap through the shared serial
and clock interfaces. MAME source is available at
<https://github.com/mamedev/mame/tree/deb2f54fe7300e7a192690318beb860159bfd52a>.

`i8251.h` exposes an opaque device, register reads and writes, input pin setters,
and synchronous callbacks for TXD, DTR, RTS, RXREADY, TXREADY, TXEMPTY, and
SYNDET. `mb89371.h` exposes two independently addressable UART channels, the
four wrapper registers per channel, pin setters, output callbacks, and a
monotonic `mb89371_advance(now_ns)` clock source. Input transitions and output
callbacks are synchronous. Callback storage is borrowed, so the callback user
object must outlive the peripheral. Reentrant register writes, clock
advancement, reset, or destroy calls from a callback are ignored; MB89371's
internal loopback uses a dedicated receiver-line hook and remains active.

The i8251 implements the mode/command/sync-character programming phases,
internal reset phase, asynchronous x1/x16/x64 sampling, synchronous data and
sync hunt, parity and framing errors, overrun, break, DSR/CTS handling, and
status/readiness outputs. Sync parity follows the mode-register bits, while
async parity uses the retained generic frame format, matching the separate
MAME code paths. TX serial bits advance on falling TXC edges; RX uses
rising RXC edges. Async RX starts on a sampled high-to-low transition and
samples the first data bit after the MAME 1.5-bit start delay. A frame with
zero configured stop bits completes without a stop sample. The i8251 external
clock API is edge driven and has no wall-clock dependency.

Reset follows MAME's register lifecycle: it clears the USART status/command and
data registers and resets generic TX/RX progress flags, while preserving frame
format, the receive serial history, and synchronous programming state. Reset
callbacks occur in source order: TXD, RTS, DTR, TXD/TXEMPTY, RXREADY, TXREADY.
The generic serial frame starts with zero data bits, and mode programming
updates it without assigning a guessed reset format.

The MB89371 starts with mode bytes `0xf0` and baud bytes `0x00`. Its reset
resets only the two child i8251s, matching MAME's lack of a wrapper reset
override. Internal baud rate is `floor(master_clock_hz / (2 << baud))`; a zero
rate disables the generator. The generator starts low and schedules its first
toggle after a half-period. It processes both channels in chronological event
order; `mb89371_time_ns()` reports the due edge timestamp inside callbacks. If
a period changes, an already pending edge remains unless the new half-period
would occur first, matching `clock_device::reinit`'s shorten-only behavior. The
source's TXEMPTY/SYNDET callback mask quirk is preserved: both use mode bit 5.
RXREADY uses bit 6; TXREADY uses bit 5.

The MAME expression `2 << baud` has undefined behavior for baud values 30 and
above because it shifts a signed integer into an unrepresentable result. This
port treats those values as disabled and sets `MB89371_FAULT_INVALID_BAUD`. A
zero master clock is rejected at creation; an internal divisor that produces a
zero-Hz rate sets `MB89371_FAULT_ZERO_RATE`. `mb89371_faults()` also reports
backward time, time overflow, and subnanosecond clock quantization. The
nanosecond clock uses rational phase accumulation and clamps any zero-nanosecond
interval to one nanosecond while reporting the quantization fault. Each advance
call processes at most one million edges; a larger backlog reports
`MB89371_FAULT_EVENT_BUDGET` and leaves virtual time at the last processed edge
so the caller can continue advancing to the same deadline. Callers can inspect
these sticky fault bits and reject a configuration when time resolution or
event backlog is outside their limits.

From the repository root, run the literal pin/register tests with
`make -f tests/Makefile.i8251 test` and
`make -f tests/Makefile.mb89371 test`. Run both with address and
undefined-behavior sanitizers through each Makefile's `sanitize` target; all
generated test binaries, dependency files, and symbols stay under `build/`.
