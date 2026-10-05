# G-Net flash-bank / Zoom composition

`libpcsxcore/arcade/gnet_zoom.cpp/.h` composes the existing standalone Zoom
sound board with the source G-Net main-CPU flash bank and Intel x16 NOR engines.
The source reference is MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`, primarily
`src/mame/sony/taitogn.cpp`, `src/mame/sony/taito_zm.cpp`, and
`src/devices/machine/rf5c296.cpp/.h`.
`taitogn.cpp` declares the flash and EPROM regions `ROM_REGION16_LE`; source
ROM bytes are therefore retained in little-endian word order at this API.

The config borrows mutable caller buffers for the 2 MiB TE28F160 firmware
flash, 512 KiB E28F400B Zoom program flash, and three 2 MiB TE28F160 sample
flashes. It also borrows the 1 MiB EPROM as read-only and takes the JP1 strap.
Internally each x16 NOR has a high-byte-first word image; the adapter swaps
pairs on ingress/egress so PSX bus reads and the caller's raw buffers retain
little-endian order. Successful NOR writes become visible in the caller's
buffer immediately, including the source reset-release `0xff` commands. The
NOR engine reports the precise changed byte span, so ordinary program words
copy back only their two changed bytes; erase operations copy the erased block.

The PSX window is 16-bit at `1f000000–1f7fffff`; bank selection is
`BIT(control,2) | (JP1 << 1)`, with JP1 choosing the initial upper bank bit.
The control byte is at `1fb40000`: bit 4 holds the sound board in reset, bit 5
clocks the source MB3773 watchdog, and bit 2 selects the flash bank. The
watchdog is advanced on the same 6.25 MHz MN10200 internal-cycle timeline as
Zoom and NOR. Its five-second timeout requests a deferred soft reset through
the caller callback; the host must schedule the actual system reset after the
current callback. As in MAME, only a falling edge on bit 5 rearms the timer.
Reset release applies the Zoom device reset and sends `0xff` to its program and
three sample flashes, matching MAME. Bank 0 maps firmware, the PCMCIA memory
window, and Zoom program flash. Bank 1 and bank 3 map the three sample flashes,
with the source `0x10000000` mirror. Bank 2 maps the read-only EPROM, mirrored
Zoom program flash, and firmware. Bank 0's PCMCIA memory window delegates to
the optional RF5C296 PC Card callbacks when supplied; without callbacks it
continues to return `GNET_ZOOM_UNSUPPORTED`. Unmapped and unaligned accesses
return explicit result codes.

`gnet_zoom_bus_read/write` routes the rest of the common `taitogn_state` PSX
map: byte controls at `1fa30000` and `1fb40000`, write-only control2 at
`1fb60000`, the constant-2/no-op register at `1fb70000`, Zoom register/data and
sound-IRQ ports at `1fb80000`–`1fbc0000`, and shared RAM at `1fbe0000`. Shared
RAM honors MAME's `.umask32(0x00ff00ff)`, including open-bus bytes at lanes 1
and 3. Flash accesses remain aligned 16-bit operations. The router takes
physical addresses and does not advance time itself; the host synchronizes the
common MN10200 cycle domain before each transaction. Unsupported register
widths fail explicitly.

`rf5c296.cpp/.h` ports the pinned MAME controller's offset doubling, selected
low/high-byte register pair (`1fb003e0/1fb003e1`), zero-valued selected-register
reads, and register-3 bit-6 card-reset behavior. Its optional callbacks mirror
MAME's PC Card `read_memory/write_memory`, `read_reg/write_reg`, and `reset` calls.
The callback descriptor is copied at board creation, while its opaque state is
borrowed for the board lifetime. Controller I/O doubles the supplied word
offset (and increments it for the high byte) before calling `read_memory` or
`write_memory`; the banked memory window passes its x16 word offset unchanged
to `read_reg` or `write_reg`. Callbacks run synchronously and must not reenter,
synchronize, or destroy the G-Net board; an attempted destroy during a callback
returns `GNET_ZOOM_BUSY` and can be retried after the callback returns.
The card attribute aperture accepts byte and aligned word/dword accesses.
Bytes preserve the low/high lane mask; dwords delegate two little-endian x16
transactions. Flash/EPROM widths and no-card behavior remain unchanged.
The synthetic fixture verifies both PSX windows, byte-lane forwarding, selected
register behavior, reset, and destroy/retry behavior from read and reset
callbacks. No ATA task-file engine, CIS/card implementation,
Taito unlock/security, mounted media, or real game boot is included; the native
G-Net adapter still supplies no card callbacks.

This reusable sub-board is now composed with the native Sony ZN G-Net adapter
described in `gnet-native.md`. That adapter loads the two BIOS-root profiles,
routes executable MIPS accesses, and exposes Zoom PCM, but still supplies no
game card. Per-game media and security, source main-CPU timing and frontend
Zoom audio integration remain required for real game support. The standalone
tests cover synthetic register, flash, and callback-backed PC Card transactions
only.

The sound CPU continues to see the Zoom program flash as read-only:
`taito_zm.cpp` maps `0x080000–0x0fffff` through the flash device's read handler
and installs no write handler there. Writable flash programming is through the
G-Net main-CPU bank window, whose source map uses read/write handlers for all
five x16 chips. Tests exercise an Intel program sequence through that bus and
execute a synthetic MN10200 program from the program-flash callback; the guest
attempted write to its own program map is ignored. These fixtures use generated
in-memory images. They verify the adapter and its maps, not a game boot.

Zoom and NOR time share the MN10200 internal 6.25 MHz cycle domain. Integer
conversion carries the fractional remainder for `4/25` microseconds per
internal cycle. The host must synchronize to an absolute monotonic cycle before
main-bus accesses. The Zoom scheduler calls the G-Net time hook at each
MN10200-domain sync point before advancing ZSG-2/TMS or emitting audio frames.
Thus observers at a sample edge see NOR status at that same source timestamp,
instead of the time only advancing at the end of a run quantum.
The MN10200 source map exposes only its own program flash, not the G-Net
firmware or sample NORs, so it cannot poll those chips directly. The timing
fixture leaves the MN10200 executing a loop from its program flash while the
main-bus test reads the firmware NOR's busy/ready status across the erase
deadline. An audio-frame callback independently observes the busy-to-ready
transition, compared between one large time slice and twenty subdivisions.
Partial x16 lane writes and ATA/card media are not implemented. `gnet_zoom_try_destroy`
returns `GNET_ZOOM_BUSY` if called reentrantly from an audio callback or while
the Zoom child is synchronizing; retry destruction after the callback returns.
The adapter does not include the PSX core, CAT702, full G-Net board, or real
media package. The synthetic test verifies the MB3773 five-second expiry and
falling-edge kick through the G-Net control register; it does not assert a game
boot.

Run the focused tests with:

```sh
make -f tests/Makefile.gnet-zoom test
make -f tests/Makefile.gnet-zoom sanitize
```
