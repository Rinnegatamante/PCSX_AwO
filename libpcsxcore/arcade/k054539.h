/*
 * Standalone Konami K054539 PCM engine.
 * MAME source: Olivier Galibert, BSD-3-Clause; see k054539_LICENSE.txt.
 */
#ifndef PCSX_ARCADE_K054539_H
#define PCSX_ARCADE_K054539_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    K054539_VOICES = 8,
    K054539_REGISTER_BYTES = 0x230,
    K054539_REVERB_BYTES = 0x8000,
    K054539_CLOCK_DIVIDER = 384,
    K054539_DEFAULT_CLOCK_HZ = 18432000,
    K054539_DEFAULT_SAMPLE_RATE = 48000,
    K054539_FLAG_REVERSE_STEREO = 1,
    K054539_FLAG_DISABLE_REVERB = 2,
    K054539_FLAG_UPDATE_AT_KEYON = 4
};

#define K054539_MAX_EXACT_TIMER_CYCLE UINT64_C(9007199254740991)

typedef struct K054539 K054539;

/* The ROM interface supplies the complete chip-visible 24-bit byte space.
 * For Konami GQ, the caller normally maps 0x000000..0x07ffff to the sample
 * ROM and 0x080000..0x3fffff to shared PCM RAM, and supplies the source
 * address-space open-bus byte outside mapped regions. */
typedef uint8_t (*K054539SampleRead)(void *opaque, uint32_t address);
/* Raw s32 values passed by MAME put_int(), before host mixer/WAV clipping. */
typedef void (*K054539SampleSink)(void *opaque, uint64_t chip_cycle,
                                  int32_t left, int32_t right);
typedef void (*K054539TimerLine)(void *opaque, uint64_t chip_cycle, int state);
typedef void (*K054539AnalogPan)(void *opaque, double left_gain,
                                 double right_gain);

typedef struct K054539Config {
    uint32_t clock_hz;
    int flags;
    K054539SampleRead sample_read; /* required; no synthetic sample backing */
    K054539TimerLine timer_line;   /* optional, like MAME's unbound delegate */
    K054539AnalogPan analog_pan;   /* optional callback from register 0x13f */
    void *opaque;                  /* shared callback context */
} K054539Config;

K054539 *k054539_create(const K054539Config *config);
void k054539_destroy(K054539 *chip);
uint32_t k054539_sample_rate(const K054539 *chip);
uint64_t k054539_cycles(const K054539 *chip);

/* Advance relative chip-clock cycles. Emits one raw sample callback for each
 * /384 stream tick and timer edges in source order. -1 reports bad input,
 * uint64 overflow, or a configured timer reaching the exclusive
 * K054539_MAX_EXACT_TIMER_CYCLE horizon, without advancing. -2 reports a
 * callback reentrancy attempt. Call before a ROM bank change as well as before
 * register/config writes. GQ's ordinary shared PCM RAM writes do not call a
 * stream barrier in MAME; board adapters must preserve that distinction.
 * Register writes, set_flags, and set_gain happen after this barrier, matching
 * MAME stream->update() on those operations. Reads do not force stream update. */
int k054539_advance_cycles(K054539 *chip, uint64_t cycles,
                           K054539SampleSink sink, void *sink_opaque);
/* -1 bad offset/timer horizon, -2 attempted reentrant mutation from callback. */
int k054539_write(K054539 *chip, uint32_t byte_offset, uint8_t data);
uint8_t k054539_read(K054539 *chip, uint32_t byte_offset,
                     int side_effects_disabled);

/* MAME init_flags replaces the flags; device start also defaults
 * UPDATE_AT_KEYON. REVERSE_STEREO is defined upstream but unused by its chip
 * source; stereo route reversal remains a board/mixer operation. */
void k054539_set_flags(K054539 *chip, int flags);
/* Invalid channel or non-finite gain returns -1; finite negative gain is
 * ignored as by MAME. */
int k054539_set_gain(K054539 *chip, unsigned channel, double gain);
/* MAME device_reset clears active mask/control, reverb RAM and timer schedule,
 * while retaining other registers, channel state, ROM port and timer level. */
void k054539_reset(K054539 *chip);
/* Callbacks are synchronous. Mutating API calls from a callback are rejected;
 * safe const reads and these diagnostic getters remain available. */
int k054539_in_callback(const K054539 *chip);
int k054539_callback_fault(const K054539 *chip);

#ifdef __cplusplus
}
#endif
#endif
