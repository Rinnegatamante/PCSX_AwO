#include "taito_fx1.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

struct Trace {
    unsigned sync_requests = 0;
    unsigned soft_resets = 0;
    std::vector<std::pair<int16_t, int16_t>> audio;
};

static void request_sync(void *opaque)
{
    ++static_cast<Trace *>(opaque)->sync_requests;
}

static void request_soft_reset(void *opaque)
{
    ++static_cast<Trace *>(opaque)->soft_resets; // Schedules; never resets inline.
}

static void audio_sample(void *opaque, int16_t left, int16_t right)
{
    static_cast<Trace *>(opaque)->audio.emplace_back(left, right);
}

struct Fixture {
    Trace trace;
    std::vector<uint8_t> banked = std::vector<uint8_t>(TAITO_FX1_MAIN_BANKED_ROM_SIZE, 0);
    std::vector<uint8_t> audio_rom = std::vector<uint8_t>(TAITO_FX1_AUDIO_ROM_SIZE, 0xff);
    std::vector<uint8_t> adpcma = std::vector<uint8_t>(0x200000, 0);
    TaitoFx1 *board = nullptr;

    explicit Fixture(const std::vector<uint8_t> &program = {})
    {
        std::fill(audio_rom.begin(), audio_rom.end(), 0x00);
        std::copy(program.begin(), program.end(), audio_rom.begin());
        taito_fx1_config_t config{};
        config.banked_rom = banked.data();
        config.banked_rom_size = banked.size();
        config.audio_rom = audio_rom.data();
        config.audio_rom_size = audio_rom.size();
        config.adpcm_a = adpcma.data();
        config.adpcm_a_size = adpcma.size();
        taito_fx1_callbacks_t callbacks{};
        callbacks.opaque = &trace;
        callbacks.request_sound_synchronization = request_sync;
        callbacks.request_soft_reset = request_soft_reset;
        callbacks.audio_sample = audio_sample;
        board = taito_fx1_create(&config, &callbacks);
        assert(board);
    }

    ~Fixture() { taito_fx1_destroy(board); }
};

static void emit(std::vector<uint8_t> &rom, std::initializer_list<uint8_t> bytes)
{
    rom.insert(rom.end(), bytes.begin(), bytes.end());
}

static std::vector<uint8_t> bank_test_program()
{
    std::vector<uint8_t> code;
    for (uint8_t bank = 0; bank < 8; ++bank) {
        emit(code, {0x3e, bank,             // LD A,bank
                    0x32, 0x00, 0xf2,      // LD (F200),A
                    0x3a, 0x00, 0x50,      // LD A,(5000)
                    0x32, (uint8_t)bank, 0xc0}); // LD (C000+bank),A
    }
    emit(code, {0x76});                    // HALT
    return code;
}

static std::vector<uint8_t> ssg_tone_program()
{
    std::vector<uint8_t> code;
    const uint8_t pairs[][2] = {{0x00, 0x01}, {0x01, 0x00},
                                {0x07, 0x3e}, {0x08, 0x0f},
                                {0x24, 0xff}, {0x25, 0x03}, {0x27, 0x05}};
    for (const auto &pair : pairs) {
        emit(code, {0x3e, pair[0], 0x32, 0x00, 0xe0,
                    0x3e, pair[1], 0x32, 0x01, 0xe0});
    }
    emit(code, {0x18, 0xfe});              // JR $ (timer/SSG keep running)
    return code;
}

static std::vector<uint8_t> comm_program()
{
    std::vector<uint8_t> rom(TAITO_FX1_AUDIO_ROM_SIZE, 0x00);
    rom[0] = 0xc3; rom[1] = 0x00; rom[2] = 0x01; // JP 0100
    /* NMI routine: receive the two command nibbles and return. */
    const uint8_t nmi[] = {
        0x3e, 0x00, 0x32, 0x00, 0xe2,       // mode 0
        0x3a, 0x01, 0xe2, 0x32, 0x00, 0xc1, // nibble 0 -> C100
        0x3e, 0x01, 0x32, 0x00, 0xe2,       // mode 1
        0x3a, 0x01, 0xe2, 0x32, 0x01, 0xc1, // nibble 1 -> C101
        0xed, 0x45                            // RETN
    };
    std::copy(std::begin(nmi), std::end(nmi), rom.begin() + 0x66);
    size_t p = 0x100;
    const uint8_t init[] = {
        0x31, 0xff, 0xcf,                    // LD SP,CFFF (mapped 8 KiB RAM)
        0x3e, 0x06, 0x32, 0x00, 0xe2,       // TC0140SYT enable NMI
        0x3e, 0x00, 0x32, 0x01, 0xe2,
        0x18, 0xfe                            // wait
    };
    std::copy(std::begin(init), std::end(init), rom.begin() + p);
    return rom;
}

