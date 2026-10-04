#include "ym2610.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

struct Trace {
    unsigned irq_changes = 0;
    bool irq = false;
    unsigned adpcm_a_reads = 0;
    unsigned adpcm_b_reads = 0;
    uint32_t last_address_a = 0;
    uint32_t last_address_b = 0;
};

static void irq(void *opaque, int state)
{
    Trace *trace = static_cast<Trace *>(opaque);
    ++trace->irq_changes;
    trace->irq = state != 0;
}

static uint8_t read_adpcm(void *opaque, YM2610RomBank bank, uint32_t address)
{
    Trace *trace = static_cast<Trace *>(opaque);
    if (bank == YM2610_ROM_ADPCM_A) {
        ++trace->adpcm_a_reads;
        trace->last_address_a = address;
    } else {
        ++trace->adpcm_b_reads;
        trace->last_address_b = address;
    }
    return static_cast<uint8_t>((address * 37 + (bank == YM2610_ROM_ADPCM_A ? 0x31 : 0x73)) & 0xff);
}

static YM2610Device *make_chip(YM2610Model model, Trace *trace = nullptr,
                               const uint8_t *rom = nullptr, size_t rom_size = 0)
{
    YM2610Config config{};
    config.model = model;
    config.clock_hz = 8000000;
    config.adpcm_a = rom;
    config.adpcm_a_size = rom_size;
    config.read_adpcm = trace != nullptr ? read_adpcm : nullptr;
    config.rom_opaque = trace;
    config.irq_line = trace != nullptr ? irq : nullptr;
    config.irq_opaque = trace;
    YM2610Device *chip = nullptr;
    assert(ym2610_create(&config, &chip) == YM2610_OK);
    assert(chip != nullptr);
    return chip;
}

static void write_reg(YM2610Device *chip, uint8_t reg, uint8_t value)
{
    ym2610_write(chip, 0, reg);
    ym2610_write(chip, 1, value);
}

static void write_reg_hi(YM2610Device *chip, uint8_t reg, uint8_t value)
{
    ym2610_write(chip, 2, reg);
    ym2610_write(chip, 3, value);
}

static std::vector<YM2610Sample> run(YM2610Device *chip, uint64_t cycles)
{
    size_t cap = static_cast<size_t>(cycles / 144 + 1);
    std::vector<YM2610Sample> result(cap);
    size_t count = 0;
    assert(ym2610_run_cycles(chip, cycles, result.data(), result.size(), &count) == YM2610_OK);
    result.resize(count);
    return result;
}

static void configure_ssg_tone(YM2610Device *chip)
{
    write_reg(chip, 0x00, 0x01);
    write_reg(chip, 0x01, 0x00);
    write_reg(chip, 0x07, 0x3e); /* channel A tone on, noise off */
    write_reg(chip, 0x08, 0x0f);
}

static void configure_fm_tone(YM2610Device *chip, uint8_t channel)
{
    /* Four operators with fast attack, no attenuation, audible pan, and a
     * sustained key on. Channel 1 exists on both YM2610 and YM2610B masks. */
    for (uint8_t slot = 0; slot < 4; ++slot) {
        static const uint8_t slot_bits[4] = {0x00, 0x08, 0x04, 0x0c};
        uint8_t op = slot_bits[slot];
        write_reg(chip, static_cast<uint8_t>(0x30 + op + channel), 0x01);
        write_reg(chip, static_cast<uint8_t>(0x40 + op + channel), 0x00);
        write_reg(chip, static_cast<uint8_t>(0x50 + op + channel), 0x1f);
        write_reg(chip, static_cast<uint8_t>(0x60 + op + channel), 0x00);
        write_reg(chip, static_cast<uint8_t>(0x70 + op + channel), 0x00);
        write_reg(chip, static_cast<uint8_t>(0x80 + op + channel), 0x0f);
    }
    write_reg(chip, static_cast<uint8_t>(0xa0 + channel), 0x80);
    write_reg(chip, static_cast<uint8_t>(0xa4 + channel), 0x22);
    write_reg(chip, static_cast<uint8_t>(0xb0 + channel), 0x07);
    write_reg(chip, static_cast<uint8_t>(0xb4 + channel), 0xc0);
    write_reg(chip, 0x28, static_cast<uint8_t>(0xf0 | channel));
}

