#include "k054539.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PCM_SPACE 0x400000u

static uint8_t pcm_space[PCM_SPACE];
static uint32_t last_read_address;
static uint64_t read_count;

typedef struct AudioTrace {
    int32_t left[256];
    int32_t right[256];
    uint64_t cycle[256];
    size_t count;
} AudioTrace;

typedef struct TimerTrace {
    uint64_t cycle[16];
    int state[16];
    size_t count;
} TimerTrace;

typedef struct PanTrace {
    double left, right;
    unsigned count;
} PanTrace;

typedef struct ReentrantTrace {
    K054539 *chip;
    unsigned seen;
} ReentrantTrace;

static void probe_reentrant(ReentrantTrace *trace, unsigned bit)
{
    K054539 *chip = trace->chip;
    if (trace->seen & bit)
        return;
    trace->seen |= bit;
    assert(k054539_in_callback(chip));
    (void)k054539_sample_rate(chip);
    (void)k054539_cycles(chip);
    (void)k054539_read(chip, 0x22c, 1); /* side-effect-disabled read allowed */
    assert(k054539_advance_cycles(chip, 0, NULL, NULL) == -2);
    assert(k054539_write(chip, 0x22c, 0) == -2);
    assert(k054539_set_gain(chip, 0, 2.0) == -2);
    k054539_set_flags(chip, 0);
    k054539_reset(chip);
    k054539_destroy(chip); /* refused; caller retains ownership */
    assert(k054539_read(chip, 0x22d, 1) == 0xff); /* blocks recursive source */
    assert(k054539_callback_fault(chip));
}

static uint8_t reentrant_sample_read(void *opaque, uint32_t address)
{
    ReentrantTrace *trace = (ReentrantTrace *)opaque;
    probe_reentrant(trace, 1);
    return address < PCM_SPACE ? pcm_space[address] : 0xee;
}

static void reentrant_sample_sink(void *opaque, uint64_t cycle,
                                  int32_t left, int32_t right)
{
    ReentrantTrace *trace = (ReentrantTrace *)opaque;
    (void)cycle;
    (void)left;
    (void)right;
    probe_reentrant(trace, 2);
}

static void reentrant_timer(void *opaque, uint64_t cycle, int state)
{
    ReentrantTrace *trace = (ReentrantTrace *)opaque;
    (void)cycle;
    (void)state;
    probe_reentrant(trace, 4);
}

static void reentrant_pan(void *opaque, double left, double right)
{
    ReentrantTrace *trace = (ReentrantTrace *)opaque;
    (void)left;
    (void)right;
    probe_reentrant(trace, 8);
}

static uint8_t sample_read(void *opaque, uint32_t address)
{
    (void)opaque;
    last_read_address = address;
    ++read_count;
    return address < PCM_SPACE ? pcm_space[address] : 0xee;
}

static void sample_sink(void *opaque, uint64_t cycle, int32_t left, int32_t right)
{
    AudioTrace *trace = (AudioTrace *)opaque;
    assert(trace->count < 256);
    trace->cycle[trace->count] = cycle;
    trace->left[trace->count] = left;
    trace->right[trace->count] = right;
    ++trace->count;
}

static void timer_sink(void *opaque, uint64_t cycle, int state)
{
    TimerTrace *trace = (TimerTrace *)opaque;
    assert(trace->count < 16);
    trace->cycle[trace->count] = cycle;
    trace->state[trace->count] = state;
    ++trace->count;
}

static void pan_sink(void *opaque, double left, double right)
{
    PanTrace *trace = (PanTrace *)opaque;
    trace->left = left;
    trace->right = right;
    ++trace->count;
}

static K054539 *new_chip(K054539TimerLine timer, K054539AnalogPan pan,
                         void *opaque, int flags)
{
    K054539Config config;
    config.clock_hz = K054539_DEFAULT_CLOCK_HZ;
    config.flags = flags;
    config.sample_read = sample_read;
    config.timer_line = timer;
    config.analog_pan = pan;
    config.opaque = opaque;
    return k054539_create(&config);
}

static void wr(K054539 *chip, uint32_t offset, uint8_t data)
{
    assert(k054539_write(chip, offset, data) == 0);
}

