# Standalone ZOOM ZSG-2 sound device

`libpcsxcore/arcade/zsg2.c/.h` ports the complete implemented device algorithm
from MAME `src/devices/sound/zsg2.cpp/.h`, revision
`deb2f54fe7300e7a192690318beb860159bfd52a`. The upstream copyright holders are
Olivier Galibert, R. Belmont, hap and superctr. Their attribution and full
BSD-3-Clause redistribution terms are retained in `zsg2.LICENSE`.

The chip has **48 channels**, 16 words per channel and four outputs. This is a
sample synthesizer; it does not require invented firmware. The caller supplies
borrowed packed sample ROM bytes or the source external-memory callback. Its
ROM size must be nonzero and divisible by four. Direct bytes are interpreted as
logical little-endian 32-bit packed words, matching `ROM_REGION32_LE`. External
reads return those logical words and receive a word offset, bounded by the
configured region length. G-Net's source uses its region for length and reads
three external flash devices; that mapping/banking belongs to the caller, not
a fictitious bank register inside the ZSG-2. Bounds reads return zero exactly
as the source does, and do not invoke the external callback.

## Implemented behavior

The implementation includes packed 7-bit mantissa/4-bit scale decompression,
word-change cache validation, both source filters, interpolation, pitch and
loop/end behavior, volume and filter ramps, key-on/off groups, current status
and volume reads, ROM address/readback, all register storage and source output
gains/polarity. The four independent PCM16 outputs are, in source order:

| Output | Source role |
|---:|---|
| 0 | Reverb send |
| 1 | Chorus send |
| 2 | Direct left |
| 3 | Direct right |

This matches `sound_stream::put_int_clamp(..., 32768)`, whose pinned definition
clamps to -32768 through +32767 (`src/emu/sound.h:247`). No sends are folded into
stereo here. The Taito Zoom configuration routes sends at gain 0.5 and direct
outputs at gain 1.0 into the separate TMS57002/ZFX-2, whose outputs 2/3 become
left/right (`src/mame/sony/taito_zm.cpp`). That downstream DSP and the MN10200
sound CPU are not part of this component.

Two upstream quirks are preserved literally. For a zero or out-of-range packed
word, `prepare_samples` returns `decoded + rom_words`, not the allocated final
empty block at `decoded + rom_words * 4`. This valid in-bounds cache alias can
contain previously decoded samples. It is not reinterpreted as intentional
silence. Interpolation narrows the difference of adjacent samples to signed
16 bits before multiplying, including its wrap at a large transition.

Signed left shifts and possible output-filter overflows in the original are
expressed through explicit two's-complement narrowing, wrapping and arithmetic
right shifts. This defines their intended machine arithmetic without C signed
undefined behavior. The source -1 dB gain approximation, ignored initial
volume on key-on, hardcoded key-on volume delta 0x0400, approximated ramp/filter
behavior and stop/loop rules remain intact.

## Register and lifecycle contract

`zsg2_create` requires an explicit input clock and valid ROM region/reader.
The Taito Zoom source uses 25 MHz; no clock or sample contents are guessed.
Create initializes fresh channel/cache storage and performs the source reset.
The source does not explicitly initialize its unknown control register array;
the standalone allocation starts that unspecified storage at zero for a
reproducible API. This does not assign hardware semantics to those unknown
registers. Later writes and reset retention match the source.

Read/write offsets are 16-bit **word offsets**, not byte addresses. Channel
words are 0x000..0x2ff and controls 0x300..0x31f; the Taito MN10200 wrapper maps
the chip at byte addresses 0x800000..0x8007ff. Only mask 0xffff is supported by
MAME. Partial writes are ignored before a stream-update barrier; partial reads
return zero and likewise do not flush the batch. The standalone result
`ZSG2_MASK_UNSUPPORTED` and optional diagnostic callback make this source
warning observable. Misaligned ROM readback addresses retain the source
truncated address and issue a diagnostic. Unknown controls retain source
register storage where applicable and issue diagnostics; no invented meaning
is assigned. Control 0x314 reads zero, following the upstream implementation's
"memory bus busy?" register.

