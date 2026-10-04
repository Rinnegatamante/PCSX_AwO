# Panasonic MN10200 / MN1020012A standalone core

This module adapts the current MAME MN10200 instruction engine and internal I/O implementation. The pinned source is `src/devices/cpu/mn10200/mn10200.cpp` and `.h` at MAME commit `deb2f54fe7300e7a192690318beb860159bfd52a`. The original source credits Olivier Galibert, R. Belmont, and hap; its BSD-3-Clause terms are retained in `libpcsxcore/arcade/mn10200_LICENSE.txt`.

The API models the MN1020012A variant used by Taito Zoom/G-Net: 24-bit little-endian byte addresses, reset PC `0x080000`, and a CPU-internal register overlay at `0x00fc00–0x00ffff`. The overlay takes precedence over the external memory callbacks. Word reads and writes align down to an even address, matching the MAME program-space helpers. Reads from an unmapped caller address return `0xff` if no callback is installed; unmapped writes are ignored in that case.

Call `mn10200_run` in internal CPU cycles. MAME's device clock conversion divides the input clock by two for the execution cycles. For Taito Zoom's 12.5 MHz input, the internal cycle rate is 6.25 MHz. The API returns consumed cycles, including instruction/interrupt overrun beyond the requested budget, as the MAME execute loop does. It can be called in slices; the instruction engine's state is retained across calls. Bus and port callbacks can query `mn10200_get_callback_cycle` for the absolute internal cycle at which that transaction occurs. Timer 0–9 deadlines use the source relationship `(prescaler base + 1) * (timer count + 1)` internal cycles and are processed at instruction boundaries, including the source's instruction-overrun behavior.

The current MAME CPU source implements IRQ pins 0–3, configurable active level/edge detection, IRQ groups and priorities, the fixed interrupt vector `0x080008`, stack writes, and the illegal-op NMI request. The internal simple timers 0–9 and their prescaler/cascade behavior are modeled. Full register read/write handling is retained for DMA, serial, GPIO, and other internal blocks.

MAME's MN10200 implementation does not perform DMA transfers or serial bit timing: it stores the corresponding registers and readback state. The later timer/counter and ADC register blocks likewise contain stubs rather than execution engines. This module preserves those source-visible register behaviors and does not invent activity for them. External memories and peripherals such as ZSG2 are owned by the board adapter and are reached through the caller's byte callbacks.

Bus/port callbacks must not call `run`, `reset`, or `write_internal` reentrantly. Such attempts fail and set a diagnostic fault. IRQ input changes are allowed from callbacks. Destroying the CPU from a callback is forbidden; the destructor leaves the instance intact if called there. Read-only getters are safe. Fault codes are diagnostic: 2 callback recursion, 3 timer deadline overflow, 4 forbidden/reentrant mutation, 5 cycle horizon overflow, 6 invalid/non-progressing run, and 7 invalid IRQ line.

Build and run the standalone synthetic vectors with:

```sh
make -f tests/Makefile.mn10200 test
make -f tests/Makefile.mn10200 sanitize
```

The tests exercise immediate and register arithmetic, external byte bus accesses, instruction-slice state, timer expiry/readback, IRQ line sensing and vector stack entry, GPIO read callbacks, callback timestamps/reentry, and cycle overflow. They establish CPU-engine behavior only; they do not claim a Taito game boots or its board peripherals are integrated.