static void wr24(K054539 *chip, uint32_t offset, uint32_t value)
{
    wr(chip, offset, (uint8_t)value);
    wr(chip, offset + 1, (uint8_t)(value >> 8));
    wr(chip, offset + 2, (uint8_t)(value >> 16));
}

static void setup_voice(K054539 *chip, unsigned ch, unsigned type,
                        unsigned start, unsigned pitch, unsigned pan,
                        unsigned loop, unsigned reverse)
{
    uint32_t base = ch * 0x20;
    wr24(chip, base, pitch);
    wr(chip, base + 3, 0); /* full source volume */
    wr(chip, base + 4, 0);
    wr(chip, base + 5, (uint8_t)pan);
    wr24(chip, base + 8, start);
    wr(chip, 0x200 + ch * 2, (uint8_t)(type | (reverse ? 0x20 : 0)));
    wr(chip, 0x201 + ch * 2, (uint8_t)(loop ? 1 : 0));
}

static void keyon(K054539 *chip, unsigned mask)
{
    wr(chip, 0x22f, 1); /* PCM on; position updates latch until key-on */
    wr(chip, 0x214, (uint8_t)mask);
}

static void start_position(K054539 *chip, unsigned ch, unsigned pos)
{
    uint32_t base = ch * 0x20 + 0x0c;
    wr24(chip, base, pos);
}

static void test_pcm8_end_and_position_latch(void)
{
    K054539 *chip = new_chip(NULL, NULL, NULL, K054539_FLAG_DISABLE_REVERB);
    AudioTrace trace = {0};
    assert(chip != NULL);
    assert(k054539_sample_rate(chip) == 48000);
    pcm_space[0] = 0x7f;
    pcm_space[1] = 0x40;
    pcm_space[2] = 0x20;
    pcm_space[3] = 0x80; /* non-looping 8-bit end marker */
    setup_voice(chip, 0, 0, 0, 0x10000, 0x18, 0, 0);
    keyon(chip, 1);
    assert((k054539_read(chip, 0x22c, 0) & 1) != 0);
    assert(k054539_advance_cycles(chip, 384u * 3u, sample_sink, &trace) == 0);
    assert(trace.count == 3);
    assert(trace.cycle[0] == 384 && trace.cycle[1] == 768 && trace.cycle[2] == 1152);
    assert(trace.left[0] == (int32_t)(16384.0 * 0.25 * sqrt(7.0 / 14.0)));
    assert(trace.right[0] == trace.left[0]);
    assert(trace.left[1] == (int32_t)(8192.0 * 0.25 * sqrt(7.0 / 14.0)));
    assert(trace.left[2] == 0);
    assert((k054539_read(chip, 0x22c, 0) & 1) == 0);
    assert(k054539_read(chip, 0x0c, 0) == 3);
    assert(k054539_cycles(chip) == 1152);
    k054539_destroy(chip);
}