static void test_models_and_output_streams()
{
    for (YM2610Model model : {YM2610_MODEL_YM2610, YM2610_MODEL_YM2610B}) {
        YM2610Device *chip = make_chip(model);
        assert(ym2610_sample_rate(chip) == 8000000 / 144);
        configure_ssg_tone(chip);
        configure_fm_tone(chip, 1);
        const std::vector<YM2610Sample> samples = run(chip, 144 * 4096);
        assert(samples.size() == (uint64_t(144) * 4096 * 55555) / 8000000);
        bool ssg_nonzero = false;
        bool fm_left_nonzero = false;
        bool fm_right_nonzero = false;
        bool ssg_varies = false;
        bool fm_varies = false;
        for (size_t i = 0; i < samples.size(); ++i) {
            ssg_nonzero |= samples[i].ssg != 0;
            fm_left_nonzero |= samples[i].fm_left != 0;
            fm_right_nonzero |= samples[i].fm_right != 0;
            if (i != 0) {
                ssg_varies |= samples[i].ssg != samples[i - 1].ssg;
                fm_varies |= samples[i].fm_left != samples[i - 1].fm_left ||
                             samples[i].fm_right != samples[i - 1].fm_right;
            }
        }
        assert(ssg_nonzero && ssg_varies);
        assert(fm_left_nonzero && fm_right_nonzero && fm_varies);
        ym2610_destroy(chip);
    }

    /* YM2610's literal upstream channel mask excludes channel 0; YM2610B's
     * mask includes it. An audible channel-0 tone distinguishes the models. */
    YM2610Device *a = make_chip(YM2610_MODEL_YM2610);
    YM2610Device *b = make_chip(YM2610_MODEL_YM2610B);
    configure_fm_tone(a, 0);
    configure_fm_tone(b, 0);
    const auto samples_a = run(a, 144 * 2048);
    const auto samples_b = run(b, 144 * 2048);
    bool a_nonzero = false;
    bool b_nonzero = false;
    for (const auto &sample : samples_a)
        a_nonzero |= sample.fm_left != 0 || sample.fm_right != 0;
    for (const auto &sample : samples_b)
        b_nonzero |= sample.fm_left != 0 || sample.fm_right != 0;
    assert(!a_nonzero && b_nonzero);
    ym2610_destroy(a);
    ym2610_destroy(b);
}

static void test_busy_timer_and_irq()
{
    Trace trace;
    YM2610Device *chip = make_chip(YM2610_MODEL_YM2610B, &trace);
    write_reg(chip, 0x24, 0xff);
    write_reg(chip, 0x25, 0x03); /* timer A period is one step */
    write_reg(chip, 0x27, 0x05); /* start and enable timer A */
    assert((ym2610_read(chip, 0) & 1) == 0);
    size_t count = 0;
    assert(ym2610_run_cycles(chip, 143, nullptr, 0, &count) == YM2610_OK);
    assert(count == 0 && (ym2610_read(chip, 0) & 1) == 0);
    assert(run(chip, 1).empty()); /* Integer stream rate: first frame at 145 clocks. */
    assert((ym2610_read(chip, 0) & 1) != 0);
    assert(trace.irq);
    assert(run(chip, 1).size() == 1);

    /* Timer A status reset clears IRQ. */
    write_reg(chip, 0x27, 0x15);
    assert((ym2610_read(chip, 0) & 1) == 0);
    assert(!trace.irq);

    /* Register writes hold BUSY for 32 prescaled clocks (192 cycles here). */
    write_reg(chip, 0x22, 0x08);
    assert((ym2610_read(chip, 0) & 0x80) != 0);
    (void)run(chip, 191);
    assert((ym2610_read(chip, 0) & 0x80) != 0);
    (void)run(chip, 1);
    assert((ym2610_read(chip, 0) & 0x80) == 0);
    ym2610_destroy(chip);
}

