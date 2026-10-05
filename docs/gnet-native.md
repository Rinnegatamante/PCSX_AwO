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

The 6.25 MHz adapter clock is the MN10200's effective execution-cycle rate:
the pinned MAME device is configured at 12.5 MHz and converts external clocks
to CPU cycles through its internal divide-by-two.

Build the standalone Vita core archive with
`make -f Makefile.libretro platform=vita ARCADE_SYSTEM12=1 ARCADE_SYSTEM_GNET=1 USE_LIBRETRO_VFS=0 USE_ASYNC_CDROM=0 USE_ASYNC_GPU=0 USE_ASYNC_SPU=0`.
With that opt-in build, the libretro frontend accepts a prepared profile
directory as game content when it contains `board.ini`. It configures the
board's memory and VRAM sizes before initializing the core, keeps the selected
CPU backend, routes the adapter's listed digital controls, resets and unloads
the board through its adapter lifecycle, and disables savestates while the
board is active because the current generic state format omits G-Net devices.
Normal console game loading remains available in the same build.

The public G-Net adapter API can now deliver Zoom PCM frames to an optional
callback. The sink is disabled by default and persists across board resets;
the libretro frontend still routes generic PSX SPU audio only and does not yet
mix or resample Zoom PCM. The EEPROM API is not connected to frontend save
storage. G-Net card/PCMCIA media, game-specific input variations, and other
game-specific device extensions remain unsupported.

The audio callback receives normalized stereo floats and a ZSG timestamp in
the 25 MHz chip-clock domain. It runs synchronously while `systemgnet_sync`
advances the board, including when CPU MMIO routing triggers synchronization,
so clients should keep it bounded and avoid reentering or destroying the
board. Timestamps increase within a sound-device reset epoch and may restart
when those devices reset. The client owns the opaque pointer and must clear the
callback before freeing it; passing a null callback disables delivery.

The standalone Zoom regression also loads a synthetic MN10200 program that
uploads a small TMS57002 tone routine through the board pins and captures the
real `gnet_zoom` audio callback. It checks finite nonzero PCM, increasing ZSG
timestamps, and identical output for one-shot versus split cycle synchronization.
This validates the emulator's PCM path with test data; it does not supply the
missing authenticated game sound program or sample media.

The general PSX core still advances in its existing PSX cycle domain; this
adapter does not yet reproduce the source 67.7376/100 MHz main-CPU execution
rate or source vblank timing. The source clocks recorded in `board.ini` are
profile checks, not a claim that those main-CPU timings are already applied.

`--system-gnet-fixture` is test-only and uses generated in-memory data. Its
executed MIPS program selects flash bank 1, reads the banked flash and constant
ID register through the PSX memory bus, and records both values in PSX RAM. It
also installs an audio sink before board load and reset, then advances the
synthetic MN10200/TMS57002 tone program and verifies finite nonzero PCM with
monotonic timestamps through the public adapter callback. The complete fixture
passes with both the interpreter and ARM64 DRC, including the PCM assertions
and sink-disable check. This checks the board map bridge, audio callback
lifecycle, and 4 MiB RAM path, not MAME assets, startup firmware, game boot, or
real-board compatibility.
