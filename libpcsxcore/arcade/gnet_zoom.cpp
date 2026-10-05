/* GPL-2.0-or-later. Source-derived Taito G-Net flash/Zoom composition. */
#include "gnet_zoom.h"
#include "nor_flash.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

struct gnet_zoom_board {
    gnet_zoom_config_t config{};
    std::array<nor_flash_t *, 5> flash{};
    std::array<uint8_t *, 5> raw{};
    std::array<size_t, 5> size{};
    std::array<std::vector<uint8_t>, 5> engine_image{};
    zoom_board *zoom = nullptr;
    uint64_t last_cycle = 0;
    uint64_t fractional_25ths = 0;
    unsigned bank = 0;
    uint8_t control = 0x10;
    uint32_t fault = 0;
};

namespace {
constexpr uint32_t MAIN_BASE = 0x1f000000u;
constexpr uint32_t MAIN_END = 0x1f800000u;
constexpr uint64_t BANK_STRIDE = 0x08000000ull;
constexpr uint64_t FIRM_BEGIN = 0x00000000ull;
constexpr uint64_t FIRM_END = 0x00200000ull;
constexpr uint64_t PCMCIA_BEGIN = 0x00200000ull;
constexpr uint64_t PCMCIA_END = 0x00300000ull;
constexpr uint64_t PROG_BEGIN = 0x00300000ull;
constexpr uint64_t PROG_END = 0x00380000ull;
constexpr uint64_t WAVE0_BEGIN = 0x08000000ull;
constexpr uint64_t WAVE1_BEGIN = 0x08200000ull;
constexpr uint64_t WAVE2_BEGIN = 0x08400000ull;
constexpr uint64_t WAVE_END = 0x08600000ull;
constexpr uint64_t EPROM_BEGIN = 0x10000000ull;
constexpr uint64_t EPROM_END = 0x10100000ull;
constexpr uint64_t BANK2_PROG_BEGIN = 0x10100000ull;
constexpr uint64_t BANK2_PROG_END = 0x10200000ull; // mirrored 512 KiB window
constexpr uint64_t BANK2_FIRM_BEGIN = 0x10200000ull;
constexpr uint64_t BANK2_FIRM_END = 0x10400000ull;
constexpr uint64_t MN_CYCLES_PER_SECOND = 6250000ull;

static int result_from_nor(int result)
{
    if (result == NOR_FLASH_OK) return GNET_ZOOM_OK;
    if (result == NOR_FLASH_ERR_RANGE) return GNET_ZOOM_UNMAPPED;
    if (result == NOR_FLASH_ERR_UNSUPPORTED || result == NOR_FLASH_ERR_SEQUENCE)
        return GNET_ZOOM_UNSUPPORTED;
    return GNET_ZOOM_DEVICE;
}

static void dirty_image_to_raw(gnet_zoom_board *b, unsigned chip)
{
    size_t offset = 0, length = 0;
    if (!nor_flash_last_write_dirty_span(b->flash[chip], &offset, &length))
        return;
    const auto &image = b->engine_image[chip];
    uint8_t *raw = b->raw[chip];
    size_t end = offset + length;
    offset &= ~size_t(1);
    end = (end + 1) & ~size_t(1);
    if (end > image.size()) end = image.size();
    for (size_t i = offset; i < end; i += 2) {
        raw[i] = image[i + 1];
        raw[i + 1] = image[i];
    }
}

static int result_from_zoom(int result)
{
    switch (result) {
    case ZOOM_BOARD_OK: return GNET_ZOOM_OK;
    case ZOOM_BOARD_ARGUMENT: return GNET_ZOOM_ARGUMENT;
    case ZOOM_BOARD_NOMEM: return GNET_ZOOM_NOMEM;
    case ZOOM_BOARD_TIME: return GNET_ZOOM_TIME;
    case ZOOM_BOARD_UNSUPPORTED: return GNET_ZOOM_UNSUPPORTED;
    default: return GNET_ZOOM_DEVICE;
    }
}

static int flash_read(gnet_zoom_board *b, unsigned chip, uint32_t word,
                      uint16_t *value)
{
    if (!b || chip >= b->flash.size() || !value) return GNET_ZOOM_ARGUMENT;
    return result_from_nor(nor_flash_read(b->flash[chip], word, value));
}

static int flash_write(gnet_zoom_board *b, unsigned chip, uint32_t word,
                       uint16_t data)
{
    if (!b || chip >= b->flash.size()) return GNET_ZOOM_ARGUMENT;
    int r = nor_flash_write(b->flash[chip], word, data);
    if (r == NOR_FLASH_OK) dirty_image_to_raw(b, chip);
    return result_from_nor(r);
}

static int advance_flash_clock(gnet_zoom_board *b, uint64_t cycle);

static int zoom_flash_read16(void *opaque, unsigned zoom_chip, uint32_t word,
                             uint16_t *value)
{
    auto *b = static_cast<gnet_zoom_board *>(opaque);
    /* Zoom chip 0 is the E28F400B program flash; chips 1..3 are its waves. */
    if (!b) return GNET_ZOOM_ARGUMENT;
    return flash_read(b, zoom_chip + 1, word, value);
}

static int zoom_time_sync(void *opaque, uint64_t internal_cycle)
{
    auto *b = static_cast<gnet_zoom_board *>(opaque);
    if (!b) return GNET_ZOOM_ARGUMENT;
    int result = advance_flash_clock(b, internal_cycle);
    if (result != GNET_ZOOM_OK) b->fault = ZOOM_FAULT_TIME;
    return result;
}

static void zoom_audio_frame(void *opaque, float left, float right,
                             uint64_t cycle)
{
    auto *b = static_cast<gnet_zoom_board *>(opaque);
    if (b->config.audio_frame)
        b->config.audio_frame(b->config.audio_opaque, left, right, cycle);
}

static int zoom_flash_write16(void *opaque, unsigned zoom_chip, uint32_t word,
                              uint16_t data, uint16_t mem_mask)
{
    if (mem_mask != 0xffff) return GNET_ZOOM_UNSUPPORTED;
    auto *b = static_cast<gnet_zoom_board *>(opaque);
    return flash_write(b, zoom_chip + 1, word, data);
}

static int advance_flash_clock(gnet_zoom_board *b, uint64_t cycle)
{
    if (cycle < b->last_cycle) return GNET_ZOOM_TIME;
    uint64_t elapsed = cycle - b->last_cycle;
    if (elapsed > (std::numeric_limits<uint64_t>::max() - b->fractional_25ths) / 4)
        return GNET_ZOOM_TIME;
    uint64_t scaled = elapsed * 4 + b->fractional_25ths;
    uint64_t microseconds = scaled / 25;
    b->fractional_25ths = scaled % 25;
    if (microseconds) {
        for (nor_flash_t *flash : b->flash)
            nor_flash_advance_us(flash, microseconds);
    }
    b->last_cycle = cycle;
    return GNET_ZOOM_OK;
}

struct FlashMap {
    unsigned chip;
    uint32_t word;
};

static int decode_main_flash(gnet_zoom_board *b, uint32_t address,
                             FlashMap *map)
{
    if (!b || !map || address < MAIN_BASE || address >= MAIN_END)
        return GNET_ZOOM_UNMAPPED;
    if (address & 1) return GNET_ZOOM_ARGUMENT;
    uint64_t offset = uint64_t(address - MAIN_BASE);
    uint64_t effective = uint64_t(b->bank) * BANK_STRIDE + offset;

    if (b->bank == 0) {
        if (effective >= FIRM_BEGIN && effective < FIRM_END) {
            map->chip = 0;
            map->word = uint32_t(effective >> 1);
            return GNET_ZOOM_OK;
        }
        if (effective >= PCMCIA_BEGIN && effective < PCMCIA_END)
            return GNET_ZOOM_UNSUPPORTED;
        if (effective >= PROG_BEGIN && effective < PROG_END) {
            map->chip = 1;
            map->word = uint32_t((effective - PROG_BEGIN) >> 1);
            return GNET_ZOOM_OK;
        }
        return GNET_ZOOM_UNMAPPED;
    }

    if (b->bank == 1 || b->bank == 3) {
        /* MAME's mirror(0x10000000) aliases bank 3 onto bank 1's waves. */
        effective &= ~0x10000000ull;
        if (effective >= WAVE0_BEGIN && effective < WAVE1_BEGIN) {
            map->chip = 2;
            map->word = uint32_t((effective - WAVE0_BEGIN) >> 1);
        } else if (effective >= WAVE1_BEGIN && effective < WAVE2_BEGIN) {
            map->chip = 3;
            map->word = uint32_t((effective - WAVE1_BEGIN) >> 1);
        } else if (effective >= WAVE2_BEGIN && effective < WAVE_END) {
            map->chip = 4;
            map->word = uint32_t((effective - WAVE2_BEGIN) >> 1);
        } else return GNET_ZOOM_UNMAPPED;
        return GNET_ZOOM_OK;
    }

    if (b->bank == 2) {
        if (effective >= EPROM_BEGIN && effective < EPROM_END)
            return GNET_ZOOM_READ_ONLY;
        if (effective >= BANK2_PROG_BEGIN && effective < BANK2_PROG_END) {
            map->chip = 1;
            map->word = uint32_t(((effective - BANK2_PROG_BEGIN) & 0x7ffffull) >> 1);
            return GNET_ZOOM_OK;
        }
        if (effective >= BANK2_FIRM_BEGIN && effective < BANK2_FIRM_END) {
            map->chip = 0;
            map->word = uint32_t((effective - BANK2_FIRM_BEGIN) >> 1);
            return GNET_ZOOM_OK;
        }
    }
    return GNET_ZOOM_UNMAPPED;
}

static uint64_t main_effective_address(const gnet_zoom_board *b,
                                       uint32_t address)
{
    return uint64_t(b->bank) * BANK_STRIDE + uint64_t(address - MAIN_BASE);
}
} // namespace

