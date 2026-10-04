# Shared PSX arcade NOR flash engine

`libpcsxcore/arcade/nor_flash.c` provides a standalone command engine for the
flash profiles currently identified in MAME's PlayStation arcade drivers. It
does not make a board bootable by itself; each board still needs its own
mapping, loader, persistence policy, and CPU integration.

The constructor takes a caller-owned, writable memory image and requires its
length to exactly match the selected part. The engine borrows the image without
initializing or copying it, so existing ROM/NVRAM contents are retained.
Destroying the device does not free the image. API bus addresses are byte offsets
for x8 chips and 16-bit word offsets for x16 chips. Intel word reads and writes
use the byte ordering in MAME's 16-bit `intelfsh` wrapper.

Implemented profiles and MAME geometry are:

| Profile | Image bytes | Bus | ID bytes | Erase geometry |
| --- | ---: | ---: | --- | --- |
| Intel 28F640J5 | 8 MiB | x16 | `89/15` | 64 KiB blocks |
| Fujitsu MBM29F016A | 2 MiB | x8 | `04/ad` | 64 KiB sectors |
| AMD 29F040 | 512 KiB | x8 | `01/a4` | 64 KiB sectors |
| SST 39SF040 | 512 KiB | x8 | `bf/b7` | 4 KiB sectors |

MAME's System 10 and System 12 board notes identify the 64-Mbit Intel device as
DA28F640J5. The device registry exposes `INTEL_28F640J5`; there is no
28F640K3 profile in the inspected upstream `intelfsh` implementation, so this
engine does not silently alias K3 to J5.

Intel supports array/ID/status modes, single-word or byte program setup, block
erase confirmation, status clear, and master-lock status. Like MAME, block-lock
configuration reads as zero and are not implemented; master lock is exposed in
ID mode but does not impose extra write protection in MAME's model. AMD/Fujitsu
profiles support the AMD unlock, autoselect, byte-program, sector-erase, and
chip-erase sequences. SST uses MAME's 39SF040 unlock aliasing and 4 KiB sector
geometry. Programming can only clear bits (`1` to `0`); erases set bits to `1`.

Erase array contents change at the command's confirm write, as in MAME, while
ready/status behavior remains timed. `nor_flash_advance_us()` advances a virtual
clock: Intel block erase takes 1 second; Fujitsu and AMD sector erase takes 1
second and chip erase takes 16 seconds; SST sector erase takes 125 ms and chip
erase takes 1 second. AMD-style status polling toggles DQ6/DQ2 while erasing.
The MAME implementation does not schedule a separate busy interval for byte
program operations, so this port keeps those writes immediate instead of
inventing a program delay.

Commands outside these implemented profiles and sequences return an explicit
`NOR_FLASH_ERR_UNSUPPORTED` or `NOR_FLASH_ERR_SEQUENCE`; out-of-range bus
accesses return `NOR_FLASH_ERR_RANGE`. Intel 28F640J5 write-buffer programming,
per-block locks, x16 AMD variants, boot-sector variants, SST page programming,
and other Intel flash families are not implemented.

Run the synthetic protocol tests with:

```sh
make -C tests -f Makefile.nor_flash clean run
```

The traces cover geometry validation, borrowed data, autoselect IDs, Intel
status/master-lock commands, x16 program ordering, AMD unlock/program/erase,
bit-clearing behavior, busy status toggles and timing, invalid sequences, and
bounds checks.
