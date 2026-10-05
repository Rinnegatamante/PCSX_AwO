#include "gnet_zoom.h"
#include "nor_flash.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
void emit(std::vector<uint8_t> &rom, size_t &p,
          std::initializer_list<uint8_t> bytes)
{
    for (uint8_t b : bytes) rom[p++] = b;
}

struct Media {
    std::vector<uint8_t> firm = std::vector<uint8_t>(GNET_FIRM_FLASH_BYTES, 0xff);
    std::vector<uint8_t> program = std::vector<uint8_t>(GNET_ZOOM_PROGRAM_FLASH_BYTES, 0xff);
    std::array<std::vector<uint8_t>, 3> samples{
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0xff),
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0xff),
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0xff)};
    std::vector<uint8_t> eprom = std::vector<uint8_t>(GNET_EPROM_BYTES, 0xff);

    gnet_zoom_config_t config(unsigned jp1 = 0)
    {
        gnet_zoom_config_t c{};
        c.firm_flash = firm.data(); c.firm_flash_size = firm.size();
        c.zoom_program_flash = program.data(); c.zoom_program_flash_size = program.size();
        for (unsigned i = 0; i < 3; ++i) {
            c.sample_flash[i] = samples[i].data();
            c.sample_flash_size[i] = samples[i].size();
        }
        c.eprom = eprom.data(); c.eprom_size = eprom.size(); c.jp1 = jp1;
        return c;
    }
};

void write_intel_word(gnet_zoom_board *b, uint32_t address, uint16_t data)
{
    assert(gnet_zoom_main_write16(b, address, data, 0xffff) == GNET_ZOOM_OK);
}
uint16_t read_word(gnet_zoom_board *b, uint32_t address)
{
    uint16_t result = 0;
    int status = gnet_zoom_main_read16(b, address, &result);
    if (status != GNET_ZOOM_OK) std::fprintf(stderr, "read %08x status %d bank %u\n", address, status, gnet_zoom_selected_bank(b));
    assert(status == GNET_ZOOM_OK);
    return result;
}

struct DestroyProbe {
    gnet_zoom_board *board = nullptr;
    unsigned callbacks = 0;
    int result = 0;
};

void probe_destroy_from_audio(void *opaque, float, float, uint64_t)
{
    auto *probe = static_cast<DestroyProbe *>(opaque);
    ++probe->callbacks;
    probe->result = gnet_zoom_try_destroy(probe->board);
}

static gnet_zoom_board *create_loop_board(Media &media,
                                          void *audio_opaque = nullptr,
                                          zoom_audio_frame_fn audio_frame = nullptr)
{
    media.program[0] = 0xea; // BRA -2: keep fetching/executing from NOR
    media.program[1] = 0xfe;
    auto config = media.config();
    if (audio_frame) {
        config.audio_opaque = audio_opaque;
        config.audio_frame = audio_frame;
    }
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK && board);
    assert(gnet_zoom_control_write(board, 0) == GNET_ZOOM_OK);
    return board;
}

struct ReadyProbe {
    gnet_zoom_board *board = nullptr;
    bool ready_seen = false;
    uint64_t first_ready_zsg_cycle = 0;
    unsigned frames = 0;
};

struct SoftResetProbe {
    gnet_zoom_board *board = nullptr;
    unsigned requests = 0;
    int destroy_result = GNET_ZOOM_OK;
};

void probe_soft_reset(void *opaque)
{
    auto *probe = static_cast<SoftResetProbe *>(opaque);
    ++probe->requests;
    probe->destroy_result = gnet_zoom_try_destroy(probe->board);
}

void probe_flash_ready_from_audio(void *opaque, float, float, uint64_t cycle)
{
    auto *probe = static_cast<ReadyProbe *>(opaque);
    ++probe->frames;
    if (!probe->ready_seen && gnet_zoom_flash_ready(probe->board, 0)) {
        probe->ready_seen = true;
        probe->first_ready_zsg_cycle = cycle;
    }
}

