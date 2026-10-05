# G-Net CHD, ATA and PC Card composition

`gnet_storage` composes the local CHD hard-disk adapter, source-derived ATA HLE,
the source-derived G-Net PC Card variants, and an RF5C296 callback descriptor.
It is a media/controller component for later board integration; it does not
load a game or claim an authentic G-Net boot.

`gnet_storage_open` opens the CHD read-only, optionally checks the caller's
20-byte CHD SHA-1, validates 512-byte sectors and MAME's bounded CHS geometry,
and then reads `CIS `, `KEY `, and `IDNT` through the exact-length metadata API.
The ATA sector callbacks translate the CHD adapter's zero-success convention
to the source ATA HLE's nonzero-success convention. ATA writes go only to the
optional overlay and reach disk only after explicit `gnet_storage_flush`.

Metadata behavior follows MAME's `harddisk_file` and `ata_flash_pccard_device`
paths in the pinned source `deb2f54fe7300e7a192690318beb860159bfd52a`:

- Missing CIS uses MAME's synthesized default CIS. Present empty CIS remains
  present and is not replaced by the default.
- Missing or non-five-byte KEY does not put a Taito Type 1, Type 2, or CF card
  into its locked state. An exact five-byte key drives the card's real unlock
  protocol.
- A 512-byte IDNT is passed raw into the source ATA engine, preserving its
  little-endian words, including word 0. A present IDNT with another valid
  length is reported in `gnet_storage_info_t` but does not enter the raw-data
  path; the source CF IDENTIFY builder supplies the ATA data instead. Missing
  IDNT uses that same builder.
- A metadata I/O error fails open explicitly. It is never treated as missing.

The ATA/CHD geometry must have 512-byte sectors, nonzero CHS, cylinders no
greater than 65535, heads and sectors per track no greater than 255, and a CHS
product no larger than `INT32_MAX`. It must match the CHD's sector count, as
required by the hard-disk geometry record.

`gnet_storage_card_ops` returns a borrowed callback descriptor copied by the
RF5C296/G-Net board. That board must be destroyed or detached before the
storage object. Calls are synchronous, single-owner, and non-reentrant. The
component reports BUSY from destruction while one of its callback/API
operations is active; retry after it returns. Teardown detaches the card from
ATA, destroys card, ATA, and CHD, in that order. Overlay persistence is
explicit; destroying an unflushed storage object discards pending writes.

The integration suite generates synthetic local CHDs with `chdman`; it does
not use copyrighted game media. It exercises Type 1/Type 2/CF/generic cards,
unlock through RF5C296/card operations, raw and fallback IDENTIFY, PIO sector
read/write, overlay flush/reopen, CHD hash checking, missing/empty/wrong-size
metadata and a real metadata read error. Its fixtures are transport and
controller checks only, not game boot evidence.

```sh
make -f tests/Makefile.gnet-storage test
make -f tests/Makefile.gnet-storage sanitize
```
