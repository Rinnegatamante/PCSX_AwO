# CHD-backed NCR and SCSI hard disk integration test

Run `make -f tests/Makefile.chd-scsi-hd test` to generate a local 2-cylinder,
2-head, 5-sector-per-track, 512-byte CHD with 1536-byte hunks and exercise it
through the NCR 53CF96 controller, SCSI bus, and hard-disk command target. The
test needs `chdman` and the repository's pinned libchdr checkout. It uses only
generated sectors and does not download or inspect game media.

The controller DMA sequence checks READ CAPACITY and INQUIRY, reads a generated
sector with READ(10), and writes another with WRITE(10). The target's source
preflight behavior is retained: after the read, WRITE first stores the prior
512-byte block at the write LBA, then DATA OUT replaces it with the payload.
The test confirms the payload by reading it back through NCR DMA, checks GOOD
status, COMMAND COMPLETE acceptance and bus disconnect, flushes and reopens the
overlay, then verifies the persisted sector. It hashes the generated source CHD
before and after the run to ensure writes stayed in the overlay.

Run `make -f tests/Makefile.chd-scsi-hd sanitize` for AddressSanitizer and
UndefinedBehaviorSanitizer coverage (`UBSAN_OPTIONS=halt_on_error=1`). Build
outputs stay under `build/chd-scsi-hd-tests/`.