void test_mn_executes_real_zoom_flash_and_bus_is_little_endian()
{
    Media media;
    size_t p = 0;
    /* The MN10200 resets at 0x80000. This fixture runs from the actual
     * borrowed E28F400B image through the Zoom flash-read callback. */
    emit(media.program, p, {0xf8, 0x34, 0x12}); // D0 = 0x1234
    emit(media.program, p, {0xf4, 0x40, 0x10, 0x00, 0x40}); // sound RAM store
    emit(media.program, p, {0xf4, 0x44, 0x00, 0x00, 0x08}); // ignored ROM write
    emit(media.program, p, {0xf8, 0x78, 0x56}); // proves execution continued
    media.firm[0] = 0x34; media.firm[1] = 0x12;
    media.program[0x200] = 0xcd; media.program[0x201] = 0xab;

    gnet_zoom_config_t config = media.config();
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK && board);
    assert(gnet_zoom_control_read(board) == 0x10);
    assert(gnet_zoom_sound_pc(board) == 0x80000);
    assert(gnet_zoom_control_write(board, 0) == GNET_ZOOM_OK);
    assert(gnet_zoom_run(board, 4000) >= 4000);
    assert(gnet_zoom_sound_pc(board) > 0x80000);
    assert(gnet_zoom_sound_d(board, 0) == 0x5678);
    assert(gnet_zoom_sound_ram_read(board, 0x10) == 0x34);
    assert(gnet_zoom_sound_ram_read(board, 0x11) == 0x12);
    assert(media.program[0] == 0xf8); // MN map is read-only in taito_zm.cpp
    assert(gnet_zoom_fault(board) == 0);

    /* Bank 0 maps the main CPU to firmware at offset 0 and Zoom program at
     * 0x300000. Raw images are little-endian despite the x16 NOR engine. */
    assert(read_word(board, 0x1f000000) == 0x1234);
    assert(read_word(board, 0x1f300200) == 0xabcd);

    /* Intel x16 program sequence through the PSX banked window updates the
     * original little-endian ROM/NVRAM buffer. */
    write_intel_word(board, 0x1f300200, 0x0040);
    write_intel_word(board, 0x1f300200, 0x1234);
    write_intel_word(board, 0x1f300200, 0x00ff); // return to array-read mode
    assert(media.program[0x200] == 0x34 && media.program[0x201] == 0x12);
    assert(read_word(board, 0x1f300200) == 0x1234);
    assert(gnet_zoom_flash_ready(board, 1) == 1);
    gnet_zoom_destroy(board);
}

void test_bank_control_mapping_eprom_alias_and_wave_flash()
{
    Media media;
    media.eprom[0] = 0x9a; media.eprom[1] = 0xbc;
    media.samples[0][0] = 0x11; media.samples[0][1] = 0x22;
    media.samples[1][0] = 0x33; media.samples[1][1] = 0x44;
    media.samples[2][0] = 0x55; media.samples[2][1] = 0x66;
    auto config = media.config(1); // JP1 selects initial bank 2
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK);
    assert(gnet_zoom_selected_bank(board) == 2);
    assert(read_word(board, 0x1f000000) == 0xbc9a); // bank2 EPROM base
    assert(gnet_zoom_main_write16(board, 0x1f000000, 0, 0xffff) == GNET_ZOOM_READ_ONLY);
    uint16_t unmapped = 0;
    assert(gnet_zoom_main_read16(board, 0x1f800000, &unmapped) == GNET_ZOOM_UNMAPPED);
    /* Bank 2 exposes firmware at 0x10200000; confirm the alternate mapping
     * reaches the same TE28F160 command engine and raw image. */
    write_intel_word(board, 0x1f200200, 0x0040);
    write_intel_word(board, 0x1f200200, 0x5678);
    write_intel_word(board, 0x1f200200, 0x00ff);
    assert(media.firm[0x200] == 0x78 && media.firm[0x201] == 0x56);
    assert(read_word(board, 0x1f200200) == 0x5678);

    /* Control bit 2 selects the other bank bit; JP1 remains the high bank bit. */
    assert(gnet_zoom_control_write(board, 0x14) == GNET_ZOOM_OK);
    assert(gnet_zoom_selected_bank(board) == 3);
    assert(read_word(board, 0x1f000000) == 0x2211);
    assert(read_word(board, 0x1f200000) == 0x4433);
    assert(read_word(board, 0x1f400000) == 0x6655);
    /* MAME maps bank 3 through mirror(0x10000000) to the same wave devices. */
    uint16_t wave_hole = 0;
    assert(gnet_zoom_main_read16(board, 0x1f600000, &wave_hole) == GNET_ZOOM_UNMAPPED);
    assert(gnet_zoom_main_write16(board, 0x1f000000, 0x0040, 0xffff) == GNET_ZOOM_OK);
    assert(gnet_zoom_main_write16(board, 0x1f000000, 0xbeef, 0xffff) == GNET_ZOOM_OK);
    assert(media.samples[0][0] == 0xef && media.samples[0][1] == 0xbe);

    /* Reset release invokes the Zoom device reset, including NOR 0xff reset
     * commands, without erasing program data. */
    assert(gnet_zoom_control_write(board, 0x04) == GNET_ZOOM_OK);
    assert(read_word(board, 0x1f000000) == 0xbeef);
    assert(gnet_zoom_fault(board) == 0);
    gnet_zoom_destroy(board);
}

