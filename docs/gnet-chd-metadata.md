# G-Net metadata from local hard-disk CHDs

`arcade_chd_hd_read_metadata` exposes the raw metadata used by MAME's hard-disk
image reader: `CIS ` for the G-Net PC Card CIS, `KEY ` for its five-byte Taito
unlock key, and `IDNT` for an ATA Identify block. MAME's `harddisk.cpp`
`get_cis_data`, `get_disk_key_data`, and `get_inquiry_data` read those tags at
index 0; the tag definitions are in `lib/util/chd.h`. The ATA storage adapter
can pass CIS/key bytes directly to the PC Card card config and accepts IDNT as
ATA Identify only when it is exactly 512 bytes.

The API reports `OK`, `MISSING`, `BUFFER_TOO_SMALL`, `ERROR`, or `ARGUMENT`,
with the exact byte length separate from the status. `OK` with length zero is
a valid empty metadata record and is distinct from `MISSING`. On
`BUFFER_TOO_SMALL`, the byte buffer is untouched and the required capacity is
reported. On missing, read error, or invalid arguments, both the byte buffer
and caller's length value remain unchanged. It never silently truncates. CHD
metadata is read into a temporary buffer and copied only after a complete
successful read.

The existing `arcade_chd_hd_info_t.inquiry[48]` remains the SCSI inquiry view.
It still copies at most 48 bytes from IDNT for the current NSCSI path. The new
getter independently reads the whole IDNT value for ATA consumers; it does
not change SCSI behavior.

`tests/check_chd_hd.py` builds local CHDs with `chdman` and adds CIS, KEY, and
IDNT data. Its checks cover missing tags, custom data, empty and wrong-length
records, required-capacity reporting, preserved output on read failure, and
legacy 48-byte inquiry behavior. These generated fixtures test metadata
transport only; no game image or authentic G-Net boot is involved.

```sh
make -f tests/Makefile.chd-hd test
make -f tests/Makefile.chd-hd sanitize
```
