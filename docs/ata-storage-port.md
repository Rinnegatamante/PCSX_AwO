# ATA HLE mass-storage port

The engine in `libpcsxcore/arcade/ata_mame/` is generated from MAME revision
`deb2f54fe7300e7a192690318beb860159bfd52a`. The importer reads that exact Git
object, verifies SHA-256 for all four input files, and rejects missing or
changed transformation anchors:

```sh
python3 tools/port_mame_ata.py --check
python3 tools/port_mame_ata.py
```

The checked-in port keeps the source class boundaries and command bodies from
`ata_hle_device_base`, `ata_mass_storage_device_base`, and `cf_device_base`.
The importer changes only the MAME include paths, constructors, log include,
`ide_hdd_device_base` image wrapper, and CF machine-configuration wrapper. It
does not replace ATA command dispatch with a second implementation. The
original MAME copyright holder and BSD-3-Clause license are retained in the
generated source.

`ata_mame_compat.h` supplies the small part of MAME's framework used by these
classes: `attotime`, callback timers, line constants, byte-order helpers, and
no-op save/log hooks. The caller advances the deterministic scheduler with
`ata_storage_run_until`. The busy and buffer-empty timers keep separate
deadlines and process equal deadlines in timer-allocation order. Deadline
addition clamps at `INT64_MAX`; it never wraps.

The C ABI in `ata_storage.h` wraps the original CompactFlash mass-storage
class. Sector callbacks receive exactly 512 bytes and follow MAME's return
convention, where nonzero is success. A failed read or write therefore flows
through MAME's original `BAD_SECTOR` error path. Callback contexts, line
contexts, passwords, and the attached G-Net card are borrowed; callers keep
them alive until ATA destruction. `ata_storage_try_destroy` reports `BUSY`
while an operation or external callback is active.
Callbacks may inspect the ATA object and try destruction, but must not issue
ATA register writes or reset the same object while a command is in progress;
nested `run_until` is rejected with `BUSY`.

ATA IDENTIFY input may be omitted to use MAME's CompactFlash identify builder,
configured with the provided serial, firmware, and model strings, or supplied
as the raw 512-byte `IDNT` metadata payload. Raw bytes are decoded as ATA
little-endian words, matching MAME's image inquiry path. The CompactFlash
IDENTIFY builder fills words 7-8, 57-58, and 60-61 from the source's
`cylinders * heads * sectors_per_track` calculation, and READ NATIVE MAX uses
the same geometry product; a supplied raw `IDNT` payload is preserved as-is.
Explicit CHD geometry is therefore not
silently replaced by the medium's sector count. When cylinders are omitted,
the adapter infers them from sector count and the configured heads/sectors,
capped at 65535 as in the wrapper's geometry representation. Since the pinned
source calculates total sectors in signed `int`, the effective geometry product
must be at most `INT_MAX` (2,147,483,647 sectors); larger configurations are
rejected before entering the source IDENTIFY or command bodies. The G-Net
bridge invokes the card hooks
at the original `process_command`, `process_buffer`, and `is_ready` boundaries;
its taskfile snapshot carries the expanded sector count (`0` from the ATA
register is represented internally as `256`).

The source `ide_hdd_device_base` class boundary, status timer, and
`calculate_status()` body are retained. Its image-backed geometry, reset
metadata, and sector access are replaced by the callback-backed adapter.
That HDD class reports rotational IDX after the source's 90 Hz rotation
interval when status is read. The pinned CompactFlash class explicitly
overrides `calculate_status()` to call the ATA base implementation, so CF
continues to suppress IDX; the separate HDD and CF tests preserve both source
behaviors. MAME save-state registration is not modeled in this standalone
port. DMA timing is supplied by `dma_transfer_time_ns`, while CF seek timing
and the source's write/sector delays remain in the copied command bodies.

The isolated suite covers power-on and software reset timing, pending IRQ
gating, raw IDENTIFY and 8-bit transfer mode, PIO and DMA multi-sector reads
and writes, failed media callbacks, and the Taito Type-2 unlock through the
real ATA engine:

```sh
make -f tests/Makefile.ata-storage test
make -f tests/Makefile.ata-storage sanitize
```
