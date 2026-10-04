/*
 * Capcom ZN adapter tests derived from MAME src/mame/sony/zn.cpp.
 * MAME source revision: deb2f54fe7300e7a192690318beb860159bfd52a
 * Copyright smf, R. Belmont (upstream does not specify years).
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "capcom_zn.h"

#include <cassert>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <tuple>

struct Fixture {
    uint32_t kick[2] = {0x89abcdefu, 0x76543210u};
    unsigned kick_reads[2] = {};
    uint16_t qsound_addresses[8] = {};
    uint8_t qsound_values[8] = {};
    unsigned qsound_writes = 0;
    unsigned qsound_reads = 0;
    uint8_t qsound_status = 0xc7;
};

static uint32_t read_kick(void *opaque, unsigned player)
{
    auto *fixture = static_cast<Fixture *>(opaque);
    assert(player < 2);
    fixture->kick_reads[player]++;
    return fixture->kick[player];
}

static int qsound_write(void *opaque, uint16_t address, uint8_t value)
{
    auto *fixture = static_cast<Fixture *>(opaque);
    assert(address >= 0xd000 && address <= 0xd002);
    assert(fixture->qsound_writes < 8);
    fixture->qsound_addresses[fixture->qsound_writes] = address;
    fixture->qsound_values[fixture->qsound_writes] = value;
    fixture->qsound_writes++;
    return 1;
}

static int qsound_read(void *opaque, uint8_t *value)
{
    auto *fixture = static_cast<Fixture *>(opaque);
    fixture->qsound_reads++;
    *value = fixture->qsound_status;
    return 1;
}

static std::vector<uint8_t> make_sound_rom()
{
    std::vector<uint8_t> rom(0x48000, 0xff);
    size_t pc = 0;
    const uint8_t program[] = {
        0x31, 0xfe, 0xff,             // LD SP,$fffe
        0x3e, 0x05, 0x32, 0x03, 0xd0, // LD A,5 / LD ($d003),A (bank 5)
        0x3a, 0x00, 0x80, 0x32, 0x01, 0xf0, // read bank byte -> RAM $f001
        0x3e, 0x12, 0x32, 0x00, 0xd0,
        0x3e, 0x34, 0x32, 0x01, 0xd0,
        0x3e, 0x56, 0x32, 0x02, 0xd0,
        0x3a, 0x07, 0xd0, 0x32, 0x03, 0xf0, // QSound status -> RAM $f003
        0xed, 0x56, 0xfb              // IM 1 / EI
    };
    std::memcpy(rom.data(), program, sizeof(program));
    pc = sizeof(program);
    rom[pc++] = 0x76; // HALT
    rom[pc++] = 0x18; // JR back to HALT after any interrupt
    rom[pc++] = 0xfd;

    // NMI reads and acknowledges the main-CPU sound latch on I/O port 00.
    rom[0x66] = 0xdb; rom[0x67] = 0x00; // IN A,($00)
    rom[0x68] = 0x32; rom[0x69] = 0x00; rom[0x6a] = 0xf0; // LD ($f000),A
    rom[0x6b] = 0xed; rom[0x6c] = 0x45; // RETN

    // IM 1 IRQ handler increments RAM $f004 and returns through RETI.
    rom[0x38] = 0x3a; rom[0x39] = 0x04; rom[0x3a] = 0xf0; // LD A,($f004)
    rom[0x3b] = 0x3c; // INC A
    rom[0x3c] = 0x32; rom[0x3d] = 0x04; rom[0x3e] = 0xf0; // LD ($f004),A
    rom[0x3f] = 0xfb; // EI
    rom[0x40] = 0xed; rom[0x41] = 0x4d; // RETI

    rom[0x8000 + 5 * 0x4000] = 0x5a;
    rom[0x8000 + 7 * 0x4000] = 0x7a;
    return rom;
}

static capcom_zn_config_t make_config(const std::vector<uint8_t> &main_rom,
        const std::vector<uint8_t> &country, const std::vector<uint8_t> &sound)
{
    capcom_zn_config_t config{};
    config.main_rom = main_rom.data(); config.main_rom_size = main_rom.size();
    config.country_rom = country.data(); config.country_rom_size = country.size();
    config.sound_rom = sound.data(); config.sound_rom_size = sound.size();
    return config;
}

static capcom_zn_callbacks_t make_callbacks(Fixture &fixture)
{
    capcom_zn_callbacks_t callbacks{};
    callbacks.opaque = &fixture;
    callbacks.read_kick = read_kick;
    callbacks.qsound_write = qsound_write;
    callbacks.qsound_read = qsound_read;
    return callbacks;
}

static void test_main_bus_mapping_and_lanes()
{
    std::vector<uint8_t> main_rom(0x2400000, 0xff);
    std::vector<uint8_t> country(0x80000, 0x44);
    auto sound = make_sound_rom();
    Fixture fixture;
    auto config = make_config(main_rom, country, sound);
    auto callbacks = make_callbacks(fixture);
    CapcomZn *board;
    uint32_t value = 0;

    main_rom[0] = 0x11; main_rom[1] = 0x22;
    main_rom[2] = 0x33; main_rom[3] = 0x44;
    main_rom[CAPCOM_ZN_MAIN_FIXED_SIZE] = 0xa0;
    main_rom[CAPCOM_ZN_MAIN_FIXED_SIZE + 1] = 0xa1;
    main_rom[CAPCOM_ZN_MAIN_FIXED_SIZE + 2] = 0xa2;
    main_rom[CAPCOM_ZN_MAIN_FIXED_SIZE + 3] = 0xa3;
    main_rom[CAPCOM_ZN_MAIN_FIXED_SIZE + 7 * CAPCOM_ZN_MAIN_BANK_SIZE] = 0x7a;
    country.back() = 0x55;
    board = capcom_zn_create(&config, &callbacks);
    assert(board);

    assert(capcom_zn_main_read(board, 0x1f000000, 4, &value) == CAPCOM_ZN_HANDLED);
    assert(value == 0x44332211u);
    assert(capcom_zn_main_read(board, 0x9f000000, 4, &value) == CAPCOM_ZN_HANDLED);
    assert(value == 0x44332211u); // KSEG0 alias
    assert(capcom_zn_main_read(board, 0xbf000001, 1, &value) == CAPCOM_ZN_HANDLED);
    assert(value == 0x22); // KSEG1 byte lane
    assert(capcom_zn_main_read(board, 0x1fbfffff, 1, &value) == CAPCOM_ZN_HANDLED);
    assert(value == 0x55);

    assert(capcom_zn_main_read(board, 0x1fb40010, 4, &value) == CAPCOM_ZN_HANDLED);
    assert(value == 0x89abcdefu);
    assert(capcom_zn_main_read(board, 0x1fb40012, 2, &value) == CAPCOM_ZN_HANDLED);
    assert(value == 0x89ab);
    assert(capcom_zn_main_read(board, 0x1fb40021, 1, &value) == CAPCOM_ZN_HANDLED);
    assert(value == 0x32);
    assert(fixture.kick_reads[0] == 2 && fixture.kick_reads[1] == 1);

    assert(capcom_zn_main_write(board, 0x1fb00000, 4, 0xa7) == CAPCOM_ZN_HANDLED);
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.main_bank == 7);
        assert(status.main_bank_offset == 0x2000000);
    }
    assert(capcom_zn_main_read(board, 0x1f400000, 4, &value) == CAPCOM_ZN_HANDLED);
    assert(value == 0xffffff7au);
    assert(capcom_zn_main_write(board, 0x1fb00001, 1, 3) == CAPCOM_ZN_UNMAPPED);
    assert(capcom_zn_main_read(board, 0x1fa00000, 4, &value) == CAPCOM_ZN_UNMAPPED);
    value = 0xdeadbeef;
    assert(capcom_zn_main_read(board, 0x1f000001, 4, &value) == CAPCOM_ZN_INVALID);
    assert(value == 0xdeadbeef);

    assert(capcom_zn_main_write(board, 0x1fb00000, 1, 0xff) == CAPCOM_ZN_HANDLED);
    value = 0xdeadbeef;
    assert(capcom_zn_main_read(board, 0x1f400000, 4, &value) == CAPCOM_ZN_FAULT);
    assert(value == 0xdeadbeef); // invalid bank pointer is not replaced with fill bytes
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.faults & CAPCOM_ZN_FAULT_ROM_BOUNDS);
    }

    capcom_zn_destroy(board);
}

static void test_real_sound_program_latch_qsound_and_periodic_irq()
{
    std::vector<uint8_t> main_rom(0x2400000, 0xff);
    std::vector<uint8_t> country(0x80000, 0xff);
    auto sound = make_sound_rom();
    Fixture fixture;
    auto config = make_config(main_rom, country, sound);
    auto callbacks = make_callbacks(fixture);
    CapcomZn *board = capcom_zn_create(&config, &callbacks);
    assert(board);

    assert(capcom_zn_run_sound_cycles(board, 2000) == CAPCOM_ZN_HANDLED);
    assert(capcom_zn_sound_read(board, 0xf001) == 0x5a);
    assert(capcom_zn_sound_read(board, 0xf003) == fixture.qsound_status);
    assert(fixture.qsound_writes == 3);
    assert(fixture.qsound_addresses[0] == 0xd000 && fixture.qsound_values[0] == 0x12);
    assert(fixture.qsound_addresses[1] == 0xd001 && fixture.qsound_values[1] == 0x34);
    assert(fixture.qsound_addresses[2] == 0xd002 && fixture.qsound_values[2] == 0x56);
    assert(fixture.qsound_reads == 1);
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.sound_bank == 5);
        assert(status.sound_bank_offset == 0x1c000);
    }

    assert(capcom_zn_main_write(board, 0x1fb60000, 4, 0x000000a5) == CAPCOM_ZN_HANDLED);
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.sound_latch == 0xa5 && status.sound_latch_pending);
    }
    assert(capcom_zn_run_sound_cycles(board, 128) == CAPCOM_ZN_HANDLED);
    assert(capcom_zn_sound_read(board, 0xf000) == 0xa5);
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.sound_latch_pending == 0);
    }

    // MAME's periodic execute timer is adjusted with the period as its first
    // expiry (interface_post_reset), so the first HOLD IRQ is at +32,000
    // sound T-states, not at reset time.
    assert(capcom_zn_run_sound_cycles(board, 32000 - 2128 - 1) == CAPCOM_ZN_HANDLED);
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.sound_cycles == 31999);
        assert(status.irq_ticks == 0 && !status.irq_asserted);
    }
    assert(capcom_zn_run_sound_cycles(board, 1) == CAPCOM_ZN_HANDLED);
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.irq_ticks == 1 && status.irq_asserted);
    }
    assert(capcom_zn_run_sound_cycles(board, 128) == CAPCOM_ZN_HANDLED);
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.irq_acknowledgements == 1 && !status.irq_asserted);
        assert(status.sound_cycles == 32000 + 128);
    }
    assert(capcom_zn_sound_read(board, 0xf004) == 1);

    capcom_zn_reset(board);
    assert(capcom_zn_sound_read(board, 0xf000) == 0xa5);
    assert(capcom_zn_sound_read(board, 0xf001) == 0x5a);
    assert(capcom_zn_sound_read(board, 0xf003) == fixture.qsound_status);
    assert(capcom_zn_sound_read(board, 0xf004) == 1);
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.sound_latch == 0xa5 && !status.sound_latch_pending);
        assert(!status.irq_asserted && status.irq_ticks == 0);
        assert(status.sound_cycles == 0 && status.irq_acknowledgements == 0);
    }
    capcom_zn_destroy(board);
}

static void test_required_callbacks_and_out_of_range_regions_fail_closed()
{
    std::vector<uint8_t> main_rom(CAPCOM_ZN_MAIN_FIXED_SIZE, 0);
    std::vector<uint8_t> country(1, 0x12);
    std::vector<uint8_t> sound(0x9000, 0);
    Fixture fixture;
    auto config = make_config(main_rom, country, sound);
    capcom_zn_callbacks_t callbacks{};
    callbacks.opaque = &fixture;
    CapcomZn *board = capcom_zn_create(&config, &callbacks);
    assert(board);

    uint32_t value = 0;
    assert(capcom_zn_main_read(board, 0x1fbfffff, 1, &value) == CAPCOM_ZN_FAULT);
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.faults & CAPCOM_ZN_FAULT_ROM_BOUNDS);
    }
    capcom_zn_reset(board);
    value = 0x12345678;
    assert(capcom_zn_main_read(board, 0x1fb40010, 4, &value) == CAPCOM_ZN_FAULT);
    capcom_zn_reset(board);
    capcom_zn_sound_write(board, 0xd003, 1);
    assert(capcom_zn_sound_read(board, 0x8000) == 0xff);
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.faults & CAPCOM_ZN_FAULT_ROM_BOUNDS);
    }
    capcom_zn_reset(board);
    assert(capcom_zn_sound_read(board, 0xd007) == 0xff);
    {
        capcom_zn_status_t status{};
        capcom_zn_get_status(board, &status);
        assert(status.faults & CAPCOM_ZN_FAULT_QSOUND_CALLBACK);
    }
    assert(capcom_zn_create(nullptr, &callbacks) == nullptr);
    {
        auto invalid = config;
        invalid.main_rom_size = CAPCOM_ZN_MAIN_FIXED_SIZE - 1;
        assert(capcom_zn_create(&invalid, &callbacks) == nullptr);
        invalid = config;
        invalid.sound_rom_size = 0x7fff;
        assert(capcom_zn_create(&invalid, &callbacks) == nullptr);
    }
    capcom_zn_destroy(board);
}

struct JointSoundFixture {
    std::array<uint16_t, 0x1000> coefficient_rom{};
    std::vector<uint8_t> samples{0x40, 0x80};
    std::vector<uint8_t> main_rom = std::vector<uint8_t>(CAPCOM_ZN_MAIN_FIXED_SIZE, 0);
    std::vector<uint8_t> country = std::vector<uint8_t>(CAPCOM_ZN_COUNTRY_SIZE, 0);
    std::vector<uint8_t> sound = std::vector<uint8_t>(0x48000, 0);
    std::vector<std::tuple<int16_t, int16_t, uint64_t>> pcm;
    Fixture external_callbacks;
    QSoundHLE *qsound = nullptr;
    CapcomZn *board = nullptr;
    uint64_t allowed_sound_end = 0;
    uint64_t qsound_cycle_origin = 0;
    unsigned pc = 0x100;

    static void audio(void *opaque, int16_t left, int16_t right, uint64_t frame)
    {
        auto *f = static_cast<JointSoundFixture *>(opaque);
        QSoundHLEStatus qs{};
        qsound_hle_get_status(f->qsound, &qs);
        /* Even when Z80 bus execution overshoots, HLE must never advance
         * past this scheduler budget's nominal endpoint. */
        assert(qs.cycles - f->qsound_cycle_origin <= f->allowed_sound_end * 15 / 4);
        f->pcm.emplace_back(left, right, frame);
    }
    void byte(uint8_t value) { sound[pc++] = value; }
    void store_a(uint16_t address) { byte(0x32); byte(address); byte(address >> 8); }
    void load_a(uint8_t value) { byte(0x3e); byte(value); }
    void poll_ready()
    {
        unsigned loop = pc;
        byte(0x3a); byte(0x07); byte(0xd0); // LD A,($d007)
        byte(0xe6); byte(0x80); // AND $80
        byte(0x28); byte(uint8_t(int(loop) - int(pc + 1))); // JR Z,loop
    }
    void command(uint8_t address, uint16_t value)
    {
        load_a(value >> 8); store_a(0xd000);
        load_a(value); store_a(0xd001);
        load_a(address); store_a(0xd002);
        poll_ready();
    }
    JointSoundFixture()
    {
        // Explicitly synthetic fixture coefficients. They do not substitute
        // for authentic dl-1425 media in a real board configuration.
        coefficient_rom[0x120] = uint16_t(-16384);
        coefficient_rom[0x120 + 196] = uint16_t(-8192);
        sound[0] = 0xc3; sound[1] = 0; sound[2] = 1; // JP $0100
        byte(0x31); byte(0xfe); byte(0xff); // LD SP,$fffe
        poll_ready();
        command(0xdf, 0); command(0xe1, 0); command(0xe2, 1);
        command(0x78, 0x8000); command(1, 0); command(2, 0);
        command(4, 2); command(5, 2); command(6, 0x4000);
        load_a(1); store_a(0xf020);
        unsigned halt = pc;
        byte(0x76); byte(0x18); byte(uint8_t(int(halt) - int(pc + 1)));

        // Actual NMI handler consumes port 0 and sends the latched byte as
        // the volume high byte, producing a second audible command.
        const uint8_t nmi[] = {
            0xf5, 0xdb, 0x00, 0x32, 0x10, 0xf0,
            0x32, 0x00, 0xd0, 0xaf, 0x32, 0x01, 0xd0,
            0x3e, 0x06, 0x32, 0x02, 0xd0, 0xf1, 0xed, 0x45
        };
        std::memcpy(sound.data() + 0x66, nmi, sizeof(nmi));
        QSoundHLEConfig qconfig{};
        qconfig.firmware = coefficient_rom.data(); qconfig.firmware_words = coefficient_rom.size();
        qconfig.samples = samples.data(); qconfig.sample_bytes = samples.size();
        qconfig.opaque = this; qconfig.pcm = audio;
        qsound = qsound_hle_create(&qconfig);
        assert(qsound);
        auto config = make_config(main_rom, country, sound);
        config.qsound_hle = qsound;
        auto callbacks = make_callbacks(external_callbacks);
        board = capcom_zn_create(&config, &callbacks);
        assert(board);
    }
    ~JointSoundFixture()
    {
        capcom_zn_destroy(board);
        qsound_hle_destroy(qsound);
    }
    capcom_zn_status_t status()
    {
        capcom_zn_status_t s{}; capcom_zn_get_status(board, &s); return s;
    }
    QSoundHLEStatus qstatus()
    {
        QSoundHLEStatus s{}; qsound_hle_get_status(qsound, &s); return s;
    }
    void run(uint64_t cycles)
    {
        allowed_sound_end = status().sound_cycles + cycles;
        assert(capcom_zn_run_sound_cycles(board, cycles) == CAPCOM_ZN_HANDLED);
        auto s = status();
        assert(s.qsound_sound_cycles == s.sound_cycles);
        assert(s.qsound_cycles == s.sound_cycles * 15 / 4);
        assert(s.qsound_cycle_fraction == s.sound_cycles * 15 % 4);
        assert(qstatus().cycles - qsound_cycle_origin == s.qsound_cycles);
    }
};