extern "C" int gnet_zoom_create(const gnet_zoom_config_t *config,
                                 gnet_zoom_board **out)
{
    if (!out) return GNET_ZOOM_ARGUMENT;
    *out = nullptr;
    if (!config || config->jp1 > 1 ||
        !config->firm_flash || config->firm_flash_size != GNET_FIRM_FLASH_BYTES ||
        !config->zoom_program_flash ||
        config->zoom_program_flash_size != GNET_ZOOM_PROGRAM_FLASH_BYTES ||
        !config->eprom || config->eprom_size != GNET_EPROM_BYTES)
        return GNET_ZOOM_ARGUMENT;
    for (unsigned i = 0; i < 3; ++i)
        if (!config->sample_flash[i] ||
            config->sample_flash_size[i] != ZOOM_SAMPLE_FLASH_BYTES)
            return GNET_ZOOM_ARGUMENT;

    auto *b = new (std::nothrow) gnet_zoom_board;
    if (!b) return GNET_ZOOM_NOMEM;
    b->config = *config;
    b->raw[0] = config->firm_flash;
    b->size[0] = config->firm_flash_size;
    b->raw[1] = config->zoom_program_flash;
    b->size[1] = config->zoom_program_flash_size;
    for (unsigned i = 0; i < 3; ++i) {
        b->raw[i + 2] = config->sample_flash[i];
        b->size[i + 2] = config->sample_flash_size[i];
    }
    try {
        for (unsigned i = 0; i < b->flash.size(); ++i) {
            b->engine_image[i].resize(b->size[i]);
            for (size_t byte = 0; byte < b->size[i]; byte += 2) {
                /* intelfsh16 stores words high-byte first; source ROM images
                 * are little-endian, so bridge the representation here. */
                b->engine_image[i][byte] = b->raw[i][byte + 1];
                b->engine_image[i][byte + 1] = b->raw[i][byte];
            }
        }
    } catch (...) {
        delete b;
        return GNET_ZOOM_NOMEM;
    }

    b->flash[0] = nor_flash_create(NOR_FLASH_INTEL_TE28F160_X16,
                                    b->engine_image[0].data(), b->size[0]);
    b->flash[1] = nor_flash_create(NOR_FLASH_INTEL_E28F400B_X16,
                                    b->engine_image[1].data(), b->size[1]);
    for (unsigned i = 0; i < 3; ++i)
        b->flash[i + 2] = nor_flash_create(NOR_FLASH_INTEL_TE28F160_X16,
                                            b->engine_image[i + 2].data(),
                                            b->size[i + 2]);
    for (nor_flash_t *flash : b->flash) {
        if (!flash) {
            for (nor_flash_t *made : b->flash) nor_flash_destroy(made);
            delete b;
            return GNET_ZOOM_NOMEM;
        }
    }

    zoom_board_config zoom_config{};
    zoom_config.boot_flash = config->zoom_program_flash;
    zoom_config.boot_flash_size = config->zoom_program_flash_size;
    for (unsigned i = 0; i < 3; ++i) {
        zoom_config.sample_flash[i] = config->sample_flash[i];
        zoom_config.sample_flash_size[i] = config->sample_flash_size[i];
    }
    zoom_config.flash_opaque = b;
    zoom_config.flash_read16 = zoom_flash_read16;
    zoom_config.flash_write16 = zoom_flash_write16;
    zoom_config.audio_opaque = b;
    zoom_config.audio_frame = config->audio_frame ? zoom_audio_frame : nullptr;
    zoom_config.time_opaque = b;
    zoom_config.time_sync = zoom_time_sync;
    if (zoom_board_create(&zoom_config, &b->zoom) != ZOOM_BOARD_OK) {
        for (nor_flash_t *flash : b->flash) nor_flash_destroy(flash);
        delete b;
        return GNET_ZOOM_DEVICE;
    }
    b->bank = config->jp1 << 1;
    b->last_cycle = zoom_board_main_cycles(b->zoom);
    *out = b;
    return GNET_ZOOM_OK;
}

