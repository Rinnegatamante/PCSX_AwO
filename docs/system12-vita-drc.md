# System12 ARMv7 DRC probe on Vita3K

The opt-in Vita NEON package has a separate synthetic DRC check. It exercises
the translated MIPS path for both 4 MiB and 16 MiB System12 RAM profiles. The
generated guest changes a ROM bank and calls both bank images, writes and
executes self-modifying code in upper RAM, checks the profile-specific RAM
alias, triggers channel 5 DMA, and reads back shared board MMIO. At the end the
guest writes its computed register results into a fixed RAM signature block.
The shared-board checks cover byte, halfword, and word reads after writes to
other offsets, validating the DRC's effective-address callback for each width.

Build the `VITA_GPU=neon ARCADE_SYSTEM12=1` VPK, then run:

```sh
python3 tools/vita-harness/check_system12_drc_vita3k.py \
  --emulator /path/to/Vita3K \
  --vpk build/vita-system12/src/pcsx-rearmed-vita-neon.vpk \
  --firmware-fs /path/to/Vita3K/fs \
  --log /tmp/system12-vita-drc.log
```

The runner creates one temporary VPK containing generated board regions and
both profiles. It copies the Vita3K app bundle into a temporary directory and
creates a sibling portable profile there, so the install allowlist and runtime
filesystem do not use the caller's Vita3K profile. It copies only the local
firmware `vs0`, `os0`, and `sa0` trees; it does not copy `ux0`, installed games,
saves, or shader caches. The temporary app, filesystem, and package are removed
when it exits. It reuses local Vita3K firmware, but no game ROM or PlayStation
BIOS is used. The standalone
`--verify-system12-drc 4|16` mode requires the
native `ari64` DRC and checks the MIPS-written RAM signature through the normal
memory accessor; it does not inspect host CPU registers.

This validates synthetic CPU, memory-map, bank, SMC, DMA, and shared-MMIO paths.
It does not validate a retail game, gameplay, or the VitaGL GPU backend.

Both profiles passed on the isolated M1 host built from local Vita3K
`149-5f58258` with the checked-in host teardown patch. The log
`/tmp/pcsx-vita-system12-drc-fixed-host.log` records all expected guest results,
88.2 kHz input converted to a 48 kHz output device, and `Stopping -> Idle` for
each profile. The runner closed its owned processes without a host exception.
The six runner/fixture unit checks and the M1 signature fixture also pass.
