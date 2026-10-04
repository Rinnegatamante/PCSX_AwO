# Sony ZN MCU serial device

`libpcsxcore/arcade/zn_mcu.{c,h}` is a standalone C99 port of MAME's
`src/mame/sony/znmcu.cpp/.h` from the pinned revision
`deb2f54fe7300e7a192690318beb860159bfd52a`. This device is HLE code in MAME;
the source has no MCU firmware to execute. The port reproduces the exposed
SELECT/SCK/DSR/TXD protocol and samples only the digital-switch, analog, and
trackball callbacks declared by that device. It does not add game-specific
commands or responses.

The callback defaults match MAME: DSW returns `0xff`, analog channels return
`0xff`, trackball counters return zero, and output callbacks are disconnected.
Creation drives DSR and TXD high. Falling SELECT starts a transaction, resets
the bit/byte cursors, and schedules a DSR-low event 50 μs later. That event
samples a packet and schedules DSR high 5 μs later. Data is sent LSB first on
SCK falling edges. SELECT high drives both outputs high and cancels a pending
timer. There is no MAME `device_reset` override; the standalone API therefore
has no reset routine that would discard pending timer or packet state.

| Mode | Header and payload |
| --- | --- |
| Digital | Header is `(1 << 4) | (DSW & 0x0f)`; remaining send bytes retain their zero-filled state |
| Analog | Analog mode takes priority if both read selectors are asserted. Header reports 8 data bytes, sampled from channels 0–7 |
| Trackball | Header reports 6 bytes. Four unsigned 16-bit position deltas wrap modulo 65536 and pack low bytes plus high nibbles into the MAME wire layout |

The first byte is sampled at the initial DSR-low timer callback. Each completed
byte schedules the next 50 μs DSR pulse only while the pre-increment byte index
is less than the mode's data-byte count. The absolute-microsecond scheduler
processes every timer event at its exact deadline and rejects clock rewinds.

Run the standalone protocol tests with:

```sh
make -f tests/Makefile.zn-mcu test
```

Fixtures check the digital header and zero-filled tail, exact 50/5 μs timing,
SELECT cancellation, LSB-first output, analog priority and sample order,
trackball packing and unsigned wraparound, history between transactions, and
MAME's callback defaults. The derived C implementation retains MAME's
BSD-3-Clause notice and copyright holder `smf`.