void test_source_maincpu_map_routes_registers_and_shared_ram_lanes()
{
    Media media;
    media.firm[0] = 0x12;
    media.firm[1] = 0x34;
    auto config = media.config();
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK);

    uint32_t value = 0;
    assert(gnet_zoom_bus_read(board, 0x1fa30000, 1, &value) == GNET_ZOOM_OK && value == 0);
    assert(gnet_zoom_bus_write(board, 0x1fa30000, 1, 0xa5) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_read(board, 0x1fa30000, 1, &value) == GNET_ZOOM_OK && value == 0xa5);
    assert(gnet_zoom_bus_read(board, 0x1f000000, 2, &value) == GNET_ZOOM_OK && value == 0x3412);

    /* Reset starts asserted with bank 0. The control register is byte-wide;
     * bit 2 selects bank 1 and bit 4 releases/reset-pulses the Zoom side. */
    assert(gnet_zoom_bus_read(board, 0x1fb40000, 1, &value) == GNET_ZOOM_OK && value == 0x10);
    assert(gnet_zoom_bus_write(board, 0x1fb40000, 1, 0x04) == GNET_ZOOM_OK);
    assert(gnet_zoom_selected_bank(board) == 1);
    assert(gnet_zoom_bus_read(board, 0x1fb70000, 2, &value) == GNET_ZOOM_OK && value == 2);
    assert(gnet_zoom_bus_write(board, 0x1fb70000, 2, 0xbeef) == GNET_ZOOM_OK);

    /* The 16-bit Zoom data/address latches and sound IRQ strobe are the exact
     * mapped devices used by taitogn_state::maincpu_program_map. */
    assert(gnet_zoom_bus_write(board, 0x1fb80002, 2, 4) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_write(board, 0x1fb80000, 2, 0x0020) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_write(board, 0x1fba0000, 2, 0) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_read(board, 0x1fbc0000, 2, &value) == GNET_ZOOM_OK && value == 0);

    /* MAME's .umask32(0x00ff00ff) exposes bytes at lanes 0 and 2 only.
     * Byte/halfword/word accesses preserve the two open-bus lanes. */
    assert(gnet_zoom_bus_write(board, 0x1fbe0000, 4, 0x00340012) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_read(board, 0x1fbe0000, 4, &value) == GNET_ZOOM_OK &&
           value == 0xff34ff12);
    assert(gnet_zoom_bus_read(board, 0x1fbe0001, 1, &value) == GNET_ZOOM_OK && value == 0xff);
    assert(gnet_zoom_bus_read(board, 0x1fbe0002, 1, &value) == GNET_ZOOM_OK && value == 0x34);
    assert(gnet_zoom_bus_write(board, 0x1fbe0004, 1, 0x56) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_read(board, 0x1fbe0004, 4, &value) == GNET_ZOOM_OK &&
           value == 0xff00ff56);

    assert(gnet_zoom_bus_read(board, 0x1fb80000, 2, &value) == GNET_ZOOM_UNMAPPED);
    assert(gnet_zoom_bus_read(board, 0x1fb00000, 1, &value) == GNET_ZOOM_UNSUPPORTED);
    assert(gnet_zoom_bus_write(board, 0x1fbe0000, 3, 0) == GNET_ZOOM_ARGUMENT);
    assert(gnet_zoom_bus_read(board, 0x1fa30000, 2, &value) == GNET_ZOOM_UNSUPPORTED);
    assert(gnet_zoom_bus_read(board, 0x1fb90000, 1, &value) == GNET_ZOOM_UNMAPPED);
    gnet_zoom_destroy(board);
}