static void test_adpcm_rom_callbacks()
{
    Trace trace;
    YM2610Device *chip = make_chip(YM2610_MODEL_YM2610B, &trace);

    /* ADPCM-A channel 0: source address zero, pan both, then key on. */
    write_reg_hi(chip, 0x08, 0xc0);
    write_reg_hi(chip, 0x10, 0x00);
    write_reg_hi(chip, 0x18, 0x00);
    write_reg_hi(chip, 0x20, 0x20);
    write_reg_hi(chip, 0x28, 0x00);
    write_reg_hi(chip, 0x00, 0x01);

    /* ADPCM-B: external read, start at zero, execute through ROM. */
    write_reg(chip, 0x11, 0xc1); /* pan both, ROM addressing */
    write_reg(chip, 0x12, 0x00); /* start low */
    write_reg(chip, 0x13, 0x00); /* start high */
    write_reg(chip, 0x14, 0x20); /* end low (ROM units of 32 bytes) */
    write_reg(chip, 0x15, 0x00); /* end high */
    write_reg(chip, 0x19, 0xff); /* delta-N low */
    write_reg(chip, 0x1a, 0xff); /* delta-N high */
    write_reg(chip, 0x1b, 0xff); /* level */
    write_reg(chip, 0x10, 0x80); /* external mode, execute */

    (void)run(chip, 144 * 1000);
    assert(trace.adpcm_a_reads > 0);
    assert(trace.adpcm_b_reads > 0);
    assert(trace.last_address_a < 0x10000);
    assert(trace.last_address_b < 0x10000);
    ym2610_destroy(chip);

    /* A caller-owned region is borrowed and used as ROM. A constant synthetic
     * sample yields a nonzero ADPCM-A stream without inventing game media. */
    std::vector<uint8_t> rom(0x2200, 0x77);
    chip = make_chip(YM2610_MODEL_YM2610B, nullptr, rom.data(), rom.size());
    write_reg_hi(chip, 0x01, 0x3f);
    write_reg_hi(chip, 0x08, 0xdf);
    write_reg_hi(chip, 0x10, 0x00);
    write_reg_hi(chip, 0x18, 0x00);
    write_reg_hi(chip, 0x20, 0x20);
    write_reg_hi(chip, 0x28, 0x00);
    write_reg_hi(chip, 0x00, 0x01);
    const auto region_samples = run(chip, 144 * 512);
    bool region_played = false;
    for (const auto &sample : region_samples)
        region_played |= sample.fm_left != 0 || sample.fm_right != 0;
    assert(region_played);
    ym2610_destroy(chip);
}

static void test_slicing_reset_and_errors()
{
    YM2610Device *whole = make_chip(YM2610_MODEL_YM2610B);
    YM2610Device *sliced = make_chip(YM2610_MODEL_YM2610B);
    configure_ssg_tone(whole);
    configure_ssg_tone(sliced);
    const auto expected = run(whole, 144 * 257);
    std::vector<YM2610Sample> actual;
    for (int i = 0; i < 257; ++i) {
        auto part = run(sliced, 144);
        actual.insert(actual.end(), part.begin(), part.end());
    }
    assert(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        assert(actual[i].ssg == expected[i].ssg);
        assert(actual[i].fm_left == expected[i].fm_left);
        assert(actual[i].fm_right == expected[i].fm_right);
    }

    size_t count = 0;
    assert(ym2610_run_cycles(whole, 144, nullptr, 0, &count) == YM2610_CAPACITY);
    assert(ym2610_elapsed_cycles(whole) == 144 * 257);
    ym2610_reset(sliced);
    assert((ym2610_read(sliced, 0) & 0x80) == 0);
    ym2610_destroy(whole);
    ym2610_destroy(sliced);

    /* MAME allocates an integer-rate stream, not exactly clock/144 frames.
     * Independently verify one second and the fractional boundary at 8 MHz. */
    YM2610Device *timed = make_chip(YM2610_MODEL_YM2610B);
    assert(run(timed, 8000000).size() == 55555);
    assert(run(timed, 144).empty());
    assert(run(timed, 1).size() == 1);
    ym2610_destroy(timed);

    YM2610Config invalid{};
    invalid.model = 9;
    invalid.clock_hz = 8000000;
    YM2610Device *device = nullptr;
    assert(ym2610_create(&invalid, &device) == YM2610_BAD_MODEL);
    invalid.model = YM2610_MODEL_YM2610;
    invalid.clock_hz = 0;
    assert(ym2610_create(&invalid, &device) == YM2610_ARGUMENT);
}

int main()
{
    test_models_and_output_streams();
    test_busy_timer_and_irq();
    test_adpcm_rom_callbacks();
    test_slicing_reset_and_errors();
    std::puts("YM2610/YM2610B wrapper tests passed");
    return 0;
}
