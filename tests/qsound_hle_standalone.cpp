// SPDX-License-Identifier: BSD-3-Clause
// Caller-supplied synthetic coefficient ROM, independent PCM/ADPCM/filter/echo expectations.
#include "qsound_hle.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>
struct Fixture {
    std::array<uint16_t, 0x1000> rom{};
    std::vector<uint8_t> samples{0x40, 0};
    std::vector<std::tuple<int16_t, int16_t, uint64_t>> pcm;
    QSoundHLE *q = nullptr;
    unsigned diagnostics = 0;
    Fixture() {
        rom[0x120] = uint16_t(-16384);
        rom[0x120 + 196] = uint16_t(-8192);
        for (unsigned i = 0; i < 16; ++i)
            rom[0x9dc + i] = 64;
    }
    ~Fixture() { qsound_hle_destroy(q); }
    static void audio(void *p, int16_t l, int16_t r, uint64_t f) {
        static_cast<Fixture *>(p)->pcm.emplace_back(l, r, f);
    }
    static void diagnostic(void *p, const char *) { ++static_cast<Fixture *>(p)->diagnostics; }
    void create(uint32_t clock = 0) {
        QSoundHLEConfig c{};
        c.firmware = rom.data();
        c.firmware_words = rom.size();
        c.samples = samples.data();
        c.sample_bytes = samples.size();
        c.clock_hz = clock;
        c.opaque = this;
        c.pcm = audio;
        c.diagnostic = diagnostic;
        q = qsound_hle_create(&c);
        assert(q);
    }
    void frames(uint64_t n) { qsound_hle_run_frames(q, n); }
    void host(unsigned addr, uint16_t data) {
        assert(!qsound_hle_write(q, 0, data >> 8));
        assert(!qsound_hle_write(q, 1, data));
        assert(!qsound_hle_write(q, 2, addr));
    }
    QSoundHLEStatus status() {
        QSoundHLEStatus s{};
        assert(!qsound_hle_get_status(q, &s));
        return s;
    }
    uint16_t reg(unsigned a) {
        uint16_t v;
        assert(!qsound_hle_read_register(q, a, &v));
        return v;
    }
    void initialize() {
        frames(5);
        assert(qsound_hle_read(q) == 0x80);
    }
    void zero_delays() {
        host(0xde, 0);
        host(0xdf, 0);
        host(0xe0, 0);
        host(0xe1, 0);
        host(0xe2, 1);
        frames(1);
    }
    void voice0(uint16_t rate = 0) {
        host(0x78, 0x8000);
        host(1, 0);
        host(2, rate);
        host(3, 0);
        host(4, 2);
        host(5, 2);
        host(6, 0x4000);
    }
};
static void boot_commands() {
    Fixture f;
    f.create();
    assert(qsound_hle_read(f.q) == 0);
    auto s = f.status();
    assert(s.state == 0 && s.frames == 0);
    f.frames(1);
    assert(f.status().state_counter == 1 && f.status().next_state == 0x39 && !qsound_hle_read(f.q));
    f.frames(1);
    assert(f.status().state_counter == 2 && !qsound_hle_read(f.q));
    f.frames(1);
    assert(f.status().state == 0x39 && !qsound_hle_read(f.q));
    f.frames(1);
    assert(f.status().state == 0x314 && f.status().filter_taps[0] == 95 && !qsound_hle_read(f.q));
    f.frames(1);
    assert(qsound_hle_read(f.q) == 0x80);
    f.host(0x78, 0x1234);
    assert(f.reg(0x78) == 0x1234 && qsound_hle_read(f.q) == 0);
    f.frames(1);
    assert(qsound_hle_read(f.q) == 0x80);
    // Bank register i applies to voice (i+1)%16, preserving source's hardware bank latency mapping.
    f.host(0, 0x4321);
    assert(f.reg(0) == 0x4321 && f.reg(0x78) == 0x1234);
    uint16_t value = 0;
    assert(qsound_hle_read_register(f.q, 7, &value) == -1);
    f.host(7, 0xabcd);
    assert(!qsound_hle_read(f.q));
    f.frames(1);
    assert(qsound_hle_read(f.q) == 0x80);
    assert(qsound_hle_write(f.q, 0xd000, 1) == -1 && f.diagnostics == 1);
    qsound_hle_reset(f.q);
    assert(!qsound_hle_read(f.q) && f.status().output[0] == 0 && f.status().data_latch == 0xabcd);
    assert(f.reg(0x78) == 0x1234);
    f.frames(1);
    assert(f.reg(0x78) == 0x8000);
    f.frames(4);
    assert(qsound_hle_read(f.q) == 0x80);
}
static void pcm_phase_and_bank() {
    Fixture f;
    f.create();
    f.initialize();
    f.zero_delays();
    f.voice0();
    f.frames(1);
    assert(f.status().voice_output[0] == 0x4000);
    assert(f.status().output[0] == 16383 && f.status().output[1] == 8192);
    // Full-volume source byte is zero-padded, with the exact DSP final rounding.
    f.host(2, 0x800);
    f.frames(1);
    assert(f.reg(1) == 0 && f.reg(3) == 0x8000);
    f.frames(1);
    assert(f.reg(1) == 1 && f.reg(3) == 0);
    f.frames(1);
    assert(f.status().voice_output[0] == 0);
    f.frames(1);
    assert(f.reg(1) == 0 && f.reg(3) == 0);
    f.frames(1);
    assert(f.status().voice_output[0] == 0x4000);
    // Signed addresses and fractional phases retain bounded source arithmetic.
    f.host(1, 0xffff);
    f.host(2, 0x1000);
    f.host(3, 0);
    f.host(5, 0x7fff);
    f.frames(1);
    assert(f.reg(1) == 0 && f.reg(3) == 0);
    Fixture bank;
    bank.samples.resize(0x20000);
    bank.samples[0] = 0x11;
    bank.samples[0x10000] = 0x66;
    bank.create();
    bank.initialize();
    bank.zero_delays();
    bank.voice0();
    bank.host(0x78, 0x8001);
    bank.frames(1);
    assert(bank.status().voice_output[0] == 0x6600);
    Fixture big;
    big.samples.resize(0x2000000);
    big.samples[0x1000000] = 0x55;
    big.create();
    big.initialize();
    big.zero_delays();
    big.voice0();
    assert(!qsound_hle_set_sample_region_bank(big.q, 3));
    big.frames(1);
    assert(big.status().voice_output[0] == 0x5500 && big.status().sample_region_bank == 1);
}
static void filter_modes() {
    Fixture wet;
    wet.rom[0x120] = 0;
    wet.rom[0x120 + 196] = 0;
    wet.rom[0x120 + 98] = uint16_t(-16384);
    wet.rom[0xd53 + 95 + 94] = uint16_t(-16384);
    wet.create();
    wet.initialize();
    wet.zero_delays();
    wet.voice0(0x1000);
    wet.frames(1);
    assert(wet.status().output[0] == 16383 && wet.status().output[1] == 0);
    wet.frames(1);
    assert(wet.status().output[0] == 0);
    wet.frames(1);
    assert(wet.status().output[0] == 16383);
    // Changing a filter address alone doesn't reload it: execute the source refresh state.
    wet.host(0xda, 0x800);
    wet.host(0xe3, 0x39);
    wet.frames(12);
    assert(wet.status().filter_table[0] == 0x800 && wet.status().output[0] == 0);
    Fixture mode;
    mode.rom[0x800 + 44] = uint16_t(-16384);
    mode.rom[0xa00 + 43] = uint16_t(-16384);
    mode.create();
    mode.initialize();
    mode.host(0xe3, 0x61a);
    mode.frames(12);
    assert(mode.status().state == 0x6b2 && mode.status().filter_taps[0] == 45 &&
           mode.status().alt_filter_taps[0] == 44);
    assert(mode.status().filter_table[0] == 0xf73 && mode.status().alt_filter_table[1] == 0xfa4);
    assert(mode.status().echo_end == 0x542);
    mode.host(0xda, 0x800);
    mode.host(0xdb, 0xa00);
    mode.host(0xdc, 0x900);
    mode.host(0xdd, 0xb00);
    mode.host(0xe3, 0x4f);
    mode.frames(12);
    mode.zero_delays();
    mode.voice0();
    mode.frames(1);
    assert(mode.status().output[0] == 16383 && mode.status().output[1] == 0);
}
static void echo_adpcm() {
    Fixture echo;
    echo.rom[0x120] = echo.rom[0x120 + 196] = 0;
    echo.create();
    echo.initialize();
    echo.zero_delays();
    echo.voice0();
    echo.host(0xba, 0x4000);
    echo.host(0xd9, 0x556);
    echo.frames(1);
    assert(echo.status().output[0] == 0);
    echo.frames(1);
    assert(echo.status().output[0] == 0);
    echo.frames(1);
    assert(echo.status().output[0] == 0); // previous delay pointer resets only after its next access
    echo.frames(1);
    assert(echo.status().output[0] == 8192);
    echo.frames(1);
    assert(echo.status().output[0] == 16383);
    echo.host(0xd9, 0xffff);
    echo.frames(1);
    assert(echo.status().echo_length == 0); // signed16 subtraction is source-specific
    Fixture adpcm;
    adpcm.samples = {0x17, 0};
    adpcm.create();
    adpcm.initialize();
    adpcm.host(0xca, 0);
    adpcm.host(0xcb, 1);
    adpcm.host(0xcc, 0x8000);
    adpcm.host(0xcd, 0x7fff);
    adpcm.host(0xd6, 1);
    bool top = false, bottom = false, muted = false;
    for (int i = 0; i < 18; ++i) {
        adpcm.frames(1);
        int output = adpcm.status().voice_output[16];
        if (!top && output == 7)
            top = true;
        else if (top && !bottom && output == 40)
            bottom = true;
        else if (bottom && output == 0)
            muted = true;
    }
    assert(top && bottom && muted && adpcm.reg(0xd6) == 0);
}
static int16_t rounded_output(int32_t sample) {
    int32_t result = ((sample * 16383 + 8192) & ~16383) >> 14;
    return int16_t(result > 32767 ? 32767 : result < -32767 ? -32767 : result);
}
static void waveform_vectors() {
    Fixture delayed;
    delayed.samples.resize(64);
    for (unsigned i = 0; i < 64; ++i)
        delayed.samples[i] = uint8_t(i * 43 + 17);
    delayed.create();
    delayed.initialize();
    delayed.zero_delays();
    delayed.host(0xdf, 3);
    delayed.host(0xe1, 5);
    delayed.host(0xe2, 1);
    delayed.frames(1);
    delayed.voice0(0x1400);
    delayed.host(4, 64);
    delayed.host(5, 64);
    delayed.host(6, 0x3000);
    std::array<int16_t, 51> left{}, right{};
    unsigned write = 3, lr = 0, rr = 49, phase = 0;
    for (unsigned frame = 0; frame < 512; ++frame) {
        unsigned address = phase >> 12;
        int32_t source = int8_t(delayed.samples[address]) * 256;
        int32_t voice = (source * 12288) >> 14;
        left[write] = voice;
        right[write] = voice / 2;
        write = (write + 1) % 51;
        int16_t expect_left = rounded_output(left[lr]), expect_right = rounded_output(right[rr]);
        lr = (lr + 1) % 51;
        rr = (rr + 1) % 51;
        delayed.frames(1);
        auto status = delayed.status();
        assert(status.output[0] == expect_left && status.output[1] == expect_right);
        phase = (phase + 0x1400) % (64 * 4096);
        assert(delayed.reg(1) == phase / 4096 && delayed.reg(3) == ((phase & 4095) << 4));
    }
    Fixture fir;
    fir.samples = {uint8_t(-31), 57, uint8_t(-99), 17, 84, uint8_t(-65), 23, 0};
    fir.rom[0x120] = fir.rom[0x120 + 196] = 0;
    fir.rom[0x120 + 98] = uint16_t(-16384);
    fir.rom[0xd53 + 95 + 92] = uint16_t(-8192);
    fir.rom[0xd53 + 95 + 93] = uint16_t(-4096);
    fir.rom[0xd53 + 95 + 94] = uint16_t(-16384);
    fir.create();
    fir.initialize();
    fir.zero_delays();
    fir.voice0(0x1000);
    fir.host(4, 8);
    fir.host(5, 8);
    fir.host(6, 0x1000);
    int32_t previous = 0, previous2 = 0;
    for (unsigned frame = 0; frame < 256; ++frame) {
        int32_t voice = int8_t(fir.samples[frame % 8]) * 64;
        int32_t filtered = voice + previous / 4 + previous2 / 2;
        fir.frames(1);
        assert(fir.status().output[0] == rounded_output(filtered) && fir.status().output[1] == 0);
        previous2 = previous;
        previous = voice;
    }
}
static void cycles_and_stress() {
    Fixture a, b;
    a.create();
    b.create();
    assert(qsound_hle_run_cycles(a.q, 10000) == 10000);
    for (int i = 0; i < 10000; ++i)
        assert(qsound_hle_run_cycles(b.q, 1) == 1);
    auto sa = a.status(), sb = b.status();
    assert(!std::memcmp(&sa, &sb, sizeof sa) && a.pcm == b.pcm);
    assert(sa.frames == 8 && sa.sample_rate == 24038);
    assert(qsound_hle_run_clocks(a.q, 1) == 0 && a.status().crystal_remainder == 1);
    assert(qsound_hle_run_clocks(a.q, 3) == 2 && a.status().crystal_remainder == 0);
    assert(qsound_hle_set_clock(a.q, 0) == -1);
    assert(!qsound_hle_set_clock(a.q, 120000000) && a.status().sample_rate == 48076);
    Fixture sec;
    sec.create();
    assert(qsound_hle_run_cycles(sec.q, 30000000) == 30000000);
    assert(sec.status().frames == 24038);
    // Extreme legal command values exercise explicit 32-bit wraparound and array bounds.
    Fixture stress;
    for (unsigned i = 0; i < stress.rom.size(); ++i)
        stress.rom[i] = uint16_t(i * 40503U);
    stress.samples = {0x80, 0x7f, 0x00, 0xff};
    stress.create();
    stress.initialize();
    for (unsigned voice = 0; voice < 16; ++voice) {
        stress.host(voice * 8 + 1, 0xffff);
        stress.host(voice * 8 + 2, 0xffff);
        stress.host(voice * 8 + 4, 0xffff);
        stress.host(voice * 8 + 5, 0x7fff);
        stress.host(voice * 8 + 6, 0x8000);
        stress.host(0xba + voice, 0x8000);
    }
    stress.host(0x93, 0x8000);
    stress.host(0xde, 0x8000);
    stress.host(0xdf, 0x7fff);
    stress.host(0xe0, 0x8000);
    stress.host(0xe1, 0x7fff);
    stress.host(0xe2, 1);
    stress.frames(3000);
    assert(stress.status().output[0] >= -32767);
    QSoundHLEConfig bad{};
    assert(!qsound_hle_create(&bad));
}
int main() {
    boot_commands();
    pcm_phase_and_bank();
    filter_modes();
    echo_adpcm();
    waveform_vectors();
    cycles_and_stress();
    puts("QSound HLE tests PASS (PCM/ADPCM/FIR/echo/state/ROM/timing/source vectors)");
}
