# Twinkle sound-board foundation

This module follows the sound-board map and board wiring in the pinned MAME
`src/mame/konami/twinkle.cpp`. The source identifies the sound CPU as a
16 MHz MC68000, with 512 KiB ROM, 256 KiB RAM, a CY7C131 1 KiB dual-port
mailbox, an RF5C400 at 16.9344 MHz, and 32 MiB of sample RAM. The MC68000's
wave window is 8 MiB; the bank register selects a word offset in 4M-word
increments, as in MAME's handler. RF5C400 external-memory callbacks see the
whole 25-bit byte-addressed RAM.

The board object borrows the exact-size ROM and wave-RAM buffers. Its CPU bus
implements ROM/RAM, mailbox, RF5C400 registers, wave-bank accesses, the LED
latch, and the SPU control register. The SPU control write acknowledges only
the first falling IRQ-enable bit in source order (IRQ1, IRQ2, IRQ4, IRQ6).
The mailbox reproduces the MAME 0x3fe right/main-side interrupt request and
left/sound-side read acknowledge. Only the mapped low byte lane has side
effects; a word read returns `0xff00 | mailbox_byte`, and a high-lane-only
read returns `0xffff`. The ATA-DMA address registers retain the source's
word-index latch, but transfer is unsupported. CPU-to-RF clock conversion
uses the exact 1323/1250 ratio; audio samples are emitted by the existing
RF5C400 callback. The 0x34000e register is source-mapped as write-no-op with
open-bus reads. Soft board reset
leaves board RAM, mailbox, bank/control latches, wave RAM, RF5C400 state, and
absolute timer phase intact; the pinned driver's `machine_reset` resets only
its SCSI DMA state.

ATA CS0 at `0x300000` and the ATA-to-wave-RAM DMA path are explicitly
unsupported. This checkout has no reusable ATA device for this board, so
those addresses fail as unsupported/unmapped bus accesses instead of returning
invented register values or DMA data. The main CPU's SCSI, panel I/O, RTC,
watchdog, FDC, display/video, DVD, security dongle, and cabinet controls are
outside this sound-board component. The periodic 60 Hz IRQ1 source uses exact
integer-cycle deadlines `floor(n * 16,000,000 / 60)`, with intervals of
266666, 266667, 266667 cycles. Board advance is split at each deadline.

The synthetic test runs real MC68000 instructions from a test ROM and checks
RAM, banked wave writes, RF register accesses and nonzero sample callback
data, memory masks, the IRQ4 mailbox handshake, and IRQ1 deadlines under long
and split run slices. Callbacks run synchronously during board advance; do
not reset, mutate, destroy, or otherwise reenter the board from a callback.
These fixtures do not demonstrate a real game boot, ATA operation, HDD sample
loading, or complete sound output. The upstream MAME-derived board wiring is
BSD-3-Clause; full retained terms are in
`libpcsxcore/arcade/twinkle_board_LICENSE.txt`.

The standalone test links the existing M68000 core, which currently requires
C++20 (`std::countr_zero`). The Vita check compiled the Twinkle adapter object
for ARMv7 against the generated M68000 headers; it was a compile check, not a
linked Vita board or runtime test. RF5C400 register programming uses the board
bus API so it always observes CPU/RF clock synchronization; callers cannot
obtain mutable CPU or RF device handles from this adapter.

Run `make -f tests/Makefile.twinkle test` and `make -f tests/Makefile.twinkle
sanitize` for the standalone normal and ASan/UBSan checks.
