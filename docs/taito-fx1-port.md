# Taito FX-1S board adapter

`libpcsxcore/arcade/taito_fx1.c` implements the FX-1S daughterboard portions of MAME `src/mame/sony/zn.cpp` at revision `deb2f54fe7300e7a192690318beb860159bfd52a`, plus the TC0140SYT protocol from `src/mame/shared/taitosnd.cpp`. It is a board component for the FX-1S / COH1000T configuration. The Taito FX-1 ZROM and G-Darius variants use different processors, audio devices, and maps and are outside this adapter.

The adapter owns a real existing-core Z80, YM2610B wrapper, TC0140SYT protocol device, and MB3773 timer. It borrows the MAME `bankedroms`, `audiocpu`, and `ym2610b:adpcma` buffers, and calls the host to schedule deferred sound synchronization and machine soft reset. The supplied ROM sizes must match FX1S game declarations: 16 MiB `bankedroms`, 128 KiB `audiocpu`, and 2 or 4 MiB ADPCM-A sample region. The 512 KiB BIOS ROM, common ZN RAM, GPU, SPU, CAT702 devices, and main PSX memory dispatch remain the responsibility of their owning components.

## Source map

The FX base config sets four 8 MiB banks over the MAME `bankedroms` region at `0x1f000000..0x1f7fffff`; the selector at `0x1fb40000` uses bits 0–1. Source `ROM_START` entries declare this region as 16 MiB, so banks 0 and 1 have backing data while selected banks 2 and 3 are preserved as selections but fault on read. The adapter does not pad the region or invent upper-bank content. The selector's bit 5 drives the MB3773 clock input. A falling edge rearms the watchdog.

The main CPU TC0140SYT ports are byte-wide at `0x1fb80000` (master mode) and `0x1fb80002` (master communication). Naturally aligned byte, halfword, and word accesses preserve little-endian byte lanes and apply each mapped register once. KSEG0/KSEG1 aliases are accepted. Other addresses are left to the Sony ZN common board map.

The 4 MHz Z80 maps fixed audio ROM at `0x0000..0x3fff`, an 8-bank 16 KiB view at `0x4000..0x7fff`, 8 KiB RAM at `0xc000..0xdfff`, and YM2610B registers at `0xe000..0xe003`. Reset selects sound bank 1; `0xf200` writes select any of eight entries, with each entry based at the start of the source audio ROM region as MAME declares. TC0140SYT slave port/communication are at `0xe200`/`0xe201`. Its NMI output drives the Z80 NMI pin; the master reset output holds the CPU stopped while asserted and resets it on release, matching MAME's CPU line callback. YM timers continue while that external CPU reset is held. The source no-op ranges are preserved: `0xe200` and `0xee00` reads return zero; writes to `0xe400..0xe403`, `0xee00`, and `0xf000` do nothing. Other unimplemented sound accesses set an explicit fault.

The YM2610B wrapper runs at 8 MHz, twice the Z80 clock. Its level IRQ output drives Z80 IRQ0. The mix follows MAME routing: SSG contributes 0.75 to both stereo channels; FM left and right contribute at unity. Audio samples are delivered by an optional callback at the wrapper's integer 8 MHz / 144 rate. The scheduler runs in one-Z80-T-state slices, giving two YM clock cycles of host time resolution; this costs more CPU time than coarse stepping. The board tracks a cycle epoch across Z80 reset release so YM bus accesses remain on the continuously advancing YM timeline while the CPU was held.

`taito_fx1_advance_to_us` takes a monotonic absolute machine time for the MB3773. Watchdog expiry queues the caller's soft-reset scheduler callback after the current adapter operation; that callback must request a later machine reset instead of resetting the board inline. TC0140SYT master accesses request an external scheduler barrier; the host may acknowledge completion through `taito_fx1_sound_synchronization_completed` after servicing it.

## Validation boundary

`tests/test_taito_fx1.cpp` executes generated Z80 programs through the real core to change all eight sound banks, read bank markers, configure a YM2610B SSG tone and timer IRQ, and receive/acknowledge a two-nibble TC0140SYT command through NMI. It also checks reset hold/release, YM clocks continuing during reset, MB3773 edge kicks and deferred expiry, main ROM banking and out-of-bounds bank selection, no-op maps, and identical audio output for whole versus split cycle runs.

Build and run the component tests with:

```sh
make -f tests/Makefile.taito-fx1 test
make -f tests/Makefile.taito-fx1 BUILD=build/taito-fx1-sanitizers \
  CFLAGS='-std=c99 -O0 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
  CXXFLAGS='-std=c++17 -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -fsanitize=address,undefined -fno-omit-frame-pointer' test
```

These tests verify this adapter's source-derived maps and chip interactions with synthetic data. They do not prove a retail game boots or that the complete ZN/FX-1 system is integrated.
