// SPDX-License-Identifier: BSD-3-Clause
// Synthetic firmware only. Expected host, sample-bus and DAC behavior from independent equations.
#include "qsound.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>
struct Fixture {
    std::array<uint16_t, 0x1000> firmware{};
    std::vector<uint8_t> samples;
    std::vector<std::tuple<int16_t, int16_t, uint64_t>> pcm;
    std::vector<std::tuple<unsigned, int16_t, uint64_t>> dac;
    QSound *q = nullptr;
    unsigned cursor = 0, diagnostics = 0;
    Fixture() : samples(2, 0) {}
    ~Fixture() { qsound_destroy(q); }
    void word(uint16_t op) { firmware[cursor++] = op; }
    void load(unsigned r, uint16_t value) {
        word(0x5000 | (r << 4));
        word(value);
    }
    void wait() {
        word(0x70ff);
        word(0x30c0);
    } // do 127: one NOP F1 instruction
    void loop() { word(cursor); }
    static void pcm_cb(void *p, int16_t l, int16_t r, uint64_t f) {
        static_cast<Fixture *>(p)->pcm.emplace_back(l, r, f);
    }
    static void dac_cb(void *p, unsigned ch, int16_t s, uint64_t c) {
        static_cast<Fixture *>(p)->dac.emplace_back(ch, s, c);
    }
    static void diag_cb(void *p, int, uint16_t, const char *) { ++static_cast<Fixture *>(p)->diagnostics; }
    void create(uint32_t clock = 0) {
        QSoundConfig c{};
        c.firmware = firmware.data();
        c.firmware_words = firmware.size();
        c.samples = samples.data();
        c.sample_bytes = samples.size();
        c.clock_hz = clock;
        c.opaque = this;
        c.pcm = pcm_cb;
        c.dac_write = dac_cb;
        c.diagnostic = diag_cb;
        q = qsound_create(&c);
        assert(q);
    }
    void run(uint64_t n) {
        assert(qsound_run_cycles(q, n) == n);
        assert(!status().halted);
    }
    QSoundStatus status() {
        QSoundStatus s{};
        assert(!qsound_get_status(q, &s));
        return s;
    }
    DSP16 *dsp() { return qsound_get_dsp(q); }
    void host(unsigned address, uint16_t value) {
        assert(!qsound_write(q, 0, value >> 8));
        assert(!qsound_write(q, 1, value));
        assert(!qsound_write(q, 2, address));
    }
    void command_firmware() {
        firmware[0] = 20;
        firmware[1] = 100;
        firmware[2] = 100;
        cursor = 20;
        load(0x1c, 0x1820); // Active PIO + external IRQ enable.
        word(0x30c0);
        word(22); // interruptible main loop
        cursor = 100;
        word(0x45d0); // first latent PDX0 read, returns stale value
        word(0x45d0); // second read consumes address, schedules data
        word(0x4800); // r0 = a0 high (address)
        word(0x45d0); // third read consumes data
        word(0x4900); // x = a0 high (data)
        word(0x6100); // RAM[r0] = x
        word(0xc100); // ireturn
    }
    void audio_firmware() {
        load(0x1c, 0x1800);
        load(0x18, 0x2e8); // Active PIO; serial OLD/OCK, MSB first CKI/12.
        load(0x1d, 0);
        load(8, 0x8001);
        word(0xc8c0); // offset 0, fetch sample into x
        word(0x4500);
        word(0x49a0); // a0=x, SDX=a0 (serial left sample)
        wait();
        load(0x1e, 1); // selects right channel and sample offset 1
        word(0x30c0); // allow the active PIO strobe to complete before reading samples
        word(0xc8c0);
        word(0x4500);
        word(0x49a0); // real ROM fetch -> serial right sample
        wait();
        loop();
    }
};
static void commands() {
    Fixture f;
    f.command_firmware();
    f.create();
    assert(qsound_read(f.q) == 0x80);
    f.run(10);
    f.host(0x42, 0x1234);
    assert(qsound_read(f.q) == 0);
    f.run(1);
    assert(qsound_read(f.q) == 0);
    for (int i = 0; i < 30 && qsound_read(f.q) == 0; ++i)
        f.run(1);
    assert(qsound_read(f.q) == 0x80);
    assert(!f.status().command_pending);
    // Source double buffers command data: changing the byte latch after address read doesn't corrupt it.
    qsound_write(f.q, 0, 0xfe);
    qsound_write(f.q, 1, 0xdc);
    f.run(40);
    assert(dsp16_ram_read(f.dsp(), 0x42) == 0x1234);
    assert(f.status().data_latch == 0xfedc);
    f.host(0x11, 0x5678);
    f.run(50);
    assert(dsp16_ram_read(f.dsp(), 0x11) == 0x5678 && qsound_read(f.q) == 0x80);
    // Host writes while busy are not rejected or queued by the source device: latest command wins.
    f.host(0x12, 0x8888);
    f.host(0x13, 0x9999);
    f.run(50);
    assert(dsp16_ram_read(f.dsp(), 0x12) == 0);
    assert(dsp16_ram_read(f.dsp(), 0x13) == 0x9999);
    assert(qsound_write(f.q, 0xd000, 1) == -1 && f.diagnostics == 1);
    f.host(3, 0xabcd);
    qsound_reset(f.q);
    assert(qsound_read(f.q) == 0x80 && !f.status().command_pending);
    assert(f.status().data_latch == 0xabcd);
    assert(dsp16_ram_read(f.dsp(), 0x42) == 0x1234);
    Fixture idle;
    idle.create();
    idle.host(1, 0x4321);
    idle.run(100000);
    assert(qsound_read(idle.q) == 0);
    qsound_reset(idle.q);
    assert(qsound_read(idle.q) == 0x80);
}
static void sample_latency() {
    Fixture f;
    f.samples.resize(0x40000);
    f.samples[5] = 0xa1;
    f.samples[0x20005] = 0xb2;
    f.samples[0x30005] = 0xc3;
    f.load(0x1c, 0x1800);
    f.load(0x1d, 5);
    f.load(8, 0x9002);
    f.word(0xc8c0);
    f.word(0x6101); // reads old bank 0, then selects 0x1002 for the next fetch
    f.word(0xc8c0);
    f.word(0x6101); // reads bank 2 after 24-bit aperture/mirror, then selects 0x1003
    f.load(0x1e, 5);
    f.word(0xc8c0);
    f.word(0x6101); // PDX1 upper bank bit survives but falls outside 24-bit address
    f.loop();
    f.create();
    f.run(80);
    assert(dsp16_ram_read(f.dsp(), 0) == 0xa100);
    assert(dsp16_ram_read(f.dsp(), 1) == 0xb200);
    assert(dsp16_ram_read(f.dsp(), 2) == 0xc300);
    auto s = f.status();
    assert(s.rom_bank == 0x9004 && s.rom_offset == 5 && s.sample_reads == 3);
    // Three-byte sample region mirrors a four-byte aperture; holes return the MAME default low value.
    Fixture odd;
    odd.samples = {0x11, 0x22, 0x33};
    odd.load(0x1c, 0x1800);
    odd.load(0x1d, 3);
    odd.load(8, 0x8000);
    odd.word(0xc8c0);
    odd.word(0x6100);
    odd.loop();
    odd.create();
    odd.run(40);
    assert(dsp16_ram_read(odd.dsp(), 0) == 0);
    Fixture bank;
    bank.samples.resize(0x2000000);
    bank.samples[0] = 0x44;
    bank.samples[0x1000000] = 0x99;
    bank.load(8, 0x8000);
    bank.word(0xc8c0);
    bank.word(0x6100);
    bank.loop();
    bank.create();
    assert(!qsound_set_sample_region_bank(bank.q, 3));
    bank.run(40);
    assert(dsp16_ram_read(bank.dsp(), 0) == 0x9900 && bank.status().sample_region_bank == 1);
}
static void audio_clock_slices() {
    Fixture a, b;
    a.samples = b.samples = {0xa3, 0x5c};
    a.audio_firmware();
    b.audio_firmware();
    a.create();
    b.create();
    a.run(3000);
    for (int i = 0; i < 3000; ++i)
        b.run(1);
    auto sa = a.status(), sb = b.status();
    assert(!std::memcmp(&sa, &sb, sizeof sa));
    assert(a.pcm == b.pcm && a.dac == b.dac);
    assert(a.dac.size() == 2);
    assert(std::get<0>(a.dac[0]) == 0 && std::get<1>(a.dac[0]) == int16_t(0xa300));
    assert(std::get<0>(a.dac[1]) == 1 && std::get<1>(a.dac[1]) == 0x5c00);
    assert(a.pcm.size() == 2 && std::get<0>(a.pcm[0]) == int16_t(0xa300) && std::get<1>(a.pcm[0]) == 0x5c00);
    assert(sa.clock_hz == 60000000 && sa.sample_rate == 24038 && !a.diagnostics);
    Fixture odd;
    odd.create();
    assert(qsound_run_clocks(odd.q, 1) == 0 && odd.status().crystal_remainder == 1);
    assert(qsound_run_clocks(odd.q, 3) == 2 && odd.status().crystal_remainder == 0);
    assert(qsound_set_clock(odd.q, 0) == -1);
    assert(!qsound_set_clock(odd.q, 120000000) && odd.status().sample_rate == 48076);
    Fixture second;
    second.create();
    second.run(30000000);
    assert(second.status().pcm_frames == 24038);
    assert(qsound_set_sample_region_bank(second.q, 0) == -1);
    QSoundConfig invalid{};
    assert(!qsound_create(&invalid));
}
int main() {
    commands();
    sample_latency();
    audio_clock_slices();
    puts("QSound LLE tests PASS (executed command/sample/audio firmware, timing and source vectors)");
}
