# Standalone NSCSI bus and target layer

This layer ports the open-collector bus resolver from MAME `nscsi_bus.cpp/.h`
and the common target-side `nscsi_full_device` protocol state machine from
`nscsi_hle.cpp/.h`. The source is pinned to MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`. The borrowed device code is
BSD-3-Clause; see [`nscsi_LICENSE.txt`](../libpcsxcore/arcade/nscsi_LICENSE.txt).

`nscsi_bus` provides sixteen SCSI-ID slots, wired-OR control and data levels,
per-endpoint wait masks, and source-style notifications only when a watched
control bit changes. Each notification stores the corresponding external bus
snapshot. Call `nscsi_bus_flush` after the chip operation that emitted pins,
outside the originating chip call. It delivers every stored per-mutation
snapshot in order, including changes made by callbacks, and never collapses
REQ/ACK edges. The explicit queue is a host-side adaptation because
MAME calls the peer inline from `regen_ctrl`; it preserves transition order,
but does not claim MAME scheduler callback timing. Overflow is reported in bus
status and stops delivery. Detaching an endpoint releases its control lines
and notifies remaining waiters; queued notifications carry an endpoint
generation, so detach/re-attach cannot reroute an old event. Bus destruction
during callback dispatch is refused and diagnosed. Destroy targets before the
bus they reference.

`nscsi_target` implements the common selection settle (400 ns), BSY response,
SEL-drop handling, optional IDENTIFY message, CDB receive, and REQ/ACK byte
transfer for data, status, and message phases. It retains MAME's opcode-group
CDB length rules for the reachable `opcode >> 5` groups 0 through 7, including
the source's variable-length treatment for groups 3, 4, and 6. The source also
contains cases 8 and 9, but those values cannot result from shifting an
8-bit opcode and are therefore unreachable. The phase queue has the source
capacity of 32 actions. Integer
nanoseconds preserve the source 400 ns target settle time; a caller clock on a
62.5 ns grid must choose a consistent conversion policy and expect at most one
caller tick of quantization. Bus mutations are not time-stamped by this bus.
The source initiator-ID scan is preserved literally: it advances while the
data bit is set and records the first unset bit (skipping the target ID). For
IDs 4 and 7 both selected, this reports ID 0; it is not a normal first-set-bit
scan. The source state also responds to any watched control change in the
selection-settle state, before the 400 ns timer if one arrives first; the timer
itself remains pending after that transition. The default target wait mask
watches SEL, BSY, and RST. Timer catch-up invokes each overdue event at its
scheduled deadline before advancing the visible clock to the caller's budget.

Reset behavior follows the pinned implementation: asserted bus RST clears the
target state, response queue, identify and buffer positions, and releases its
lines. The already scheduled target timer is retained. An explicit device
reset also leaves that timer pending, matching MAME's timer behavior. Timer
time is absolute and monotonic; rewind and arithmetic overflow set diagnostics.

Target command semantics and media remain external. Creation requires a real
backend callback; there is no built-in disk, fabricated READY response, or
default SCSI device. The backend receives the complete CDB and queues data
phases/status during that command callback, with separate byte callbacks for
its own buffers. Missing byte callbacks reject the requested phase before any
transfer byte is produced or consumed. CD-ROM and hard-disk commands,
sense/media policies, and image
transport are separate follow-on work. An absent target is simply an absent
bus endpoint, allowing the NCR controller's own selection timeout to occur.

Run `make -f tests/Makefile.nscsi test sanitize`. Tests cover per-edge wait filtering,
detach/re-attach generation checks, queue overflow, full IDENTIFY and
six-byte INQUIRY CDB transfer, data-in and data-out, status and command
completion with a test-only backend, target reset during selection, and an
absent ID. A real NCR 53CF96 is connected directly to the resolver and
transfers the full CDB, DMA data-in bytes, status byte, and command-complete
message against that fixture backend, then issues `CI_MSG_ACCEPT` and checks
the source disconnect interrupt and released bus lines. The test also follows
the controller's source selection-timeout deadline before the target
responds. These protocol fixtures are not runtime media profiles.