static void test_loop_reverse_and_dpcm(void)
{
    K054539 *loop_chip = new_chip(NULL, NULL, NULL, K054539_FLAG_DISABLE_REVERB);
    K054539 *reverse_chip = new_chip(NULL, NULL, NULL, K054539_FLAG_DISABLE_REVERB);
    K054539 *dpcm_chip = new_chip(NULL, NULL, NULL, K054539_FLAG_DISABLE_REVERB);
    K054539 *dpcm_end = new_chip(NULL, NULL, NULL, K054539_FLAG_DISABLE_REVERB);
    K054539 *reverse16 = new_chip(NULL, NULL, NULL, K054539_FLAG_DISABLE_REVERB);
    AudioTrace trace = {0};
    assert(loop_chip && reverse_chip && dpcm_chip && dpcm_end && reverse16);

    pcm_space[0] = 0x30;
    pcm_space[1] = 0x10;
    pcm_space[2] = 0x80;
    setup_voice(loop_chip, 0, 0, 0, 0x10000, 0x18, 1, 0);
    keyon(loop_chip, 1);
    assert(k054539_advance_cycles(loop_chip, 384u * 2u, sample_sink, &trace) == 0);
    assert((k054539_read(loop_chip, 0x22c, 0) & 1) != 0);
    assert(k054539_read(loop_chip, 0x0c, 0) == 0); /* looped to start */
    assert(trace.left[1] == (int32_t)(12288.0 * 0.25 * sqrt(7.0 / 14.0)));

    memset(&trace, 0, sizeof(trace));
    pcm_space[1] = 0x10;
    pcm_space[2] = 0x30;
    pcm_space[3] = 0x40;
    pcm_space[4] = 0x80;
    setup_voice(reverse_chip, 0, 0, 3, 0x10000, 0x18, 0, 1);
    keyon(reverse_chip, 1);
    start_position(reverse_chip, 0, 3);
    keyon(reverse_chip, 1);
    assert(k054539_advance_cycles(reverse_chip, 384, sample_sink, &trace) == 0);
    assert(k054539_read(reverse_chip, 0x0c, 0) == 2);
    assert(trace.left[0] == (int32_t)(12288.0 * 0.25 * sqrt(7.0 / 14.0)));

    memset(&trace, 0, sizeof(trace));
    pcm_space[4] = 0x34;
    pcm_space[5] = 0x12;
    setup_voice(reverse16, 0, 0x04, 6, 0x10000, 0x18, 0, 1);
    keyon(reverse16, 1);
    start_position(reverse16, 0, 6);
    keyon(reverse16, 1);
    assert(k054539_advance_cycles(reverse16, 384, sample_sink, &trace) == 0);
    assert(trace.left[0] == (int32_t)(0x1234 * 0.25 * sqrt(7.0 / 14.0)));
    assert(k054539_read(reverse16, 0x0c, 0) == 4);

    memset(&trace, 0, sizeof(trace));
    pcm_space[0] = 0x10; /* high nibble +1 => delta +0x100 */
    pcm_space[1] = 0x02; /* low nibble +2 => accumulated delta +0x200 */
    setup_voice(dpcm_chip, 0, 0x08, 0, 0x10000, 0x18, 0, 0);
    keyon(dpcm_chip, 1);
    assert(k054539_advance_cycles(dpcm_chip, 384u * 2u, sample_sink, &trace) == 0);
    assert(trace.left[0] == (int32_t)(256.0 * 0.25 * sqrt(7.0 / 14.0)));
    assert(trace.left[1] == (int32_t)(768.0 * 0.25 * sqrt(7.0 / 14.0)));

    memset(&trace, 0, sizeof(trace));
    pcm_space[0] = 0x88; /* DPCM end marker */
    setup_voice(dpcm_end, 0, 0x08, 0, 0x10000, 0x18, 0, 0);
    keyon(dpcm_end, 1);
    assert(k054539_advance_cycles(dpcm_end, 384, sample_sink, &trace) == 0);
    assert(trace.left[0] == 0 && (k054539_read(dpcm_end, 0x22c, 0) & 1) == 0);

    k054539_destroy(loop_chip);
    k054539_destroy(reverse_chip);
    k054539_destroy(dpcm_chip);
    k054539_destroy(dpcm_end);
    k054539_destroy(reverse16);
}

