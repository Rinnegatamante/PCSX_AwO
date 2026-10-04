# Fujitsu MB3773 watchdog

`arcade/mb3773.c` retains the behavior of MAME's
`src/devices/machine/mb3773.cpp` at pinned commit
`deb2f54fe7300e7a192690318beb860159bfd52a` (copyright smf, BSD-3-Clause).
MAME uses a fixed five-second one-shot watchdog; its source explicitly leaves
parameter-based timeout calculation as future work. This adapter uses that
same timeout.

Creation starts CK low and arms the initial reset deadline. Only a falling
CK edge rearms it. Device reset rearms while retaining CK. Expiry requests a
soft machine reset once; the host schedules it after the current bus/scheduler
operation returns. The callback must not reset or destroy the device inline.
Absolute microsecond time rejects backwards movement and deadline overflow.
Counters are diagnostics, not an additional hardware register.

The Taito FX base map drives CK from bit 5 of its ROM-bank register at
`0x1fb40000`; bits 0–1 select an 8 MiB bank. Board wiring remains necessary.
This device alone does not establish game execution.

`make -f tests/Makefile.mb3773 test` verifies exact deadlines, edge polarity,
one-shot expiry, reset retention and deferred host reset requests.