void test_nor_busy_time_and_bad_config()
{
    Media media;
    media.firm[0x100] = 0x00;
    auto config = media.config();
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK);
    assert(gnet_zoom_main_write16(board, 0x1f000000, 0x20, 0xffff) == GNET_ZOOM_OK);
    assert(gnet_zoom_main_write16(board, 0x1f000000, 0xd0, 0xffff) == GNET_ZOOM_OK);
    assert(gnet_zoom_flash_ready(board, 0) == 0);
    assert(gnet_zoom_sync_to_cycle(board, 6250000ull) == GNET_ZOOM_OK);
    assert(gnet_zoom_flash_ready(board, 0) == 1);
    assert(media.firm[0] == 0xff && media.firm[1] == 0xff && media.firm[0x100] == 0xff);
    assert(gnet_zoom_sync_to_cycle(board, 1000) == GNET_ZOOM_TIME);
    gnet_zoom_destroy(board);

    config.firm_flash_size--;
    board = reinterpret_cast<gnet_zoom_board *>(uintptr_t(0x1));
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_ARGUMENT);
    assert(board == nullptr);
    board = reinterpret_cast<gnet_zoom_board *>(uintptr_t(0x1));
    assert(gnet_zoom_create(nullptr, &board) == GNET_ZOOM_ARGUMENT);
    assert(board == nullptr);
}

void test_cpu_fetch_advances_nor_at_bus_cycle_independent_of_quantum()
{
    Media one_quantum_media;
    ReadyProbe one_probe;
    gnet_zoom_board *one = create_loop_board(one_quantum_media, &one_probe,
                                              probe_flash_ready_from_audio);
    one_probe.board = one;
    write_intel_word(one, 0x1f000000, 0x0020);
    write_intel_word(one, 0x1f000000, 0x00d0);
    assert(gnet_zoom_flash_ready(one, 0) == 0);
    assert(gnet_zoom_sync_to_cycle(one, 6250000ull - 20) == GNET_ZOOM_OK);
    assert(gnet_zoom_flash_ready(one, 0) == 0);
    assert(read_word(one, 0x1f000000) == 0x0000); // busy status at predeadline
    assert(gnet_zoom_sync_to_cycle(one, 6250000ull + 20) == GNET_ZOOM_OK);
    assert(gnet_zoom_flash_ready(one, 0) == 1);
    assert(read_word(one, 0x1f000000) == 0x0080);
    assert(one_probe.frames > 0 && one_probe.ready_seen);
    assert(one_probe.first_ready_zsg_cycle >= 25000000ull);
    assert(one_probe.first_ready_zsg_cycle <= 25000000ull + 769);
    assert(gnet_zoom_try_destroy(one) == GNET_ZOOM_OK);

    Media split_media;
    ReadyProbe split_probe;
    gnet_zoom_board *split = create_loop_board(split_media, &split_probe,
                                                probe_flash_ready_from_audio);
    split_probe.board = split;
    write_intel_word(split, 0x1f000000, 0x0020);
    write_intel_word(split, 0x1f000000, 0x00d0);
    for (uint64_t cycle = 312500; cycle < 6250000; cycle += 312500) {
        assert(gnet_zoom_sync_to_cycle(split, cycle) == GNET_ZOOM_OK);
        assert(gnet_zoom_flash_ready(split, 0) == 0);
    }
    assert(gnet_zoom_sync_to_cycle(split, 6250000ull + 20) == GNET_ZOOM_OK);
    assert(gnet_zoom_flash_ready(split, 0) == 1);
    assert(read_word(split, 0x1f000000) == 0x0080);
    assert(split_probe.frames > 0 && split_probe.ready_seen);
    assert(split_probe.first_ready_zsg_cycle >= 25000000ull);
    assert(split_probe.first_ready_zsg_cycle <= 25000000ull + 769);
    uint64_t first_cycle_delta = one_probe.first_ready_zsg_cycle > split_probe.first_ready_zsg_cycle
        ? one_probe.first_ready_zsg_cycle - split_probe.first_ready_zsg_cycle
        : split_probe.first_ready_zsg_cycle - one_probe.first_ready_zsg_cycle;
    assert(first_cycle_delta <= 769);
    assert(gnet_zoom_try_destroy(split) == GNET_ZOOM_OK);
}