static void test_pcm16_and_fractional_voice_mix(void)
{
    K054539 *chip = new_chip(NULL, NULL, NULL, K054539_FLAG_DISABLE_REVERB);
    K054539 *mix = new_chip(NULL, NULL, NULL, K054539_FLAG_DISABLE_REVERB);
    AudioTrace trace = {0};
    assert(chip && mix);
    pcm_space[2] = 0x20;
    pcm_space[3] = 0x40;
    pcm_space[4] = 0x00;
    pcm_space[5] = 0xc0;
    setup_voice(chip, 0, 0x04, 0, 0x10000, 0x18, 0, 0);
    keyon(chip, 1);
    assert(k054539_advance_cycles(chip, 384u * 2u, sample_sink, &trace) == 0);
    assert(trace.left[0] == (int32_t)(0x4020 * 0.25 * sqrt(7.0 / 14.0)));
    assert(trace.left[1] == (int32_t)(-16384.0 * 0.25 * sqrt(7.0 / 14.0)));

    memset(&trace, 0, sizeof(trace));
    pcm_space[0] = 0;
    pcm_space[1] = 0x01;
    setup_voice(mix, 0, 0, 0, 0x10000, 0x12, 0, 0);
    setup_voice(mix, 1, 0, 0, 0x10000, 0x12, 0, 0);
    assert(k054539_set_gain(mix, 0, 0.75) == 0);
    assert(k054539_set_gain(mix, 1, 0.75) == 0);
    keyon(mix, 3);
    assert(k054539_advance_cycles(mix, 384, sample_sink, &trace) == 0);
    assert(trace.left[0] == 25); /* sum fractional voices, then s32 truncate */
    assert(trace.right[0] == 92);

    /* The sample engine reports raw MAME s32 values; it does not saturate a
       multi-voice high-gain sum to signed 16-bit audio. */
    memset(&trace, 0, sizeof(trace));
    wr(mix, 0x215, 3);
    pcm_space[1] = 0x7f;
    start_position(mix, 0, 0);
    start_position(mix, 1, 0);
    wr(mix, 0x05, 0x18);
    wr(mix, 0x25, 0x18);
    assert(k054539_set_gain(mix, 0, 3.0) == 0);
    assert(k054539_set_gain(mix, 1, 3.0) == 0);
    wr(mix, 0x214, 3);
    assert(k054539_advance_cycles(mix, 384, sample_sink, &trace) == 0);
    assert(trace.left[0] > 32767 && trace.right[0] > 32767);
    assert(k054539_set_gain(mix, 8, 1.0) == -1);
    assert(k054539_set_gain(mix, 0, INFINITY) == -1);
    assert(k054539_set_gain(mix, 0, NAN) == -1);
    k054539_destroy(chip);
    k054539_destroy(mix);
}

static void test_register_port_and_reset(void)
{
    K054539 *chip = new_chip(NULL, NULL, NULL, 0);
    PanTrace pan = {0};
    K054539 *with_pan = new_chip(NULL, pan_sink, &pan, 0);
    uint64_t before_reads;
    assert(chip && with_pan);
    assert(k054539_write(chip, 0x230, 0) == -1);
    assert(k054539_read(chip, 0x230, 0) == 0xff);

    /* GQ ROM/RAM callback address space and the 0x22d cursor semantics. */
    pcm_space[0x80000] = 0x69;
    pcm_space[0x80001] = 0x96;
    wr(chip, 0x22e, 4);
    wr(chip, 0x22f, 0x10);
    before_reads = read_count;
    assert(k054539_read(chip, 0x22d, 1) == 0x69);
    assert(last_read_address == 0x80000 && read_count == before_reads + 1);
    assert(k054539_read(chip, 0x22d, 0) == 0x69);
    assert(last_read_address == 0x80000);
    assert(k054539_read(chip, 0x22d, 0) == 0x96);
    assert(last_read_address == 0x80001);
    wr(chip, 0x22e, 0x7f);
    assert(k054539_read(chip, 0x22d, 0) == 0xee);
    assert(last_read_address == 0xfe0000); /* outside GQ callback backing */
    wr(chip, 0x22e, 0x80);
    wr(chip, 0x22d, 0x34);
    wr(chip, 0x22d, 0x12);
    wr(chip, 0x22e, 0x80);
    assert(k054539_read(chip, 0x22d, 1) == 0x34);
    assert(k054539_read(chip, 0x22d, 1) == 0x34); /* no cursor side effect */
    assert(k054539_read(chip, 0x22d, 0) == 0x34);
    assert(k054539_read(chip, 0x22d, 0) == 0x12);

    wr(with_pan, 0x13f, 0x11);
    assert(pan.count == 1 && pan.left == 0.0 && pan.right == 1.0);

    k054539_reset(chip);
    assert(k054539_read(chip, 0x22c, 0) == 0);
    assert(k054539_read(chip, 0x22f, 0) == 0);
    wr(chip, 0x22f, 0x10);
    wr(chip, 0x22e, 0x80);
    assert(k054539_read(chip, 0x22d, 0) == 0); /* reset cleared reverb RAM */
    k054539_destroy(chip);
    k054539_destroy(with_pan);
}