extern "C" int gnet_zoom_try_destroy(gnet_zoom_board *b)
{
    if (!b) return GNET_ZOOM_OK;
    if (zoom_board_is_busy(b->zoom)) return GNET_ZOOM_BUSY;
    zoom_board_destroy(b->zoom);
    for (nor_flash_t *flash : b->flash) nor_flash_destroy(flash);
    delete b;
    return GNET_ZOOM_OK;
}

extern "C" void gnet_zoom_destroy(gnet_zoom_board *b)
{
    (void)gnet_zoom_try_destroy(b);
}

extern "C" uint8_t gnet_zoom_control_read(const gnet_zoom_board *b)
{
    return b ? b->control : 0xff;
}

extern "C" int gnet_zoom_control_write(gnet_zoom_board *b, uint8_t data)
{
    if (!b) return GNET_ZOOM_ARGUMENT;
    int clock_result = advance_flash_clock(b, zoom_board_main_cycles(b->zoom));
    if (clock_result != GNET_ZOOM_OK) return clock_result;
    bool old_reset = (b->control & 0x10) != 0;
    bool next_reset = (data & 0x10) != 0;
    if (old_reset != next_reset) {
        int r = zoom_board_set_sound_reset(b->zoom, next_reset);
        if (r != ZOOM_BOARD_OK)
            return (r == ZOOM_BOARD_TIME || b->fault == ZOOM_FAULT_TIME)
                ? GNET_ZOOM_TIME : result_from_zoom(r);
    }
    b->control = data;
    b->bank = ((data >> 2) & 1) | (b->config.jp1 << 1);
    return GNET_ZOOM_OK;
}

