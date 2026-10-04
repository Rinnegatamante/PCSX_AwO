# Native CHD hard-disk backing

The Mac harness now enables its existing console CHD reader by default using
the repository's pinned `deps/libchdr` revision
`9ccd3a78e239246ee6e1166e1be82ab0ff0dcddb`. Initialize the submodule with
`git submodule update --init deps/libchdr`; `HAVE_CHD=0` keeps the build optional.
This does not add a disc boot CLI or prove a Konami board boot.

`libpcsxcore/arcade/chd_hd.{h,cpp}` provides sector backing for the independent
SCSI disk target. It opens a local CHD read-only, validates one GDDD geometry
against the header, and exposes optional IDNT inquiry data. It accepts CHD
versions 3–5, a 32-bit sector count, sector sizes through 64 KiB and hunks
through 64 MiB. Missing parents, malformed metadata, duplicate geometry and
inconsistent sizes fail. Reads replace the caller's buffer only on success.
The wrapper caches one decompressed hunk and handles sector/hunk boundaries.
The public read/write result is **zero on success**; SCSI callbacks require an
adapter that returns `result == 0`.

An optional overlay path enables writes. Modified sectors replace base sectors
in memory; the original CHD remains read-only. `flush` explicitly saves the
sorted sector records to a temporary file, flushes/fsyncs it and atomically
renames it. Close discards writes that were not flushed. A failed save preserves
an existing overlay. Overlay headers bind the CHD SHA1 and complete geometry;
CRC32 covers each record's LBA and bytes. Wrong-image, truncated, duplicate,
unordered, out-of-range and corrupt records fail at open. Writable images need
a nonzero CHD SHA1. The current overlay cache is limited to 256 MiB and reports
failure when full. This is host persistence, not MAME's differential CHD format
or a save-state format. The API has one owner and is not concurrent.

The pinned bitstream reader promotes an input byte to signed `int` before
shifting it 24 bits. UBSan detected that overflow while reading a generated
compressed CHD. The host build applies an unsigned cast and a 32-bit removal
shift fix to a build-local copy, retaining upstream notices. The generation
script checks each original expression exactly once and fails if it changes;
the submodule checkout stays unmodified. The test includes high-bit 32-bit
reads, byte reads and bit-offset seek under UBSan.

```sh
make -f tests/Makefile.chd-hd -j8 test
make -f tests/Makefile.chd-hd -j8 sanitize
```

These tests require `chdman`. They create and compress generated data with real
CHD codecs, verify every sector including a final partial hunk, alternate cache
entries, read-only behavior, overwrite/reopen persistence, discarded writes,
wrong-image/corrupt overlays and save failures. A destination changed into a
symlink or hard link to the source CHD is rejected. A SHA256 comparison confirms
all input CHDs are unchanged after the test. ASan and UBSan pass with
`UBSAN_OPTIONS=halt_on_error=1`. No game disk or firmware is supplied by the test.

The [NCR/SCSI/CHD integration test](chd-scsi-hd-test.md) now passes READ/WRITE
DMA, status, COMMAND COMPLETE, message acceptance and disconnect, followed by
overlay flush/reopen and another DMA read. This uses the real controller,
bus, target and compressed-sector backing. Native GQ board integration and
authentic game execution remain pending.
