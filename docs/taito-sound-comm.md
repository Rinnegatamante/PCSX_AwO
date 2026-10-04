# Taito sound command link

`taito_sound_comm` implements the shared four-bit command-link behavior used
by the TC0140SYT and PC060HA devices. In the pinned MAME source,
`pc060ha_device` derives from `tc0140syt_device` without changing its
protocol. This module therefore models that common device logic; it does not
provide a sound CPU, YM chip, or audio generation.

The master side selects a mode, then accesses the communication register. Modes
0–3 transfer four low nibbles. Writing mode 1 or 3 marks the corresponding
master-to-slave pair full; reading mode 1 or 3 on the slave clears that full
bit. The slave-to-master direction uses status bits 2 and 3, cleared when the
master consumes modes 1 and 3. Mode 4 reads status on either side. Master mode
4 writes drive the sound CPU reset callback. Slave mode 5 disables NMI and
mode 6 enables it; the NMI line is active when enabled and either incoming
master-to-slave full bit remains set.

Master communication accesses invoke the optional `request_synchronization`
callback to request a scheduler barrier before the latch operation. MAME
services that request after the current master operation returns. The host
must enqueue the request and yield afterwards; the callback must not execute
the slave inline or reenter the device. This preserves the source's ordering
point rather than running the slave against the old latch value. The device has no independent timer. Reads with side
effects disabled do not advance modes or clear status and do not invoke the
synchronization callback. Reset clears the local modes, latches, status, and NMI-enable
state; as in MAME, it does not drive callbacks during reset. Missing callbacks
are disconnected, matching the device's default no-op callback behavior.

The implementation follows MAME's `src/mame/shared/taitosnd.cpp` and
`taitosnd.h`, copyright Philip Bennett, under BSD-3-Clause. PC060HA uses the
same implementation in that source. Standalone protocol traces are covered by
`make -f tests/Makefile.taito-sound-comm test`.