static void test_main_bank_window_and_bounds()
{
    Fixture f;
    std::fill_n(f.banked.begin(), 4, 0x11);
    std::fill_n(f.banked.begin() + TAITO_FX1_BANK_WINDOW_SIZE, 4, 0x22);
    uint32_t value = 0;
    assert(taito_fx1_main_read(f.board, 0x1f000000, 4, &value) == TAITO_FX1_HANDLED);
    assert(value == 0x11111111);
    assert(taito_fx1_main_write(f.board, 0x1fb40000, 1, 1) == TAITO_FX1_HANDLED);
    assert(taito_fx1_main_read(f.board, 0x9f000000, 4, &value) == TAITO_FX1_HANDLED);
    assert(value == 0x22222222); // KSEG alias and bank 1.
    assert(taito_fx1_main_write(f.board, 0x1fb40000, 1, 2) == TAITO_FX1_HANDLED);
    assert(taito_fx1_main_read(f.board, 0x1f000000, 4, &value) == TAITO_FX1_FAULT);
    taito_fx1_status_t status{};
    taito_fx1_get_status(f.board, &status);
    assert(status.main_bank == 2);
    assert(status.faults & TAITO_FX1_FAULT_MAIN_BANK_BOUNDS);
    assert(taito_fx1_main_read(f.board, 0x1f000001, 2, &value) == TAITO_FX1_INVALID);
    assert(taito_fx1_main_read(f.board, 0x1f800000, 1, &value) == TAITO_FX1_UNMAPPED);
}

static void test_executed_z80_banks_and_sound_ram()
{
    auto code = bank_test_program();
    Fixture f(code);
    for (unsigned bank = 0; bank < 8; ++bank)
        f.audio_rom[bank * TAITO_FX1_AUDIO_BANK_SIZE + 0x1000] = uint8_t(0xa0 + bank);
    assert(taito_fx1_run_sound_cycles(f.board, 2000) == TAITO_FX1_HANDLED);
    for (unsigned bank = 0; bank < 8; ++bank)
        assert(taito_fx1_sound_read(f.board, uint16_t(0xc000 + bank)) == 0xa0 + bank);
    taito_fx1_status_t status{};
    taito_fx1_get_status(f.board, &status);
    assert(status.sound_bank == 7);
    assert(status.sound_bank_offset == 7 * TAITO_FX1_AUDIO_BANK_SIZE);
    assert(status.z80_instructions > 30);
    assert(status.faults == 0);
}