static void test_timer_and_chunk_equivalence(void)
{
    TimerTrace timer = {0};
    AudioTrace whole = {0}, chunks = {0};
    K054539 *chip = new_chip(timer_sink, NULL, &timer, 0);
    K054539 *a = new_chip(NULL, NULL, NULL, K054539_FLAG_DISABLE_REVERB);
    K054539 *b = new_chip(NULL, NULL, NULL, K054539_FLAG_DISABLE_REVERB);
    float base_hz = ((float)K054539_DEFAULT_CLOCK_HZ / 384.0f) / 14400.0f;
    float frequency = 38.0f * base_hz;
    double period_cycles = (1.0 / (double)frequency / 2.0) *
                           K054539_DEFAULT_CLOCK_HZ;
    uint64_t due = (uint64_t)ceil(period_cycles);
    assert(chip && a && b);

    wr(chip, 0x227, 0);
    assert(timer.count == 1 && timer.cycle[0] == 0 && timer.state[0] == 0);
    wr(chip, 0x22f, 0x20);
    assert(k054539_advance_cycles(chip, due - 1, NULL, NULL) == 0);
    assert(timer.count == 1);
    assert(k054539_advance_cycles(chip, 1, NULL, NULL) == 0);
    assert(timer.count == 2 && timer.state[1] == 1 && timer.cycle[1] == due);
    wr(chip, 0x22f, 0x01); /* output disabled; scheduled timer still runs */
    assert(timer.count == 3 && timer.state[2] == 0);
    assert(k054539_advance_cycles(chip, due * 2, NULL, NULL) == 0);
    assert(timer.count == 3);
    wr(chip, 0x22f, 0x21);
    assert(k054539_advance_cycles(chip, due, NULL, NULL) == 0);
    assert(timer.count >= 4 && timer.state[timer.count - 1] == 1);
    k054539_reset(chip);
    {
        size_t count = timer.count;
        assert(k054539_advance_cycles(chip, due * 3, NULL, NULL) == 0);
        assert(timer.count == count); /* reset stops, without extra IRQ writes */
    }

    pcm_space[100] = 0x11;
    pcm_space[101] = 0x22;
    pcm_space[102] = 0x33;
    setup_voice(a, 0, 0, 99, 0x8000, 0x18, 1, 0);
    setup_voice(b, 0, 0, 99, 0x8000, 0x18, 1, 0);
    keyon(a, 1);
    keyon(b, 1);
    assert(k054539_advance_cycles(a, 384u * 4u, sample_sink, &whole) == 0);
    assert(k054539_advance_cycles(b, 17, sample_sink, &chunks) == 0);
    assert(k054539_advance_cycles(b, 367, sample_sink, &chunks) == 0);
    assert(k054539_advance_cycles(b, 384u * 3u, sample_sink, &chunks) == 0);
    assert(whole.count == chunks.count && whole.count == 4);
    assert(memcmp(whole.left, chunks.left, whole.count * sizeof(int32_t)) == 0);
    assert(memcmp(whole.right, chunks.right, whole.count * sizeof(int32_t)) == 0);
    k054539_destroy(chip);
    k054539_destroy(a);
    k054539_destroy(b);
}

