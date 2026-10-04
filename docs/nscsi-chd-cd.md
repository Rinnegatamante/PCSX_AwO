# CHD CD media backend

`chd_cd.*` is a read-only CD media backend built on the repository-pinned
libchdr. It supplies every `nscsi_cd_media_ops_t` callback from a local CHD:
track count/TOC, track classification, MODE1 sector reads, and CDDA playback
status and PCM. It does not create a ready medium when opening fails.

The source-derived layout follows MAME `cdrom.cpp` at
`deb2f54fe7300e7a192690318beb860159bfd52a`: CHTR/CHT2 track metadata, 2448-byte
frames, per-track 4-frame CHD padding, logical/physical/CHD frame offsets,
generated pregaps, postgap offsets, and the 16-byte raw MODE1 header skip.
CDDA follows MAME `cdda.cpp`: a 44.1 kHz stereo stream, 588 sample frames per
sector, big-endian signed PCM16 samples, source-sector progression, pause,
natural end, and independent output gains. `chd_cd_advance_to` is called with
monotonic absolute nanoseconds and carries fractional sample time across calls.

The first implementation deliberately accepts only CHTR/CHT2 metadata with
`MODE1`/`MODE1_RAW` and `AUDIO` tracks and no subchannel payload. It rejects
GD-ROM metadata, old binary TOC metadata, MODE2/CDI, subcode tracks, unknown
pregap encodings, malformed track lists, parent-dependent CHDs that libchdr
cannot open, and layouts whose padded frame count differs from the CHD image.
It does not perform CD error correction, synthesize raw MODE1 headers, expose
subchannel bytes, implement CDDA scanning, or provide CHD writing. All read
failures are returned to the SCSI target; no filler sector is presented as
media data.

The fixture test creates a private three-track CHD with MAME `chdman` from local
synthetic cooked/raw MODE1 and audio files. No game image is embedded or downloaded. Run
`make -f tests/Makefile.chd-cd test` and
`make -f tests/Makefile.chd-cd sanitize`; sanitizer execution uses
`ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1` where enabled.

The backend derives from MAME `src/lib/util/cdrom.cpp` and
`src/devices/sound/cdda.cpp` at the commit above; it is an independent
implementation, not a claim that any particular arcade board boots.
See [chd_cd_LICENSE.txt](../libpcsxcore/arcade/chd_cd_LICENSE.txt) and the
libchdr notices under `deps/libchdr/`.

The host parser checks decimal fields without overflowing signed conversions,
limits hunks to 64 MiB, and rejects oversized track tables and unsupported
pregap subcode. It leaves the caller's sector buffer unchanged on read failure
and invalidates the cache before decompression. PCM callbacks can inspect
status; nested advance, close, sector reads and playback mutations are rejected.
Callbacks must not throw exceptions. These host guards do not change CD command
responses or supply missing media. Opening failures release the CHD handle,
including allocation failures; diagnostics use a bounded internal buffer.