static void test_borrowed_hle_joint_execution_and_access_timing()
{
    JointSoundFixture whole, split;
    whole.run(20000);
    for (unsigned i = 0; i < 20000; ++i) split.run(1);
    assert(capcom_zn_sound_read(whole.board, 0xf020) == 1);
    assert(capcom_zn_sound_read(split.board, 0xf020) == 1);
    auto qw = whole.qstatus(), qs = split.qstatus();
    assert(qw.commands == 9 && qs.commands == 9);
    assert(qw.frames == 60 && qw.frames == qs.frames);
    assert(qw.output[0] == 16383 && qw.output[1] == 8192);
    assert(whole.pcm == split.pcm);
    assert(!whole.external_callbacks.qsound_reads && !whole.external_callbacks.qsound_writes);
    assert(!split.external_callbacks.qsound_reads && !split.external_callbacks.qsound_writes);
    uint16_t value = 0;
    assert(!qsound_hle_read_register(whole.qsound, 6, &value) && value == 0x4000);

    assert(capcom_zn_main_write(whole.board, 0x1fb60000, 1, 0xa5) == CAPCOM_ZN_HANDLED);
    assert(capcom_zn_main_write(split.board, 0x1fb60000, 1, 0xa5) == CAPCOM_ZN_HANDLED);
    whole.run(5000);
    unsigned remaining = 5000;
    while (remaining) {
        unsigned n = remaining < 17 ? remaining : 17;
        split.run(n); remaining -= n;
    }
    assert(whole.pcm == split.pcm);
    assert(capcom_zn_sound_read(whole.board, 0xf010) == 0xa5);
    assert(!whole.status().sound_latch_pending && !split.status().sound_latch_pending);
    assert(whole.qstatus().commands == 10 && split.qstatus().commands == 10);
    assert(!qsound_hle_read_register(whole.qsound, 6, &value) && value == 0xa500);
    assert(whole.qstatus().output[0] < 0 && split.qstatus().output[0] < 0);
    auto ws = whole.status(), ss = split.status();
    assert(ws.sound_cycles == ss.sound_cycles && ws.qsound_cycles == ss.qsound_cycles);
    assert(ws.z80_cycles == ss.z80_cycles && ws.z80_cycle_debt == ss.z80_cycle_debt);

    // Reset starts fresh board conversion/IRQ origins and resets the borrowed
    // device while retaining source-defined HLE stream timing/counters.
    whole.qsound_cycle_origin = whole.qstatus().cycles;
    capcom_zn_reset(whole.board);
    assert(!whole.status().sound_cycles && !whole.status().z80_cycles);
    assert(!whole.status().qsound_cycle_fraction && !whole.qstatus().ready);
    assert(!whole.qstatus().output[0] && !whole.qstatus().output[1]);
    whole.run(1);
    assert(whole.status().z80_cycles == 2 && whole.status().z80_cycle_debt == -1);
    assert(whole.status().qsound_cycles == 3 && whole.status().qsound_cycle_fraction == 3);
    uint64_t device_time = whole.qstatus().cycles;
    assert(capcom_zn_sound_read(whole.board, 0xd007) == 0);
    capcom_zn_sound_write(whole.board, 0xd000, 0x22);
    assert(whole.qstatus().cycles == device_time); // diagnostic accesses use nominal time, not CPU debt
    whole.run(1);
    assert(whole.status().z80_cycles == 2 && whole.status().z80_cycle_debt == 0);
    assert(whole.status().qsound_cycles == 7 && whole.status().qsound_cycle_fraction == 2);

    // Destroy owns only the board/Z80; borrowed QSound remains usable.
    capcom_zn_destroy(whole.board); whole.board = nullptr;
    QSoundHLEStatus surviving{};
    assert(!qsound_hle_get_status(whole.qsound, &surviving));
    assert(!qsound_hle_set_clock(whole.qsound, 120000000));
    auto bad_config = make_config(whole.main_rom, whole.country, whole.sound);
    bad_config.qsound_hle = whole.qsound;
    auto callbacks = make_callbacks(whole.external_callbacks);
    assert(!capcom_zn_create(&bad_config, &callbacks));
}

int main()
{
    test_main_bus_mapping_and_lanes();
    test_real_sound_program_latch_qsound_and_periodic_irq();
    test_required_callbacks_and_out_of_range_regions_fail_closed();
    test_borrowed_hle_joint_execution_and_access_timing();
    std::puts("Capcom ZN daughterboard tests passed");
    return 0;
}