static void test_reverb_wrap_and_timer_horizon(void)
{
    K054539 *chip = new_chip(NULL, NULL, NULL, 0);
    K054539 *timer = new_chip(NULL, NULL, NULL, 0);
    AudioTrace trace = {0};
    const uint64_t horizon = K054539_MAX_EXACT_TIMER_CYCLE;
    uint16_t expected;
    assert(chip && timer);

    /* Source gain is capped at 1.8, then narrowed as a 16-bit reverb word.
       The conversion is explicit wrap rather than undefined float-to-short. */
    pcm_space[0] = 0x7f;
    setup_voice(chip, 0, 0, 0, 0x10000, 0x18, 0, 0);
    assert(k054539_set_gain(chip, 0, 20.0) == 0);
    keyon(chip, 1);
    assert(k054539_advance_cycles(chip, 384, sample_sink, &trace) == 0);
    expected = (uint16_t)(int32_t)(32512.0 * 1.8);
    wr(chip, 0x22e, 0x80);
    wr(chip, 0x22f, 0x10);
    assert(k054539_read(chip, 0x22d, 0) == (uint8_t)expected);
    assert(k054539_read(chip, 0x22d, 0) == (uint8_t)(expected >> 8));

    /* Timer-enabled scheduling refuses a target beyond exact double cycles
       atomically; the quiet no-stream path makes the boundary test cheap. */
    wr(timer, 0x227, 0);
    assert(k054539_advance_cycles(timer, horizon, NULL, NULL) == -1);
    assert(k054539_cycles(timer) == 0);
    k054539_destroy(timer);

    timer = new_chip(NULL, NULL, NULL, 0);
    assert(timer != NULL);
    assert(k054539_advance_cycles(timer, horizon - 1, NULL, NULL) == 0);
    assert(k054539_cycles(timer) == horizon - 1);
    assert(k054539_write(timer, 0x227, 0) == -1);
    assert(k054539_read(timer, 0x227, 0) == 0); /* no partial register write */
    k054539_destroy(chip);
    k054539_destroy(timer);
}

static void test_final_sample_tick_near_uint64_limit(void)
{
    K054539 *chip = new_chip(NULL, NULL, NULL, 0);
    AudioTrace trace = {0};
    uint64_t start = UINT64_MAX - 300;
    uint64_t tick = UINT64_MAX - 255;
    assert(chip != NULL);
    assert(tick % K054539_CLOCK_DIVIDER == 0);
    assert(k054539_advance_cycles(chip, start, NULL, NULL) == 0);
    assert(k054539_cycles(chip) == start);
    assert(k054539_advance_cycles(chip, tick - start, sample_sink, &trace) == 0);
    assert(trace.count == 1 && trace.cycle[0] == tick);
    assert(k054539_cycles(chip) == tick);
    k054539_destroy(chip);
}

static void test_callback_reentrancy_guard(void)
{
    K054539Config config;
    ReentrantTrace trace = {0};
    K054539 *chip;
    AudioTrace output = {0};
    memset(&config, 0, sizeof(config));
    config.clock_hz = K054539_DEFAULT_CLOCK_HZ;
    config.flags = 0;
    config.sample_read = reentrant_sample_read;
    config.timer_line = reentrant_timer;
    config.analog_pan = reentrant_pan;
    config.opaque = &trace;
    chip = k054539_create(&config);
    assert(chip != NULL);
    trace.chip = chip;

    wr(chip, 0x22f, 0x11); /* enables the side-effecting sample port */
    wr(chip, 0x227, 0);    /* timer callback */
    wr(chip, 0x13f, 0x11); /* analog callback */
    setup_voice(chip, 0, 0, 0, 0x10000, 0x18, 0, 0);
    wr(chip, 0x214, 1);
    assert(k054539_advance_cycles(chip, 384, reentrant_sample_sink, &trace) == 0);
    assert(trace.seen == 15); /* timer, analog, sample-read and output callbacks */
    assert(k054539_callback_fault(chip));
    assert(!k054539_in_callback(chip));
    assert(k054539_read(chip, 0x22f, 1) == 0x11); /* reset was refused */
    assert(k054539_read(chip, 0x22c, 1) == 1);    /* voice remains keyed on */
    (void)output;
    k054539_destroy(chip);
}

int main(void)
{
    K054539Config invalid = {0};
    memset(pcm_space, 0, sizeof(pcm_space));
    invalid.clock_hz = K054539_DEFAULT_CLOCK_HZ;
    assert(k054539_create(&invalid) == NULL); /* sample source is mandatory */
    invalid.sample_read = sample_read;
    invalid.clock_hz++;
    assert(k054539_create(&invalid) == NULL); /* /384 hardware stream */
    test_pcm8_end_and_position_latch();
    test_loop_reverse_and_dpcm();
    test_pcm16_and_fractional_voice_mix();
    test_register_port_and_reset();
    test_timer_and_chunk_equivalence();
    test_reverb_wrap_and_timer_horizon();
    test_final_sample_tick_near_uint64_limit();
    test_callback_reentrancy_guard();
    puts("K054539 8-voice PCM/DPCM/register/timer tests PASS");
    return 0;
}