extern "C" unsigned gnet_zoom_selected_bank(const gnet_zoom_board *b)
{
    return b ? b->bank : 0;
}

extern "C" int gnet_zoom_main_read16(gnet_zoom_board *b, uint32_t address,
                                      uint16_t *value)
{
    if (!b || !value) return GNET_ZOOM_ARGUMENT;
    FlashMap map{};
    int decoded = decode_main_flash(b, address, &map);
    if (decoded == GNET_ZOOM_READ_ONLY) {
        uint32_t offset = uint32_t(main_effective_address(b, address) - EPROM_BEGIN);
        *value = uint16_t(b->config.eprom[offset]) |
                 (uint16_t(b->config.eprom[offset + 1]) << 8);
        return GNET_ZOOM_OK;
    }
    if (decoded != GNET_ZOOM_OK) return decoded;
    int clock_result = advance_flash_clock(b, zoom_board_main_cycles(b->zoom));
    if (clock_result != GNET_ZOOM_OK) return clock_result;
    return flash_read(b, map.chip, map.word, value);
}

extern "C" int gnet_zoom_main_write16(gnet_zoom_board *b, uint32_t address,
                                       uint16_t data, uint16_t mem_mask)
{
    if (!b) return GNET_ZOOM_ARGUMENT;
    int clock_result = advance_flash_clock(b, zoom_board_main_cycles(b->zoom));
    if (clock_result != GNET_ZOOM_OK) return clock_result;
    FlashMap map{};
    int decoded = decode_main_flash(b, address, &map);
    if (decoded == GNET_ZOOM_READ_ONLY) return GNET_ZOOM_READ_ONLY;
    if (decoded != GNET_ZOOM_OK) return decoded;
    if (mem_mask != 0xffff) return GNET_ZOOM_UNSUPPORTED;
    return flash_write(b, map.chip, map.word, data);
}

