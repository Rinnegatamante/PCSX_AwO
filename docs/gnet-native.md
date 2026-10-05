# Native G-Net board adapter

The native adapter composes the pinned MAME G-Net map with the existing Sony
ZN common board implementation. Source is MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`, primarily
`src/mame/sony/taitogn.cpp` and `src/mame/sony/zn.cpp`.

The strict `board.ini` loader currently accepts the two MAME BIOS-root machine
sets, `coh1002t` and `coh3002t`. Each package supplies these assembled regions:

| Region file | MAME region | Size | Source check |
|---|---|---:|---|
| `maincpu.bin` | `maincpu:rom` | 512 KiB | Exact root-set SHA1 |
| `cat702_0.bin` | `cat702_1` | 8 bytes | Exact root-set SHA1 |
| `cat702_1.bin` | `cat702_2` | 8 bytes | Exact root-set SHA1 |
| `firm.bin` | `firm` | 2 MiB | Exact root-set SHA1 |
| `eprom.bin` | `eprom` | 1 MiB | Exact SHA1; COH1002T source region is erased `0xff`, COH3002T uses BIOS v1 |

The sound program and three sample-flash devices are absent from the BIOS-root
ROM declarations. MAME creates those flash devices in erased state, so the
adapter initializes them to `0xff`; it does not substitute media. The source's
`upd78081` region is a NO_DUMP device that this G-Net machine configuration
does not instantiate. This does not imply that all games or PCMCIA media work.

The `board.ini` file has exactly these keys, in any order:

```ini
format=pcsx-aow-gnet-v1
mame_set=coh3002t
mame_source_commit=deb2f54fe7300e7a192690318beb860159bfd52a
board_model=ZN2
cpu_device=CXD8661R
cpu_clock_hz=100000000
gpu_device=CXD8654Q
gpu_clock_hz=53693175
main_ram_size=4194304
vram_size=2097152
game_execution_verified=false
```

The adapter configures the source 4 MiB PSX RAM and 2 MiB VRAM, Sony ZN CAT702,
EEPROM and SIO devices, then routes CPU accesses through `gnet_zoom_bus_*` and
the shared ZN map. PSX cycles advance the 6.25 MHz MN10200 sound board on a
fractional, monotonic clock. `1fb00000–1fb0ffff` remains an explicit
unsupported RF5C296/PCMCIA range; game cards, per-game input variations, and
game-specific extensions are not claimed as implemented.

The general PSX core still advances in its existing PSX cycle domain; this
adapter does not yet reproduce the source 67.7376/100 MHz main-CPU execution
rate or source vblank timing. The source clocks recorded in `board.ini` are
profile checks, not a claim that those main-CPU timings are already applied.

`--system-gnet-fixture` is test-only and uses generated in-memory data. Its
executed MIPS program selects flash bank 1, reads the banked flash and constant
ID register through `psxMemRead/Write`, and records both values in PSX RAM. It
validates the CPU-to-core-map wiring, not MAME assets or a game boot.