static void test_tc0140syt_master_packet_nmi_ack_and_reset_pin()
{
    Fixture f(comm_program());
    assert(taito_fx1_run_sound_cycles(f.board, 800) == TAITO_FX1_HANDLED);
    taito_fx1_status_t before{};
    taito_fx1_get_status(f.board, &before);
    assert(before.nmi_asserted == 0);

    assert(taito_fx1_main_write(f.board, 0x1fb80000, 1, 0) == TAITO_FX1_HANDLED);
    assert(taito_fx1_main_write(f.board, 0x1fb80002, 1, 0x0a) == TAITO_FX1_HANDLED);
    assert(taito_fx1_main_write(f.board, 0x1fb80000, 1, 1) == TAITO_FX1_HANDLED);
    assert(taito_fx1_main_write(f.board, 0x1fb80002, 1, 0x0b) == TAITO_FX1_HANDLED);
    taito_fx1_status_t queued{};
    taito_fx1_get_status(f.board, &queued);
    assert(queued.nmi_asserted == 1);
    assert(queued.synchronization_pending);
    assert(queued.synchronization_requests == 2);
    assert(f.trace.sync_requests == 2);
    taito_fx1_sound_synchronization_completed(f.board);
    assert(taito_fx1_run_sound_cycles(f.board, 1200) == TAITO_FX1_HANDLED);
    assert(taito_fx1_sound_read(f.board, 0xc100) == 0x0a);
    assert(taito_fx1_sound_read(f.board, 0xc101) == 0x0b);
    taito_fx1_get_status(f.board, &queued);
    assert((queued.tc0140_status & 0x01) == 0);
    assert(queued.nmi_asserted == 0);

    /* A nonzero mode-4 command asserts Z80 reset; it holds the CPU, while YM
     * clocks continue. The Z80 device reset happens when the line releases. */
    assert(taito_fx1_main_write(f.board, 0x1fb80000, 1, 4) == TAITO_FX1_HANDLED);
    assert(taito_fx1_main_write(f.board, 0x1fb80002, 1, 1) == TAITO_FX1_HANDLED);
    taito_fx1_status_t reset_status{};
    taito_fx1_get_status(f.board, &reset_status);
    assert(reset_status.sound_reset_asserted);
    uint64_t instructions = reset_status.z80_instructions;
    uint16_t held_pc = reset_status.z80_pc;
    uint64_t ym_cycles = reset_status.ym_cycles;
    assert(taito_fx1_run_sound_cycles(f.board, 40) == TAITO_FX1_HANDLED);
    taito_fx1_get_status(f.board, &reset_status);
    assert(reset_status.z80_instructions == instructions);
    assert(reset_status.z80_pc == held_pc);
    assert(reset_status.ym_cycles == ym_cycles + 80);

    assert(taito_fx1_main_write(f.board, 0x1fb80000, 1, 4) == TAITO_FX1_HANDLED);
    assert(taito_fx1_main_write(f.board, 0x1fb80002, 1, 0) == TAITO_FX1_HANDLED);
    taito_fx1_get_status(f.board, &reset_status);
    assert(!reset_status.sound_reset_asserted && reset_status.z80_pc == 0);
    assert(taito_fx1_run_sound_cycles(f.board, 80) == TAITO_FX1_HANDLED);
    taito_fx1_get_status(f.board, &reset_status);
    assert(!reset_status.sound_reset_asserted);
    assert(reset_status.z80_instructions > instructions);
}

static void reset_sequence(Fixture &f, bool split)
{
    assert(taito_fx1_run_sound_cycles(f.board, 1000) == TAITO_FX1_HANDLED);
    assert(taito_fx1_main_write(f.board, 0x1fb80000, 1, 4) == TAITO_FX1_HANDLED);
    assert(taito_fx1_main_write(f.board, 0x1fb80002, 1, 1) == TAITO_FX1_HANDLED);
    if (split) {
        for (unsigned cycles : {19u, 1024u, 8957u, 10000u})
            assert(taito_fx1_run_sound_cycles(f.board, cycles) == TAITO_FX1_HANDLED);
    } else {
        assert(taito_fx1_run_sound_cycles(f.board, 20000) == TAITO_FX1_HANDLED);
    }
    taito_fx1_status_t held{};
    taito_fx1_get_status(f.board, &held);
    assert(held.sound_reset_asserted && held.ym_cycles == held.sound_cycles * 2);
    assert(taito_fx1_main_write(f.board, 0x1fb80000, 1, 4) == TAITO_FX1_HANDLED);
    assert(taito_fx1_main_write(f.board, 0x1fb80002, 1, 0) == TAITO_FX1_HANDLED);
    if (split) {
        for (unsigned cycles : {1u, 17u, 507u, 2475u})
            assert(taito_fx1_run_sound_cycles(f.board, cycles) == TAITO_FX1_HANDLED);
    } else {
        assert(taito_fx1_run_sound_cycles(f.board, 3000) == TAITO_FX1_HANDLED);
    }
}

static void test_ym_bus_timestamps_across_reset_hold_and_splits()
{
    Fixture whole(ssg_tone_program()), split(ssg_tone_program());
    reset_sequence(whole, false);
    reset_sequence(split, true);
    taito_fx1_status_t a{}, b{};
    taito_fx1_get_status(whole.board, &a);
    taito_fx1_get_status(split.board, &b);
    assert(a.faults == 0 && b.faults == 0);
    assert(a.sound_cycles == b.sound_cycles && a.ym_cycles == b.ym_cycles);
    assert(a.ym_bus_events == b.ym_bus_events);
    assert(a.last_ym_bus_sound_time == b.last_ym_bus_sound_time);
    assert(a.last_ym_bus_sound_time >= 21000); // CPU YM writes after release.
    assert(whole.trace.audio == split.trace.audio);
}

