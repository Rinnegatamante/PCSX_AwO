# Taito Zoom / G-Net sound-board adapter

`libpcsxcore/arcade/zoom_board.cpp/.h` wires existing standalone MN10200,
ZSG-2 and TMS57002 devices according to the pinned MAME source at
`deb2f54fe7300e7a192690318beb860159bfd52a`. The board wiring is derived from
`src/mame/sony/taito_zm.cpp/.h` and the G-Net address maps, reset and flash
sample reader in `src/mame/sony/taitogn.cpp`. Source copyright holders are
retained with the full BSD-3-Clause text in
`libpcsxcore/arcade/zoom_board_LICENSE.txt`; each underlying device keeps its
own upstream attribution and license.

The adapter accepts caller-owned flash images: one 512 KiB E28F400B program
flash and three 2 MiB TE28F160 sample flashes. All image lengths are checked
exactly. Reads are little-endian 16-bit flash words. A caller may override
reads and receive the source reset writes through flash callbacks to attach a
stateful flash-device implementation. The Zoom MN10200 map exposes the boot
flash read-only, just as `taito_zm.cpp` does. The G-Net main CPU's writable
flash-bank map is not part of this sound-board module. Intel flash command,
busy/status, erase, and program behavior is not simulated here; without a
caller callback the borrowed images remain immutable.

The MN10200 address map is:

| Address | Source device |
|---|---|
| `080000–0fffff` | Read-only boot flash |
| `400000–41ffff` | 128 KiB sound RAM |
| `800000–8007ff` | ZSG-2, 16-bit word registers |
| `c00000` | TMS57002 byte data port |
| `e00000–e000ff` | 256-byte main/sound mailbox |
| `fc00–ffff` | MN10200 core's internal register block |

MAME issues ZSG-2 register accesses as words. The MN10200 API's optional
external word callback preserves these as one aligned transaction, while the
original byte-only callback path remains unchanged for existing users and
the CPU internal-register overlay continues to take priority.

On the host PSX bus, the board exposes 16-bit accesses at `1fb80000` (register
data), `1fb80002` (register address), `1fba0000` (sound IRQ pulse) and
`1fbc0000` (IRQ status, always zero). Register addresses 4 and 5 set DSP
outputs 2 and 3 gain to `(data & 0x3f) / 63`. The 256-byte mailbox is at
`1fbe0000–1fbe01ff` with source lane mask `0x00ff00ff`; the low byte and byte
2 of each aligned 32-bit access map to consecutive mailbox bytes. The other
two byte lanes read as open bus and ignore writes. The IRQ write asserts then
clears MN10200 IRQ0, matching the source pulse. The TMS EMPTY line is inverted
onto MN10200 IRQ1.

The source clock domains are MN10200 input 12.5 MHz, internal execution 6.25
MHz, TMS57002 12.5 MHz and ZSG-2 25 MHz. `zoom_board_run` budgets MN internal
cycles and retains instruction overrun from the source CPU core. The
absolute-cycle sync API runs the sound CPU to the requested time (unless held
in reset), then advances ZSG-2 and TMS in the 4:2 clock ratio. ZSG-2 calls
the TMS sample path at its source integer rate `floor(25,000,000 / 768)`, or
32,552 frames/second. Since that integer rate makes the first LRCLK frame
land at 769 ZSG cycles, the TMS timestamp for a sample is the integer floor
of half that ZSG cycle. This retains a stable phase across slices without
claiming scheduler-level equivalence with a complete MAME machine.

`zoom_board_run` and `zoom_board_sync_to_cycle` return `ZOOM_BOARD_DEVICE` when
the board has latched a component fault. `zoom_board_fault` preserves the first
fault until explicitly cleared: MN10200 execution faults, unsupported TMS57002
instructions, TMS callbacks, ZSG-2 diagnostics, bad bus accesses, and host
callback/time errors have distinct values. Clearing the board fault does not
clear a device's own sticky diagnostic; the next synchronization can report it
again. A device fault is never reported as a successful silent run.

The ZSG-2's three external sample-flash windows use MAME's `zsg2_ext_r`
mapping: a ZSG word offset is doubled, selects one of the three flash chips
using address bits 20–21, and reads two adjacent 16-bit words into a logical
little-endian 32-bit packed sample. ZSG outputs 0/1 enter the TMS at gain
0.5; outputs 2/3 enter at gain 1.0. TMS outputs 2/3 are sent to the audio
callback at their configured host gains. Values are normalized float samples;
the callback timestamp is the exact ZSG-2 cycle. The callback runs inside the
scheduler and must not mutate, reset, run or destroy the board. Read-only
inspection is permitted.

The board starts with the sound CPU held in reset, matching the G-Net driver.
On reset release, the Zoom device resets ZSG-2, TMS57002 and the host register
address; shared and sound RAM, TMS control latch, flash contents, and absolute
device clocks persist. If a flash write callback is installed, reset release
also forwards the source `write(0, 0xff)` command to the boot and three sample
flash devices. A manual device reset has only the Zoom device reset effects.

This adapter does not implement G-Net's flash-bank mapper, the Intel flash
command engines, PCMCIA/ATA card path, the main PSX system, or authentic game
firmware. Callers must provide the actual image data and route those other
board devices themselves. The synthetic fixture establishes that the
MN10200 instruction engine executes from the supplied program image, performs
one real ZSG-2 word write, uses the RAM/mailbox decoders and produces a
clocked ZSG-to-TMS audio-frame callback. It is not a game-boot claim.

Run the board integration test and sanitizer build with:

```sh
make -f tests/Makefile.zoom-board test
make -f tests/Makefile.zoom-board sanitize
```

The fixture uses generated in-memory test images only. It checks CPU writes,
ZSG word-access preservation, main mailbox lane decoding and end-of-window
bounds, the 769-cycle sample edge, output callback timestamp, read-only MN
flash mapping, and the four source flash reset commands. The MN10200 callback
tests separately cover word reads/writes, internal-register overlay and
reentry rejection.
