# Standalone NSCSI CD-ROM target

This module composes the pinned MAME `nscsi_cdrom_device` command behavior with
the project’s `NscsiTarget` and open-collector `NscsiBus`. Source revision is
`deb2f54fe7300e7a192690318beb860159bfd52a`; borrowed code is BSD-3-Clause
(Olivier Galibert), with the complete notice in
[`nscsi_cd_LICENSE.txt`](../libpcsxcore/arcade/nscsi_cd_LICENSE.txt).
The model identity follows the base `nscsi_toshiba_xm5401_device` constructor:
TOSHIBA, `CD-ROM XM-5401TA`, revision `3605`, inquiry byte `0x98`, compliance
`0x02`. The XM-5401 class adds no command overrides or speed timing; the
separate 2x, Apple, SGI, Sun, and XM-3301/5301/5701 subclasses are outside this
module.

The base CD class overrides vendor-command CDB recognition: opcodes matching
`(opcode & 0xd0) == 0xd0` consume ten command bytes. The target's optional
recognition callback retains this rule before unsupported toolbox commands
return CHECK CONDITION. Ordinary command groups retain the generic behavior.
READ data phases stream through the sector callback without the response
buffer's 64 KiB limit. Tests include READ(6)'s zero-length field (256 blocks),
READ(10) above 64 KiB and a zero-block READ(10).

The caller must provide real-media callbacks for presence and sequence number,
MODE1 sector reads, track starts/lead-out/track lookup/ADR-control/type, and
CDDA start/pause/status/output gain. Creation fails if any callback is absent;
there is no built-in disc, READY response, fabricated TOC, or fake CDDA state.
The core does not parse CHD/CUE files or render audio. It leaves that work to a
media backend and audio mixer.

| SCSI operation | Behavior ported from the common MAME base |
| --- | --- |
| TEST UNIT READY, READ(6), READ(10) | Require inserted media; read one MODE1 2048-byte sector through the callback and cache it while sequential bytes are transferred |
| INQUIRY, REQUEST SENSE, REZERO | XM-5401 identity, fixed 18-byte sense data, and generic target behavior |
| MODE SELECT(6), MODE SENSE(6) | Mode pages and optional block descriptor; block size begins at 2048 and accepts any nonzero divisor of 2048, including 512 |
| READ CAPACITY(10), READ DISC INFORMATION, READ TOC/PMA/ATIP | Derive capacity, lead-in/lead-out, tracks, MSF conversion, and ADR/control bits from caller metadata |
| READ SUB-CHANNEL, PLAY AUDIO TRACK/INDEX, PAUSE/RESUME | Report caller CDDA state and issue playback controls through callbacks |
| START STOP, PREVENT/ALLOW, SEND/RECEIVE DIAGNOSTIC | Preserve the base device’s acknowledged operations and fixed diagnostic data |
| Unknown opcodes | CHECK CONDITION with ILLEGAL REQUEST, matching the generic target fallback |

A changed media sequence produces one UNIT ATTENTION CHECK CONDITION (except
INQUIRY), then updates the cached sequence and sector state. If a sector-read
callback fails during a transfer, the pinned MAME code returns a zero-filled
sector; this port follows that explicit source behavior and records a
`NSCSI_CD_FAULT_BAD_MEDIA_READ` diagnostic. An absent disc returns NOT READY
with ASC `0x3a` for media-required commands. Media callback pointers remain
mandatory even when the current drive reports no inserted disc.

The upstream source also contains D0–DA BlueSCSI/ZuluSCSI file-transfer
commands backed by MAME's host share directory. This project API has no share
filesystem, so those filesystem commands are rejected with CHECK CONDITION and
recorded as unsupported. The OS-specific file access is not emulated. The
XM-5401's block size defaults to 2048; 512-byte operation is available through
the source-compatible configuration/mode-select path for boards that request
it. Device reset retains the configured block size, matching the MAME base.
Mode parameter storage is retained across commands/reset and initialized to
zero at creation. The host adapter reads only transmitted complete descriptors
and audio pages; short/malformed parameters do not cause the upstream unchecked
array accesses. Zero/non-divisor block sizes leave the block size intact and
record `NSCSI_CD_FAULT_MALFORMED_MODE`.

A caller creates the `NscsiBus`, attaches its initiator endpoint, creates the
CD target at its configured SCSI ID, and advances absolute target time with
`nscsi_cdrom_advance_to`. Call `nscsi_bus_flush` after each controller pin
mutation, outside that controller's callback. Destroy the CD endpoint before
destroying the bus.

Run `make -f tests/Makefile.nscsi-cd test` and
`make -f tests/Makefile.nscsi-cd sanitize`. Tests use only an in-memory
synthetic disc fixture: they cover inquiry identity, LUN response, capacity at
2048/512-byte blocks, mode-select block size/audio page, TOC, media-change
attention, CDDA subchannel/play/pause callbacks, and a real NCR 53CF96 READ(10)
DMA transfer through the target and resolver followed by status, command
complete, message acceptance, and bus disconnect. The fixtures validate the
adapter protocol only; no original game disc is bundled or claimed to boot.