static void test_ym2610b_tone_irq_mix_and_split_determinism()
{
    auto code = ssg_tone_program();
    Fixture whole(code), split(code);
    assert(taito_fx1_run_sound_cycles(whole.board, 24000) == TAITO_FX1_HANDLED);
    for (unsigned slice : {17u, 301u, 1u, 4096u, 8192u, 11393u})
        assert(taito_fx1_run_sound_cycles(split.board, slice) == TAITO_FX1_HANDLED);
    assert(whole.trace.audio.size() > 100);
    assert(whole.trace.audio == split.trace.audio);
    assert(std::any_of(whole.trace.audio.begin(), whole.trace.audio.end(),
                       [](auto sample) { return sample.first || sample.second; }));
    taito_fx1_status_t a{}, b{};
    taito_fx1_get_status(whole.board, &a);
    taito_fx1_get_status(split.board, &b);
    assert(a.sound_cycles == b.sound_cycles && a.ym_cycles == b.ym_cycles);
    assert(a.audio_samples == b.audio_samples);
    assert(a.ym_irq_asserted && b.ym_irq_asserted); // Executed 8 MHz YM timer A setup.
}

static void test_mb3773_falling_edge_kick_and_deferred_expiry()
{
    Fixture f;
    assert(taito_fx1_advance_to_us(f.board, 4900000) == TAITO_FX1_HANDLED);
    assert(taito_fx1_main_write(f.board, 0x1fb40000, 1, 0x20) == TAITO_FX1_HANDLED);
    assert(taito_fx1_advance_to_us(f.board, 4999999) == TAITO_FX1_HANDLED);
    assert(f.trace.soft_resets == 0);
    assert(taito_fx1_main_write(f.board, 0x1fb40000, 1, 0x00) == TAITO_FX1_HANDLED);
    assert(taito_fx1_advance_to_us(f.board, 9999998) == TAITO_FX1_HANDLED);
    assert(f.trace.soft_resets == 0);
    assert(taito_fx1_advance_to_us(f.board, 9999999) == TAITO_FX1_HANDLED);
    assert(f.trace.soft_resets == 1);
    taito_fx1_status_t status{};
    taito_fx1_get_status(f.board, &status);
    assert(status.watchdog_reset_requests == 1);
    assert(status.watchdog_time_us == 9999999);
    assert(status.soft_reset_pending == 0); // Host callback was asked to schedule it.
}

static void test_nop_maps_and_required_callbacks()
{
    Fixture f;
    assert(taito_fx1_sound_read(f.board, 0xe200) == 0);
    assert(taito_fx1_sound_read(f.board, 0xee00) == 0);
    taito_fx1_sound_write(f.board, 0xe400, 0x55);
    taito_fx1_sound_write(f.board, 0xee00, 0x55);
    taito_fx1_sound_write(f.board, 0xf000, 0x55);
    assert(taito_fx1_sound_read(f.board, 0x9000) == 0xff);

    taito_fx1_config_t config{};
    config.banked_rom = f.banked.data(); config.banked_rom_size = f.banked.size();
    config.audio_rom = f.audio_rom.data(); config.audio_rom_size = f.audio_rom.size();
    config.adpcm_a = f.adpcma.data(); config.adpcm_a_size = f.adpcma.size();
    taito_fx1_callbacks_t callbacks{};
    callbacks.opaque = &f.trace;
    callbacks.request_soft_reset = request_soft_reset;
    assert(taito_fx1_create(&config, &callbacks) == nullptr); // Scheduler barrier is mandatory.
    config.audio_rom_size--;
    callbacks.request_sound_synchronization = request_sync;
    assert(taito_fx1_create(&config, &callbacks) == nullptr); // Exact source ROM geometry.
}

int main()
{
    test_main_bank_window_and_bounds();
    test_executed_z80_banks_and_sound_ram();
    test_tc0140syt_master_packet_nmi_ack_and_reset_pin();
    test_ym_bus_timestamps_across_reset_hold_and_splits();
    test_ym2610b_tone_irq_mix_and_split_determinism();
    test_mb3773_falling_edge_kick_and_deferred_expiry();
    test_nop_maps_and_required_callbacks();
    std::puts("Taito FX-1S board adapter tests passed");
    return 0;
}