extern "C" int gnet_zoom_sync_to_cycle(gnet_zoom_board *b,
                                        uint64_t internal_cycle)
{
    if (!b) return GNET_ZOOM_ARGUMENT;
    int r = zoom_board_sync_to_cycle(b->zoom, internal_cycle);
    if (r != ZOOM_BOARD_OK) {
        if (!b->fault) b->fault = zoom_board_fault(b->zoom);
        return (r == ZOOM_BOARD_TIME || b->fault == ZOOM_FAULT_TIME)
            ? GNET_ZOOM_TIME : result_from_zoom(r);
    }
    r = advance_flash_clock(b, zoom_board_main_cycles(b->zoom));
    if (r != GNET_ZOOM_OK) b->fault = ZOOM_FAULT_TIME;
    return r;
}

extern "C" int64_t gnet_zoom_run(gnet_zoom_board *b, uint64_t cycles)
{
    if (!b) return GNET_ZOOM_ARGUMENT;
    int64_t ran = zoom_board_run(b->zoom, cycles);
    if (ran < 0) {
        if (!b->fault) b->fault = zoom_board_fault(b->zoom);
        return (ran == ZOOM_BOARD_TIME || b->fault == ZOOM_FAULT_TIME)
            ? GNET_ZOOM_TIME : result_from_zoom((int)ran);
    }
    if (advance_flash_clock(b, zoom_board_main_cycles(b->zoom)) != GNET_ZOOM_OK) {
        b->fault = ZOOM_FAULT_TIME;
        return GNET_ZOOM_TIME;
    }
    return ran;
}

extern "C" uint32_t gnet_zoom_sound_pc(const gnet_zoom_board *b)
{
    return b ? zoom_board_sound_pc(b->zoom) : 0;
}

extern "C" uint32_t gnet_zoom_sound_d(const gnet_zoom_board *b, unsigned index)
{
    return b ? zoom_board_sound_d(b->zoom, index) : 0;
}

extern "C" uint32_t gnet_zoom_sound_ram_read(const gnet_zoom_board *b,
                                              unsigned offset)
{
    return b ? zoom_board_sound_ram_read(b->zoom, offset) : 0xff;
}

extern "C" uint32_t gnet_zoom_fault(const gnet_zoom_board *b)
{
    if (!b) return GNET_ZOOM_ARGUMENT;
    return b->fault ? b->fault : zoom_board_fault(b->zoom);
}

extern "C" int gnet_zoom_flash_ready(const gnet_zoom_board *b,
                                      unsigned chip)
{
    if (!b || chip >= b->flash.size()) return GNET_ZOOM_ARGUMENT;
    return nor_flash_ready(b->flash[chip]);
}
