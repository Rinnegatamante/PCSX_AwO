# PlayStation SIO port

`libpcsxcore/arcade/psx_sio.{c,h}` is a standalone C99 port of the register,
serial shifter, status, IRQ, and timer behavior in the pinned MAME
`src/devices/cpu/psx/sio.cpp` and `sio.h` (commit
`deb2f54fe7300e7a192690318beb860159bfd52a`). The original source credits smf
and is copyright 2003–2011 under BSD-3-Clause; the complete retained notice is
in `libpcsxcore/arcade/psx_sio_LICENSE.txt`.

The caller creates SIO0 or SIO1 and supplies optional IRQ, SCK, TXD, DTR, and
RTS line callbacks. RXD and DSR are input pins. Pin callbacks can query
`psx_sio_elapsed_cycles`; during multi-deadline catch-up they see each exact
event cycle before the scheduler advances to the requested target. Register
offsets are MAME's 32-bit word offsets from the SIO base: 0 is TX/RX data, 1 is status, 2
contains mode in its low half and control in its high half, and 3 contains the
baud counter in its high half. Offsets repeat modulo four. Writes use MAME's
one-bits-mean-written `mem_mask`; the source's TX data register remains
unconditional even when its mask is zero.

Each serial bit is timed in absolute 33.8688 MHz PSX cycles. Mode low bits
select divisors 1, 16, or 64; the baud counter multiplies that divisor. The
device schedules the next one-shot deadline whenever MAME calls
`sio_timer_adjust`, and callers advance deadlines before each bus transaction.
Writing mode or baud does not arm a stopped timer by itself, matching the
source. Control RESET is the only register reset operation; there is no
separate MAME `device_reset` callback.

SIO0 sends eight LSB-first bits, toggles SCK low/high for each bit, and samples
the RXD pin at the same bit deadlines. It raises TX-complete/RX-ready IRQs per
the source masks, preserves the first byte on overrun, and clears IRQ only on
IACK or control reset. DSR asserts on a falling pin transition and can raise
its own IRQ. SIO1 transmits through TXD but does not toggle SCK or initialize
the receive shifter in this MAME source. The RTS callback is present in MAME's
device configuration but is never driven by the source implementation; this
port likewise leaves it untouched.

The [native Capcom ZN adapter](capcom-zn-native.md) owns both SIO blocks,
connects SIO0 pins to the MCU/CAT702 signal merger, and routes IRQs to the
PSX core. Generated MIPS guests verify received MCU headers and IRQ7.
This standalone port does not
provide controller protocol responses, serial storage, or game-specific
handshakes.

Run the independent protocol and timing tests with:

```sh
make -f tests/Makefile.psx-sio test
make -f tests/Makefile.psx-sio sanitize
```

The fixtures verify exact baud deadlines and callback timestamps across a
single multi-edge catch-up, LSB-first `0xb3` transmission and
`0x62` RX sampling, overrun preservation, masked-register semantics, DSR edge
IRQ, IACK, reset-line ordering, TX-data overwrite behavior, disabled SIO1 RX,
and invalid/rewound timer handling. They exercise the standalone source only;
they do not claim that a ZN board or game boots.
