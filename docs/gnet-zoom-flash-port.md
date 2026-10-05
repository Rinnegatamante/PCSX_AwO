# G-Net flash-bank / Zoom composition

`libpcsxcore/arcade/gnet_zoom.cpp/.h` composes the existing standalone Zoom
sound board with the source G-Net main-CPU flash bank and Intel x16 NOR engines.
The source reference is MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`, primarily
`src/mame/sony/taitogn.cpp` and `src/mame/sony/taito_zm.cpp`.
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
Zoom program flash, and firmware. The PCMCIA window is explicitly unsupported
because its RF5C296/card implementation is outside this adapter. Unmapped and
unaligned accesses return explicit result codes.

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
Partial x16 lane writes and PCMCIA are not implemented. `gnet_zoom_try_destroy`
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