void test_busy_destroy_refusal_and_time_error_propagation()
{
    Media media;
    DestroyProbe probe;
    gnet_zoom_board *board = create_loop_board(media, &probe,
                                                probe_destroy_from_audio);
    probe.board = board;
    assert(gnet_zoom_run(board, 4000) >= 4000);
    assert(probe.callbacks != 0);
    assert(probe.result == GNET_ZOOM_BUSY);
    assert(gnet_zoom_sound_pc(board) == 0x80000); // loop is still alive after refusal
    assert(gnet_zoom_try_destroy(board) == GNET_ZOOM_OK);

    Media time_media;
    board = create_loop_board(time_media);
    assert(gnet_zoom_run(board, UINT64_MAX) == GNET_ZOOM_TIME);
    assert(gnet_zoom_try_destroy(board) == GNET_ZOOM_OK);
}

void test_mame_mb3773_control_bit_and_five_second_timer()
{
    Media media;
    SoftResetProbe probe;
    auto config = media.config();
    config.reset_opaque = &probe;
    config.request_soft_reset = probe_soft_reset;
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK && board);
    probe.board = board;

    constexpr uint64_t cycles_per_second = 6250000;
    assert(gnet_zoom_sync_to_cycle(board, 49 * cycles_per_second / 10) == GNET_ZOOM_OK);
    assert(gnet_zoom_control_write(board, 0x30) == GNET_ZOOM_OK); // CK high does not kick.
    assert(gnet_zoom_sync_to_cycle(board, 5 * cycles_per_second) == GNET_ZOOM_OK);
    assert(probe.requests == 1);
    assert(probe.destroy_result == GNET_ZOOM_BUSY);
    assert(gnet_zoom_watchdog_reset_requests(board) == 1);

    assert(gnet_zoom_control_write(board, 0x10) == GNET_ZOOM_OK); // Falling CK rearms at 5 s.
    assert(gnet_zoom_sync_to_cycle(board, 10 * cycles_per_second - 1) == GNET_ZOOM_OK);
    assert(probe.requests == 1);
    assert(gnet_zoom_sync_to_cycle(board, 10 * cycles_per_second) == GNET_ZOOM_OK);
    assert(probe.requests == 2);
    assert(probe.destroy_result == GNET_ZOOM_BUSY);
    assert(gnet_zoom_watchdog_reset_requests(board) == 2);
    assert(gnet_zoom_try_destroy(board) == GNET_ZOOM_OK);
}
} // namespace

int main()
{
    test_mn_executes_real_zoom_flash_and_bus_is_little_endian();
    test_bank_control_mapping_eprom_alias_and_wave_flash();
    test_source_maincpu_map_routes_registers_and_shared_ram_lanes();
    test_nor_busy_time_and_bad_config();
    test_cpu_fetch_advances_nor_at_bus_cycle_independent_of_quantum();
    test_busy_destroy_refusal_and_time_error_propagation();
    test_mame_mb3773_control_bit_and_five_second_timer();
    std::puts("G-Net Zoom/NOR standalone tests passed");
}
