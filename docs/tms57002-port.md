# Standalone TMS57002 DASP core

This adapter uses the TMS57002 implementation and instruction semantics from
MAME commit `deb2f54fe7300e7a192690318beb860159bfd52a` (Olivier Galibert,
BSD-3-Clause). It retains MAME's core state, decoder and execution logic. The
build runs the upstream `tmsmake.py` generator over the retained
`tmsinstr.lst`; generated execution handlers are written only below `build/`.
The local compatibility layer replaces MAME's device, scheduler, address-space,
sound-stream and callback wrappers. It does not replace the instruction set.

The C API is declared in `libpcsxcore/arcade/tms57002.h`. Host PLOAD and CLOAD
are asserted low, matching `pload_w` and `cload_w` in the source. PLOAD writes
the three-byte ST0, ST1 and 24-bit instruction words. CLOAD takes an address
byte and queues each following four-byte value for the next source access to
that C-memory location. Host `data_r` drains the source's four-byte response
buffer; DREADY reads high when no response is pending. PC0 reads high when the
program counter is nonzero. EMPTY reads high when the CLOAD update queue is
empty. `reset()` follows the device reset method: it resets control counters,
status bits and decode cache while retaining C-memory, D-memory and program
RAM contents.

Call `tms57002_run()` with DSP clock cycles. The caller owns the clock domain;
the Konami GQ source configures the part at 24 MHz and schedules `sync_w(1)` at
48 kHz from the sound board. In this adapter, `tms57002_process_sample()` is
the four-channel synchronous stream boundary: it converts four normalized
float inputs to the source's signed 24-bit serial input representation, returns
the four source outputs normalized by `2^23`, then performs the source
`sync_w(1)` step. It does not execute clock cycles itself. A run budget counts
the exact requested DSP clocks, including idle clocks. The API accepts at most
`INT64_MAX` cycles per call so its signed return value remains representable;
the absolute `uint64_t` clock is checked before any mutation. The optional
`cycle_origin` configuration anchors it to the board clock at creation. `-1` means an
invalid/overflowing budget, and `-2` means a mutating API was called
synchronously from an external-memory or status-line callback. The
`tms57002_current_cycle()` getter reports the in-budget clock position from
inside those callbacks, rather than the end of the current run slice.

The external data port is byte-addressed. The callback receives the address
selected by MAME's ST0 mode and XBA/XOA address generation; absent reads return
`0xff` and absent writes are ignored. Program RAM has 256 24-bit instruction
words and is independent of the 256-word coefficient C-memory. Both retain
contents across device reset. Diagnostic direct program writes mask to 24 bits and invalidate the
decoded-instruction cache. The host PLOAD path retains MAME's write/cache
behavior.

The MAME TMS57002 has no interrupt input or IRQ output callback in this
device implementation. DREADY, PC0 and EMPTY are its exposed host/status lines;
this adapter does not invent an IRQ. It also does not include a 68000, K054539,
board address decoder, sample ROM, or guest DSP firmware. The caller must map
the real GQ external memory regions and provide authentic firmware/media.
Unknown source opcodes set `tms57002_unsupported_instruction()`; source-side
MAME log/UI reporting is not brought into the standalone build.

Run the hand-encoded host/program, arithmetic, memory, audio, callback-time,
reset, and cycle-slicing fixtures with:

```sh
make -f tests/Makefile.tms57002 CXX=clang++ test
make -f tests/Makefile.tms57002 CXX=clang++ sanitize
```

Callbacks must not throw exceptions.

These are synthetic component tests. They do not establish a TMS57002 board
integration or authentic game boot.
