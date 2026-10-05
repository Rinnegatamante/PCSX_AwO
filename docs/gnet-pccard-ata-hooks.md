# G-Net PC Card behavior and ATA integration boundary

`libpcsxcore/arcade/gnet_pccard.{h,cpp}` ports the card-facing behavior from
MAME `src/devices/bus/pccard/ataflash.cpp`, pinned at
`deb2f54fe7300e7a192690318beb860159bfd52a`. It implements the default/custom
CIS aperture, configuration option/status and pin replacement registers, the
Taito Type 1 attribute-key window, and Type 1/Type 2/Taito CF lock protocols.
The default CIS bytes and the 512-byte `0xff` extension match MAME's reset
path. `gnet_pccard_identify_word0` exposes MAME's `0x848a` Identify Device
word override for the future ATA adapter.

The MAME image source is also explicit: `ataflash.cpp::device_reset` calls
`harddisk_image_device::get_cis_data` and each Taito variant calls
`get_disk_key_data`; `lib/util/harddisk.cpp` resolves these to CHD metadata
tags `CIS ` and `KEY ` (`lib/util/chd.h`). The caller supplies already-decoded
bytes and a five-byte key to this adapter; this port does not parse CHD or
choose/load disk images.

The ATA device is composed behind borrowed `gnet_pccard_ata_ops_t` callbacks.
The future ATA HLE must route PC Card memory offsets 0..7 through its
`command_r/w` path, 8..15 through `control_r/w`, and call
`gnet_pccard_process_command` from its `process_command` override before
normal ATA command dispatch. MAME's `command_w` invokes `process_command`
synchronously, so this hook is the one sanctioned callback reentry. It applies
a HANDLED delta to its live status,
error, IRQ, sector count and transfer length; PASS continues normal HLE
dispatch. Its `process_buffer` override calls
`gnet_pccard_process_buffer` before normal media handling; its readiness query
uses `gnet_pccard_is_ready`. The taskfile snapshot must reflect the ATA HLE
registers latched before command execution, including MAME's internal
`sector_count` expansion where ATA register value zero is represented as 256.
Memory taskfile reads and writes
pass `eight_bit_data_transfers = !(mem_mask & 0xff00)`, matching MAME's
`ACCESSING_BITS_8_15` handling. The wrapper does not own taskfile/status state,
buffer timing, IRQ delivery, transfer machinery, Identify construction beyond
the one explicit word override, or sector/media I/O.

When no backend is supplied, attribute/configuration accesses and unlock state
remain usable, but ATA memory reads return `0xffff`, writes are ignored, and
`gnet_pccard_reset` reports `GNET_PCCARD_NO_ATA_BACKEND`. That result is not a
successful ATA command. The callback descriptor is copied by the PC Card
wrapper; its opaque state remains borrowed and must outlive it. Apart from the
required command/buffer/readiness hooks, backend callbacks must not reenter the
card or destroy it; a `gnet_pccard_try_destroy` attempted during a callback
returns `BUSY`. Serialize all card/backend calls; the card adds no thread
synchronization.

The unit tests use a small synthetic backend fixture solely to test this
composition contract and the MAME card state transitions. They do not include
ATA HLE or a disk image and provide no authentic G-Net boot claim. A full boot
still requires a source-faithful ATA HLE/CF mass-storage integration and
valid PC Card CIS/key plus sector media metadata.

Run the isolated checks with:

```sh
make -f tests/Makefile.gnet-pccard test
make -f tests/Makefile.gnet-pccard sanitize
```

The Mac harness and optional `ARCADE_SYSTEM_GNET=1` libretro build also compile
the card component. The Mac DRC G-Net regression passes the synthetic MIPS,
81-frame PCM, bank and upper-VRAM checks after this build integration
(`/tmp/pcsx-gnet-pccard-metadata-drc-20261005.log`). A VitaSDK cross-compile
of the card translation unit passes with the platform's Cortex-A9/NEON flags;
command and object/source hashes are recorded in
`/tmp/pcsx-gnet-pccard-vita-object-20261005/evidence.json`. These checks do not
attach a card to the native loader or validate a G-Net game on Vita.