`reset` clears the read address, keys off all channels, writes zero through all
channel register handlers and resets the source batch counter. In particular,
zero pitch means step 1, not step 0. Position/step phase, sample history and
filter history remain, as do unknown control registers and the decode cache.
The standalone clock, rational phases and lifetime frame/diagnostic counters
remain monotonic. Bring the device to the reset time first. `get_status` and
`get_channel` provide inspection without a stream update. `decode_block` is a
side-effecting cache diagnostic, not a hardware register or supplied media.
Callbacks must not reenter or destroy the device. Audio, external-ROM and
diagnostic callbacks run under a guard: nested read/write/run/decode/reset or
update/render operations return `ZSG2_REENTRANT` without mutation. Reset now
returns an explicit result, including this rejection; inspection remains
available during callbacks. Destroy inside a callback is ignored.

## Source batches and standalone clock scheduling

This distinction matters: the pinned ZSG-2 increments `m_sample_count` **once
at the end of `sound_stream_update`**, not once per sample. Volume/cutoff ramps
run on every frame of odd-numbered update batches, and on none of the frames
of even-numbered batches. Replacing this with ordinary per-frame alternation
would change the implemented MAME behavior.

`zsg2_render_update` is the literal batch API. It renders the requested number
of frames with one batch parity and increments that counter once. An explicit
zero-frame call also increments it, matching the source function body. It
accepts an optional interleaved four-output buffer and/or the configured PCM
callback. It does not advance chip-clock time; it is useful for explicit
source-equation comparisons and hosts supplying exact batch histories.

The clocked API uses `zsg2_run_cycles` or monotonic `zsg2_advance_to_cycle`.
Its stream frequency is the source integer `floor(clock_hz / 768)`, so 25 MHz
produces 32,552 frames/second, with rational phase retained between budgets.
Callbacks occur at each completed frame's exact input-clock cycle. Ordinary
run slicing does not change the source batch counter. The standalone host
model closes nonempty batches at 50 Hz and at valid read/write accesses;
`zsg2_stream_update` is an additional explicit nonempty-batch barrier.

The 50 Hz default is grounded in pinned `src/emu/sound.h:398`
(`STREAMS_UPDATE_FREQUENCY = 50`), `sound.cpp:47` (derived interval),
`sound.cpp:771-772` (periodic update timer) and `sound.cpp:2672-2697` (ordered
stream updates). Device MMIO calls its own `m_stream->update` in `zsg2.cpp`.
However, MAME routed dependencies may introduce other update boundaries, and
`sound_stream::update/update_nodeps` at `sound.cpp:588-635` includes the current
sample index with a +1 lookahead. The standalone clocked API deliberately
uses completed frames. It is a documented host scheduling model, **not a claim
that every clock slice reproduces an arbitrary full MAME machine's batch
history**. Whole and split budgets agree when they use the same MMIO/explicit
update history. An oracle must supply the literal MAME batch sequence through
the explicit batch API if that distinction affects its trace.

## Validation and limits

```sh
make -f tests/Makefile.zsg2 test
make -f tests/Makefile.zsg2 BUILD=build/zsg2-san \
  CFLAGS='-std=c99 -O0 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
  CXXFLAGS='-std=c++17 -O0 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' test
```

The tests use only clearly marked synthetic packed sample ROMs. They cover
2,048 independent compressed-block vectors, scalar mathematical reference
frames and state across four pitches, interpolation/filter/loop/end behavior,
all 256 ramp values, batch-parity envelope behavior, four independent gains
and polarity, 48-channel clipping, readback and masks, key groups, bounded
external memory, cache alias/update behavior, selective reset, retained clock
phase, callback timing/reentry rejection and whole-versus-split schedules with shared MMIO
boundaries. Both normal and ASAN/UBSAN runs pass. The public header compiles as
C99 and the implementation has no MAME runtime dependencies beyond the linked
standard math library.

Upstream already notes uncertain filters/ramps, possible bad sample dumps with
clicks/pops in G-Darius/RayStorm and occasional out-of-range reads. This port
preserves those implemented behaviors and makes access diagnostics visible.
No authentic Taito sample media, complete Zoom board/DSP, full-machine MAME
oracle, game boot or authentic audio-fidelity result has been verified here.
