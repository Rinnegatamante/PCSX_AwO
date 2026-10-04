# Konami System GQ source profile

`scripts/konamigq_boards.json` covers the complete GQ family at pinned MAME
revision `deb2f54fe7300e7a192690318beb860159bfd52a`: one set, `cryptklr`,
Crypt Killer (GQ420 UAA). Regenerate or verify against the local checkout with:

```sh
python3 scripts/konamigq_boards.py
python3 scripts/konamigq_boards.py --check
python3 -m unittest discover -s tests -p test_konamigq_boards.py
```

The registry checks the source GAME declaration against the pinned listxml
family and extracts all three 512 KiB regions: `maincpu:rom`, `soundcpu`, and
`k054539`. It retains each file's CRC/SHA1 and load geometry, including the
68000 sound program's `ROM_LOAD16_WORD_SWAP` transform. The hard disk is
`420uaa04`, SHA1 `67cb1418fc0de2a89fc61847dc9efb9f1bebb347`. MAME declares
it writable, connected at SCSI ID 0; the NCR53CF96 initiator is ID 7. This
registry does not validate an installed disk, mount media, or execute a game.
The [local media preparer](konamigq-media.md) separately verifies ROM hashes,
HDD CHD geometry, source SHA1 and hunk integrity before packaging it.

The source config uses CXD8530BQ at 67.7376 MHz, 4 MiB RAM, a type-1 CXD8538Q
with 2 MiB VRAM, an 8 MHz 68000, a 24 MHz TMS57002, MB89371 at 4 MHz,
NCR53CF96 at 16 MHz, K056800 at 18.432 MHz and two K054539 chips at
18.432 MHz. Each PCM chip sees the same 512 KiB sample ROM and shared
3.5 MiB PCM RAM at `0x080000..0x3fffff`.

GQ has no EEPROM ROM region. The driver supplies 64 default 16-bit words
through `default_data`, retained in the registry with their 128-byte
little-endian storage SHA256. These are source defaults for an initially
unpersisted device. A later adapter must preserve an existing user's EEPROM.
The default table carries the driver's BSD-3-Clause notice in
`scripts/konamigq_LICENSE.txt`.

The native GQ adapter, mounted writable HDD target, 68000 sound execution,
TMS57002 effects and three-lightgun I/O remain pending. Standalone device
tests and this source profile do not establish a Crypt Killer boot.
