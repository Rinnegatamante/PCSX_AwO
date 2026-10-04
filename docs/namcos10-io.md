# Namco System 10 base I/O

`libpcsxcore/arcade/namcos10_io.{c,h}` is a standalone C99 implementation of
the common base-board I/O handlers in the pinned MAME
`src/mame/namco/namcos10.cpp` revision
`deb2f54fe7300e7a192690318beb860159bfd52a`. Its image-free callbacks let a
board adapter provide the MAME `SYSTEM` and `IN1` input-port values and receive
the base timer's INT10 assertion. The module does not depend on the System 10
board adapter or frontend.

The bus API accepts physical PSX addresses, byte/halfword/word widths and
little-endian CPU values. KSEG aliases are canonicalized. Partial write masks
use MAME's ones-mean-written `COMBINE_DATA` convention for the two I2C host
registers.

| Address | MAME behavior |
| --- | --- |
| `1fba0000` | 16-bit `SYSTEM` input callback; defaults to all pull-ups (`ffff`) |
| `1fba0002` | Write resets the serial EXIO identifier cursor; reads shift the configured 8-bit ident byte, duplicated on result bits 0 and 1. `ff` means no EXIO |
| `1fba0004..7` | 32-bit active-low `IN1` callback; defaults to all pull-ups (`ffffffff`) |
| `1fba0008`, `1fba000a` | I2C clock/data wire reads and host-line writes |
| `1fba0012..19` | Four write-only interrupt acknowledgements, one pending bit per halfword offset |
| `1fba001a` | Pending interrupt bits; timer I/O event is bit 3 |
| `1fb20000..03` | Write no-op used around DMA completion |
| `1fb60000..03` | Watchdog no-op read/write; MAME's default unmap read value is zero |

The I2C behavior deliberately mirrors MAME's base handler: it detects START
and STOP edges, shifts each transmitted byte, pulls device SDA low for an ACK,
then releases SDA on the ACK falling edge. The driver has no backing I2C
EEPROM or read-data device, so the model does not store bytes or fabricate
readback. No CAT702 is wired in the pinned System 10 base map. The ATA window
at `1fbc0000` and CD status window at `1fbf0000` belong to separate devices and
remain unmapped for their own adapters.

`ns10_io_advance_us` supplies the MAME 100 Hz timer. The timer starts with an immediate event on the first advance call (including
a zero-time advance), matching `timer_device::device_reset` and its default
zero start delay. Every subsequent elapsed 10 ms tick
sets pending bit 3 and invokes the optional INT10 callback with `asserted=1`;
the surrounding PSX interrupt controller owns line acknowledgement. Device
reset clears the I/O pending bits and serial state and reanchors the timer,
including its immediate initial event, as MAME resets the timer device.
`ns10_io_cdrom_interrupt` forwards the external ATA/CD IRQ to pending bit 2
and INT10 exactly as the source callback does; it does not emulate the drive.

Run the synthetic register and protocol checks with:

```sh
make -f tests/Makefile.namcos10-io test
```

The tests cover input callbacks and byte lanes, KSEG mirrors, EXIO identifier
shifting with and without a daughterboard, the MAME I2C ACK trace without
readback storage, 100 Hz pending/INT10 behavior, interrupt acknowledgements,
and both no-op windows. The derived C source retains the upstream BSD-3-Clause
notice and copyright holders `smf,windyfairy`.
