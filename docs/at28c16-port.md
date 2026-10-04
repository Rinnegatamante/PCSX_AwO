# Standalone AT28C16

`libpcsxcore/arcade/at28c16.c/.h` ports MAME's `src/devices/machine/at28c16.cpp/.h` from commit `deb2f54fe7300e7a192690318beb860159bfd52a`. The upstream BSD-3-Clause copyright belongs to smf; see `at28c16_LICENSE.txt`.

The chip stores 2,048 data bytes plus a 32-byte ID area. The port implements the upstream byte-write and high-voltage behavior with explicit emulated microsecond time. It does not invent page programming, delays, keys, or game-specific contents.

## API and storage

`at28c16_create(image, size)` copies the provided image:

- `NULL, 0` creates the upstream erased default, all 2,080 bytes set to 0xff.
- A 2,048-byte image supplies the optional default-data region; ID bytes remain erased.
- A 2,080-byte image supplies full persisted NVRAM, including ID bytes.
- Other pointer/length combinations fail.

Use `at28c16_destroy` to release the chip. There is deliberately no reset API: MAME does not define `device_reset` for this device. Keep the same chip instance and emulated clock alive across a board reset so stored bytes, pins, and an in-flight write continue unchanged.

`at28c16_get_image` and `at28c16_import_image` require exactly 2,080 bytes. Both operate on the stored image, independently of busy polling. Import leaves the pin state, current emulated time, last-write value, and pending completion event intact, matching upstream NVRAM loading. File I/O and persistence belong to the board or frontend; the chip never writes a host file.

## Writes and explicit time

Call `at28c16_advance_to_us(chip, absolute_time)` before timed bus operations. Time is monotonic and measured in emulated microseconds; equal timestamps are valid. A rewind fails without changing state. No host wall clock is used.

An ordinary changed-byte write stores its new byte immediately and schedules completion at `now_us + 200`. Until that exact deadline, every `at28c16_read` returns `last_write ^ 0x80`, regardless of the requested offset or A9/OE pins. At the deadline the device becomes idle and normal reads resume. This is upstream's global D7 polling behavior, not an address-specific handshake.

Busy writes are ignored and do not extend the deadline. Writing the already-stored byte while idle starts no event. The write API returns 1 when a changed write/erase starts busy, 0 for an ignored/same-value write, and -1 for invalid inputs or a deadline that would overflow the 64-bit microsecond clock. Writes that cannot schedule a deadline do not mutate the stored bytes.

Example: a 0x35 write at 100 µs stores 0x35 immediately; all polling reads return 0xb5 through 299 µs; advancing to 300 µs completes the write and reads expose stored data again.

## High-voltage pins and address mapping

`at28c16_set_a9_12v` and `at28c16_set_oe_12v` retain the low bit of their integer input, exactly as upstream.

With A9 at 12 V, offsets at or above 0x7e0 add 32 before accessing storage. Consequently logical offsets 0x7e0–0x7ff access ID storage at 0x800–0x81f; lower offsets keep accessing ordinary data. Without high-voltage A9, offsets 0x7e0–0x7ff access the final 32 ordinary data bytes. The API also permits direct backing offsets 0x800–0x81f with A9 low, matching the source memory region, although normal board wiring supplies an 11-bit address.

With OE at 12 V, any idle write erases all 2,080 bytes to 0xff, irrespective of address, data, or A9. It sets `last_write = 0xff`, so polling returns 0x7f for 200 µs. An erase starts busy even when all bytes were already erased. Setting the OE pin alone does not erase memory; a write is required. An attempted erase while busy is ignored.

Idle reads beyond mapped storage return 0xff. Idle ordinary writes beyond storage fail with -1 instead of forwarding to MAME's unmapped memory handlers; erase and busy polling continue to ignore the supplied address. This is a bounded adapter behavior for invalid offsets, not a new physical addressing mode.

## Snapshots

`at28c16_get_state` returns a caller-owned `AT28C16State`: full image, both high-voltage pins, last-write value, current time, and pending busy deadline. `at28c16_set_state` validates it before replacing the state. Idle snapshots require `last_write = -1` and no deadline. Busy snapshots require a byte-valued last write and a future deadline at most 200 µs away.

Snapshot restore intentionally replaces the time origin, including restoring an earlier saved time. Ordinary `advance_to_us` still rejects rewinds. These are structured in-memory snapshots; a frontend should serialize named fields explicitly rather than assuming C struct padding or host endianness is a portable file format.

## Validation

```sh
make -f tests/Makefile.at28c16 test
make -f tests/Makefile.at28c16 BUILD=build/at28c16-sanitize \
  CFLAGS='-std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' test
```

The deterministic suite covers default/data/full-image validation, copied caller buffers, exact 200 µs deadlines, D7 polling at every offset, ignored busy writes, same-value idle writes, monotonic time rejection, safe time-overflow failure, both ID boundaries and pin low-bit behavior, whole-image erase including ID bytes, already-erased erase timing, full NVRAM import/export, import during busy, in-flight snapshot restore, and invalid snapshot rejection.

This device has not been integrated into a board by this change, and the tests use synthetic image contents. There is no board-boot claim.
