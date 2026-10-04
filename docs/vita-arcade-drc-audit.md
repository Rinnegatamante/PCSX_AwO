# Vita DRC audit for arcade boards

## Current boundary

Vita is an ARMv7 target in `Makefile.vita` (`-mcpu=cortex-a9 -mfpu=neon -marm`). Its `make -f Makefile.libretro platform=vita` build selects the `ari64` new dynarec through the ARM default in `Makefile.libretro`; it links the ARM backend (`new_dynarec.c`, `pcsxmem.c`, `assem_arm.c`, `linkage_arm.S`). NEON is selected separately for the GPU. The System12 adapter now registers a board-aware code source and invalidates the translated bank window on bank changes; the DRC itself handles the selected RAM and ROM geometries. Other board families still require their own fetch, memory-map, and invalidation implementations and are rejected explicitly when the DRC harness is selected.

The board adapter selects its RAM and ROM sizes before `psxInit()`. System12 profiles use 4 MiB RAM, with one profile using 16 MiB, and a 4 MiB fixed main ROM copied into the PSX ROM allocation and mapped at `0x1fc00000..0x1fffffff` (also the `0x9fc...`/`0xbfc...` aliases). The banked ROM is selected in 2 MiB windows at physical `0x1fa00000..0x1fbfffff` and its KSEG aliases. Interpreter execution works because `psxMemInit()`/`system12_mem_reset()` populate `memRLUT` pointers and `psxinterpreter.c` fetches through that LUT. The DRC has a different, fixed model for discovering and compiling instruction bytes.

## DRC assumptions found and addressed

`libpcsxcore/new_dynarec/new_dynarec.c` defines `RAM_SIZE` as `0x200000`. Generated load/store checks use this constant in several sites, including the ARM inline-memory checks around `emit_cmpimm(addr, RAM_SIZE)` and the constant-address checks in `do_readstub`/`do_writestub` generation. This assumes the normal 2 MiB console RAM. A board-selected 4 MiB or 16 MiB allocation does not change the generated checks.

Code-page identity also assumes console mirrors. `pmmask()` clears `0x00e00000` for addresses below 16 MiB, folding bits 21–23 away; `mark_invalid_code()` expands invalidation through 2 MiB mirrors; and `get_page()`/`PAGE_COUNT` retain only 1024 4 KiB pages. On System12, this makes code pages in larger RAM alias to the same DRC page identity. A DMA `Clear()` for a destination above the first 2 MiB can therefore invalidate a different logical code page or miss the intended one. System12 DMA calls `psxCpu->Clear(destination, bytes / 4)` after copying into `psxM`.

There is also a second fast-memory map in `new_dynarec/pcsxmem.c`. It has its own 2 MiB direct RAM mappings and maps only the console's first 8 MiB mirror window. Its BIOS table uses a fixed 512 KiB region, repeated through 4 MiB, instead of the selected `PSX_ROM_SIZE`; that disagrees with System12's 4 MiB ROM mapping. Its per-byte/halfword/word handler tables cover standard `0x1f801xxx` devices, but not System12's board aperture. Consequently generated accesses can bypass the `system12_read()` / `system12_write()` dispatch added in `psxmem.c` unless System12 address ranges are explicitly mapped to DRC dynamic handlers. Simply changing the interpreter/core `memRLUT` is not sufficient.

Instruction source discovery is a separate blocker. `get_source_start()` returns fixed RAM ranges and only `0x9fc00000`/`0xbfc00000` BIOS aliases, with a hard-coded 512 KiB span; it omits the mapped `0x1fc00000` alias and cannot honor the board's 4 MiB ROM size. It has no board callback or case for System12's current banked window (`0x1fa00000..0x1fbfffff` and its KSEG aliases), so `new_recompile_block()` reports “Compile at bogus memory address” for those ranges. The interpreter's successful LUT fetch does not feed this DRC path. A bank write only changes the System12 `memRLUT` entries in `map_bank()`; already translated code for the logical 2 MiB bank window is not invalidated when the backing bank changes.

