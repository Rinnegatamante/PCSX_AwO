# Standalone 93C46 16-bit EEPROM

`libpcsxcore/arcade/eeprom93c46.c` ports the 64-word, 16-bit `EEPROM_93C46_16BIT`
behavior configured by Konami System GV. The protocol/state behavior follows
MAME `src/devices/machine/eepromser.cpp` and `eeprom.cpp` at
`deb2f54fe7300e7a192690318beb860159bfd52a`. The full source-specific BSD-3-Clause
notice is in `libpcsxcore/arcade/eeprom93c46_LICENSE.txt`.

The caller owns emulated time: advance the device to an absolute nanosecond
timestamp before changing CS, CLK, or DI. The microsecond helper is available
for coarse schedulers, while nanoseconds preserve distinct native edges. Time
rewinds fail. A rising CS enters the start-bit state; the first rising CLK edge
must happen at a later emulated timestamp, matching MAME's guard against
simultaneous CS/CLK edges. The internal integer-nanosecond grid quantizes
MAME's finer `attotime`; callers converting CPU cycles should keep a rational
remainder across advances instead of rounding each interval independently.
Start is a separate bit followed by two opcode and six address bits. DO returns READY
while waiting for a start bit, serial data during a READ, and the MAME pull-up
level in other states.

The supported commands are READ, WRITE, ERASE, EWEN, EWDS, WRAL, and ERAL.
Writes begin locked. Single-word write/erase busy times are 1,750,000/1,000,000
ns; WRAL/ERAL are 8,000,000 ns. Data changes when the command finishes shifting, as in
MAME's EEPROM base device; the busy interval controls READY. WRAL ANDs the
provided value with each existing word, while ERAL restores every word to
`0xffff`. A device reset clears the pending completion time but preserves the
stored image, pin levels, lock, and serial state, matching the upstream base
device reset behavior.

Images are exactly 128 bytes. Each word is serialized low byte first, matching
MAME `internal_read`/`internal_write` and its NVRAM byte array. A new chip with
no image starts erased. Import/export reject partial images; import changes
only stored bytes and leaves current protocol and timing state intact.

This module models the 16-bit 93C46 configuration used by System GV. It does
not claim the 8-bit ORG variant or other 93Cxx models. Run its synthetic serial
protocol and timing traces with:

```sh
make -f tests/Makefile.eeprom93c46 test sanitize
```

The tests also verify that READ keeps shifting pull-up ones for more than 512
clocks after the first word, a one-nanosecond CS/CLK separation is accepted,
device reset preserves lock and an in-progress read shift, and a serial write
near the maximum virtual timestamp returns `-2` without changing EEPROM bytes.
Overflow traces cover WRITE, ERASE, WRAL and ERAL.
