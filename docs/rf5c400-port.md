# Ricoh RF5C400 standalone port

The source is MAME `src/devices/sound/rf5c400.cpp/.h` at
`deb2f54fe7300e7a192690318beb860159bfd52a`, copyright Ville Linde, with
improvements credited to the hoot development team. The BSD-3-Clause notice
is retained in `libpcsxcore/arcade/rf5c400_LICENSE.txt`.

The module retains the source's 32 channels, 16-bit and low/high-byte PCM
formats, attenuation/pan tables, experimental envelope tables, inclusive
sample-end test, loop-offset handling, position polling, key-on/release/off,
external-memory registers and four output streams. Twinkle clocks this chip
at 16.9344 MHz, giving `floor(clock / 384) = 44100` output frames per second.
Its sound CPU maps word registers at byte addresses `400000..400fff`; the
chip has a separate 25-bit byte-addressed sample bus backed by 32 MiB RAM.

`RF5C400Config` borrows a little-endian sample ROM or delegates aligned
25-bit byte reads to an external memory callback. Writes through the memory
registers reach the supplied write callback, so a board can provide writable
sample RAM. ROM accesses past the borrowed size return `ffff`; writes do not
modify borrowed ROM. The chip's external address register retains its full
written value; the memory transaction applies the 25-bit bus mask.

The source ignores `mem_mask`, including partial register writes. The port
preserves that behavior rather than combining byte lanes inside the chip.
The host must advance to the access cycle before calling immediate register
read/write functions. The register APIs take **word offsets**, unlike the
sample-memory callbacks, which take byte addresses.

Clocked runs maintain a rational sample phase and call the output callback at
the exact completed-frame chip cycle. Whole and split advances with the same
MMIO history produce identical samples and callback timestamps. An optional
creation epoch starts the first sample one period after that epoch; reset
does not rewind the clock. The upstream device has no reset override, so
soft reset preserves its registers and channel/envelope state. The wrapper
initializes otherwise unspecified status/external-memory fields to zero on
creation; this is deterministic initialization, not a claim about hardware
power-on values.

Output values are unclipped signed sums in PCM units, with 32768 representing
full scale. Outputs 0/1 are direct left/right; 2/3 are external-effect sends.
Twinkle routes only 0/1 to its speakers. The port preserves the source's
integer truncation for each voice before mixing. An offline render API is
also available; it changes channel state but does not advance clock time or
invoke the clocked callback. Callbacks may inspect state, but recursive
mutation/render/advancement is rejected and destruction leaves an active
device intact.

Build and test with:

```sh
make -f tests/Makefile.rf5c400 test
make -f tests/Makefile.rf5c400 sanitize
```

Fixed vectors cover all PCM formats including negative samples, independent
direct/effect pans, reserved formats, 32-voice unclipped mixing, envelope
holds/release, exact pitch, inclusive loops, source register fallthrough,
memory-address masking and write strobes, ignored partial masks, position
polling, reset retention, callback reentry, clock overflow and whole/split
sample timestamps. The port also cross-compiles with the ARMv7 Vita SDK.

This establishes a sound-chip component. It does not integrate Twinkle's
MC68000, ATA sample uploader, dual-port RAM, media, DVD/session devices or
PSX board adapter, and does not validate a Twinkle game. Upstream marks the
chip's effects/envelopes imperfect; the port retains those approximations.