The ARM assembly backend is not the root cause: it emits code based on the DRC's memory classification and constants. Adjusting `assem_arm.c` checks alone would leave instruction-source discovery, code-page aliases, bank changes, and invalidation wrong. On Vita, new dynarec dispatch uses this ARM backend; there is no supported native ARMv7 DRC route through `assem_arm64.c`.

## Implemented DRC work

The native DRC remains selected; the adapter does not silently fall back to the interpreter:

1. DRC RAM bounds and page identities use the active `PSX_RAM_SIZE`, with console 2 MiB behavior retained. The enlarged page identity map preserves distinct System12 pages and alias expansion follows the selected geometry.
2. Fixed ROM source discovery covers the selected `PSX_ROM_SIZE` through 1fc/9fc/bfc aliases. A provider supplies bank-window bytes through an exclusive 2 MiB span and keeps the guest virtual PC as the block key.
3. System12 bank changes call hard invalidation for 1fa00000..1fc00000. The hard invalidator removes source/copy identities as well as linked translations so an old bank cannot be restored.
4. DRC memory tables route board MMIO to the existing board read/write handlers and dynamically map RAM/ROM geometry. The channel-5 CHCR callbacks are installed for DRC builds, alongside the existing channels.
5. `new_dynarec_arcade_selftest()` and `new_dyna_pcsx_mem_arcade_selftest()` validate RAM aliases, ROM aliases, page identities, and installed handlers. `tests/check_system12_drc.py` executes guest MIPS code in the native DRC against both 4 MiB and 16 MiB profiles. It checks the 1fc ROM alias, distinct bank 0/1 routines at one logical PC, upper-RAM store/load and self-modifying code, the 4 MiB mirror versus the 16 MiB non-alias, and a channel-5 DMA copied routine above 2 MiB.

The DRC-side integration now exposes `new_dynarec_set_code_span_provider()` and `new_dynarec_invalidate_range_hard()`. A non-null source provider enables the board-aware handler pages in `new_dynarec/pcsxmem.c`; the board should register it before `psxInit()` and clear it after `psxShutdown()`. The provider supplies source words and an exclusive virtual-PC limit for the selected banked window. Fixed ROM and RAM source spans use the selected `PSX_ROM_SIZE` and `PSX_RAM_SIZE`. A bank-selection write should call the hard invalidator for the logical `0x1fa00000..0x1fc00000` window (aliases fold to that identity), so old translated blocks cannot be restored from an unchanged old-bank source pointer. `new_dynarec_arcade_selftest()` checks page identity and RAM alias arithmetic, while `new_dyna_pcsx_mem_arcade_selftest()` checks installed RAM/ROM/board-handler table mappings. Until all stages pass in guest execution, the Vita DRC must report this board configuration unsupported at build or board-load time. Selecting the interpreter automatically would hide an unsupported DRC path and would not meet the native-DCR requirement.

## Verification boundary

The synthetic ARM64 DRC harness passes `python3 tests/check_system12_drc.py`. Both the legacy COH-700 4 MiB profile and COH-716 16 MiB/alt-bank profile execute through the DRC and report a real 12-byte channel-5 transfer; trace assertions verify both bank signatures, upper-RAM SMC signatures, alias behavior, and a translated guest shared-RAM MMIO roundtrip. The 4 MiB SMC probe writes through the `80610000` mirror after executing the same backing code at `80210000`. The same guest also passes on the interpreter harness. ARMv7 DRC source files were cross-compiled for Vita during implementation, but this is not a Vita device/runtime test. No complete System12 game boot on Vita is claimed by this test.

The actual Soul Calibur profile does **not** pass the DRC game check. The M1
host page-protection fix allows the early ROM boot to execute, but a 3600-frame
run reaches reserved-instruction exceptions with the RAM vector at `80`
overwritten by `ffffffff`. Debugging shows a guest byte-copy loop writing
through the valid 4 MiB RAM mirror `80400080`; the interpreter does not make
that write in its successful game run. The first CPU or board-state divergence
is still under investigation. A frame count and zero H8 unsupported opcodes
cannot turn this failure into a game-validation PASS.

If a DRC build is selected for another board family, its loader must reject that board explicitly before CPU initialization until that family's memory, fetch, and invalidation contract is implemented. It must not silently change to the interpreter.
