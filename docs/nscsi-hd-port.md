# Standalone NSCSI hard-disk backend

`nscsi_hd` is the media and command layer for the existing standalone
`NscsiTarget`. `nscsi_hd_create(bus, id, config)` attaches a real target to the
bus and owns it until `nscsi_hd_destroy`. A configuration with no mounted media
returns `NULL` without attaching an endpoint, matching MAME's `m_scsi_id = -1`;
the caller must not substitute an always-ready target. The caller supplies
geometry and sector read/write callbacks. Callback success is `1`, failure is
`0`; a missing write callback models read-only media.

The port follows MAME `src/devices/bus/nscsi/hd.cpp` and the base request-sense
and unknown-command handling in `src/devices/machine/nscsi_hle.cpp`, revision
`deb2f54fe7300e7a192690318beb860159bfd52a`. The implemented commands are
TEST UNIT READY, REZERO UNIT, REQUEST SENSE, READ/WRITE(6), INQUIRY, MODE
SENSE(6), START STOP UNIT, RECEIVE/SEND DIAGNOSTIC, SYNCHRONIZE CACHE(10),
FORMAT UNIT, READ CAPACITY(10), READ/WRITE(10), MODE SELECT(6), and VERIFY(10).
Other opcodes use the base HLE result: CHECK CONDITION with ILLEGAL REQUEST and
zero ASC/ASCQ. VERIFY(10) with BYTCHK set uses that same result.

The adapter preserves these source details:

- REQUEST SENSE validates CDB bytes 1–3 and transfers at most 18 bytes. Fixed
  sense uses the source response byte `0xf0`, additional length 10, and the
  stored key/ASC/ASCQ. Invalid fields use key 5 and ASC `0x24`; bad LUNs use
  key 5 and ASC `0x25`.
- INQUIRY page 0 returns at most 56 bytes. Mounted media without IDNT metadata
  receives the source's ` SEAGATE          ST225N1.00` identity and command
  extension bytes. Supplied IDNT bytes replace the 48 bytes at offsets 8–55.
  Unsupported INQUIRY pages complete GOOD without a data phase.
- MODE SENSE(6) returns the source block descriptor and pages 00, 01, 02, 03,
  04, 08, and 30. Page `3f` returns supported pages in the source's descending
  page order. Unsupported individual pages set key 5, ASC `0x24`.
- READ(6) transfer length zero means 256 blocks. READ(10) transfer length zero
  means no data bytes, but still performs the source's initial sector read.
  An initial read failure returns CHECK CONDITION, key 5, ASC `0x24`; a later
  read failure returns zero bytes and increments a diagnostic counter, matching
  the source's zero-filled block behavior.
  The preflight does not update the source's `cur_lba`: the first data byte
  reads the first sector again when the prior cache LBA differs. A preflight
  failure preserves the old block; only lazy read failures clear it. Tests
  exercise the distinct callback counts and both failure paths.
- WRITE(6/10) calls the write-sector callback on the first LBA before DATA OUT,
  using the current 512-byte block buffer. A failed preflight returns CHECK
  CONDITION, key 5, ASC `0x24`. Payload bytes are written at each sector
  boundary. Later write failures are recorded but do not change the GOOD status,
  as in the source. MODE SELECT(6) reuses buffer ID 2 and the source's current
  LBA/block state.
- FORMAT UNIT sends a zero-filled sector to the callback and preserves the
  source loop expression `cylinder * head * sector`. Its write failures do not
  alter the GOOD status.

MAME's fresh `block[512]` member has indeterminate contents. This adapter
zero-initializes it so C/C++ undefined behavior does not become nondeterministic
media writes, and sets `NSCSI_HD_FAULT_UNINITIALIZED_PREFLIGHT` when the first
WRITE preflight has no preceding read. A preceding READ populates the buffer;
the next WRITE preflight writes those old bytes to its first LBA before the
payload overwrites the sector. This deliberately keeps the defined part of the
source side effect.

The source hard-disk class has an optional cylinder-aware seek model, but the
GQ target uses the default immediate command delay because its driver does not
call `set_seek_timing`. This API does not expose an approximate seek control;
adding one needs a corresponding target scheduler delay hook. Sector sizes
above 512 bytes and geometries whose block count exceeds the 32-bit source LBA
range are rejected rather than risking the source's fixed-buffer or arithmetic
limits. No host sleeps, fabricated sectors, or image-format parsing occur here.
