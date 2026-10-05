/* G-Net ATA flash PC Card behavior, adapted from MAME ataflash.cpp.
 * Source revision: deb2f54fe7300e7a192690318beb860159bfd52a.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "gnet_pccard.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <vector>

struct gnet_pccard {
    gnet_pccard_variant_t variant = GNET_PCCARD_ATA_FLASH;
    std::vector<uint8_t> cis;
    std::array<uint8_t, 5> key{};
    bool key_present = false;
    uint16_t type1_locked = 0;
    bool locked = false;
    uint8_t configuration_option = 0;
    uint8_t configuration_status = 0;
    uint16_t pin_replacement = 0x002e;
    gnet_pccard_ata_ops_t ata{};
    bool has_ata = false;
    unsigned callback_depth = 0;
    rf5c296_card_ops_t card_ops{};
};

namespace {

constexpr uint32_t kCisVisibleSize = 0x100;
constexpr uint32_t kTaskfileSize = 8;
constexpr uint32_t kControlSize = 8;
constexpr uint32_t kType1KeyBase = 0x280;
constexpr uint32_t kType1LockedReg = 0x201;
constexpr uint32_t kConfigurationOption = 0x100;
constexpr uint32_t kConfigurationStatus = 0x101;
constexpr uint32_t kPinReplacement = 0x102;
constexpr uint8_t kType2UnlockCommand1 = 0xfe;
constexpr uint8_t kType2UnlockCommand2 = 0xfc;
constexpr uint8_t kCompactFlashUnlockCommand = 0x0f;

struct CallbackGuard {
    explicit CallbackGuard(gnet_pccard *card_) : card(card_) { ++card->callback_depth; }
    ~CallbackGuard() { --card->callback_depth; }
    gnet_pccard *card;
};

void clear_delta(gnet_pccard_state_delta_t *delta)
{
    if (delta) std::memset(delta, 0, sizeof(*delta));
}

void set_locked_command_error(const gnet_pccard_taskfile_t *taskfile,
                              gnet_pccard_state_delta_t *delta)
{
    delta->status_set = GNET_PCCARD_STATUS_ERR;
    delta->status_clear = GNET_PCCARD_STATUS_DRDY;
    delta->error = GNET_PCCARD_ERROR_NONE;
    delta->error_valid = 1;
    (void)taskfile;
}

static uint16_t read_memory(void *opaque, uint32_t offset, uint16_t mem_mask)
{
    auto *card = static_cast<gnet_pccard *>(opaque);
    CallbackGuard guard(card);
    if (!card->has_ata) return 0xffff;
    if (offset < kTaskfileSize)
        return card->ata.command_r(card->ata.opaque, offset, mem_mask,
                                   (mem_mask & 0xff00) == 0);
    if (offset < kTaskfileSize + kControlSize)
        return card->ata.control_r(card->ata.opaque, offset & 7, mem_mask);
    return 0xffff;
}

static void write_memory(void *opaque, uint32_t offset, uint16_t data,
                         uint16_t mem_mask)
{
    auto *card = static_cast<gnet_pccard *>(opaque);
    CallbackGuard guard(card);
    if (!card->has_ata) return;
    if (offset < kTaskfileSize) {
        card->ata.command_w(card->ata.opaque, offset, data, mem_mask,
                            (mem_mask & 0xff00) == 0);
    } else if (offset < kTaskfileSize + kControlSize) {
        card->ata.control_w(card->ata.opaque, offset & 7, data, mem_mask);
    }
}

static uint16_t read_reg(void *opaque, uint32_t offset, uint16_t mem_mask)
{
    auto *card = static_cast<gnet_pccard *>(opaque);
    CallbackGuard guard(card);
    switch (offset) {
    case kConfigurationOption: return card->configuration_option;
    case kConfigurationStatus: return card->configuration_status;
    case kPinReplacement: return card->pin_replacement;
    case kType1LockedReg:
        if (card->variant == GNET_PCCARD_TAITO_TYPE1)
            return card->type1_locked != 0;
        break;
    default:
        if (offset < kCisVisibleSize) return card->cis[offset];
        break;
    }
    (void)mem_mask;
    return 0xffff;
}

static void write_reg(void *opaque, uint32_t offset, uint16_t data,
                      uint16_t mem_mask)
{
    auto *card = static_cast<gnet_pccard *>(opaque);
    CallbackGuard guard(card);
    if (card->variant == GNET_PCCARD_TAITO_TYPE1 &&
        offset >= kType1KeyBase && offset <= kType1KeyBase + 8) {
        const unsigned pos = offset - kType1KeyBase;
        const uint8_t expected = pos < card->key.size() ? card->key[pos] : 0;
        if (static_cast<uint8_t>(data) == expected)
            card->type1_locked &= static_cast<uint16_t>(~(1u << pos));
        else
            card->type1_locked |= static_cast<uint16_t>(1u << pos);
        return;
    }

    switch (offset) {
    case 0x07: // MAME documents writes here but leaves their meaning unknown.
        return;
    case kConfigurationOption:
        card->configuration_option = static_cast<uint8_t>(data);
        return;
    case kConfigurationStatus:
        card->configuration_status = static_cast<uint8_t>(data);
        return;
    default:
        (void)mem_mask;
        return;
    }
}

static void reset_callback(void *opaque)
{
    (void)gnet_pccard_reset(static_cast<gnet_pccard *>(opaque));
}

} // namespace

extern "C" int gnet_pccard_create(const gnet_pccard_config_t *config,
                                    gnet_pccard_t **out)
{
    if (out) *out = nullptr;
    if (!config || !out || config->variant < GNET_PCCARD_ATA_FLASH ||
        config->variant > GNET_PCCARD_TAITO_COMPACT_FLASH ||
        (config->cis_size && !config->cis) || (config->key_size && !config->key))
        return GNET_PCCARD_ARGUMENT;
    if (config->ata && (!config->ata->reset || !config->ata->command_r ||
        !config->ata->command_w || !config->ata->control_r ||
        !config->ata->control_w))
        return GNET_PCCARD_ARGUMENT;

    auto *card = new (std::nothrow) gnet_pccard;
    if (!card) return GNET_PCCARD_NOMEM;
    card->variant = config->variant;
    card->cis.assign(512, 0xff);
    if (config->cis_size) {
        card->cis.assign(config->cis, config->cis + config->cis_size);
        if (card->cis.size() < 512) card->cis.resize(512, 0xff);
    } else {
        static constexpr uint8_t default_cis[] = {
            0x01, 0x04, 0xdf, 0x4a, 0x01, 0xff,
            0x1a, 0x05, 0x01, 0x03, 0x00, 0x02, 0x0f,
            0x21, 0x02, 0x04, 0x01, 0xff, 0xff
        };
        std::copy(std::begin(default_cis), std::end(default_cis), card->cis.begin());
    }
    if (config->key_size == card->key.size()) {
        std::copy(config->key, config->key + card->key.size(), card->key.begin());
        card->key_present = true;
    }
    if (config->ata) {
        card->ata = *config->ata;
        card->has_ata = true;
    }
    card->card_ops = {card, read_memory, write_memory, read_reg, write_reg,
                      reset_callback};
    (void)gnet_pccard_reset(card);
    *out = card;
    return GNET_PCCARD_OK;
}

extern "C" int gnet_pccard_try_destroy(gnet_pccard_t *card)
{
    if (!card) return GNET_PCCARD_ARGUMENT;
    if (card->callback_depth) return GNET_PCCARD_BUSY;
    delete card;
    return GNET_PCCARD_OK;
}

extern "C" void gnet_pccard_destroy(gnet_pccard_t *card)
{
    if (card) (void)gnet_pccard_try_destroy(card);
}

extern "C" int gnet_pccard_reset(gnet_pccard_t *card)
{
    if (!card) return GNET_PCCARD_ARGUMENT;
    card->configuration_option = 0;
    card->configuration_status = 0;
    card->pin_replacement = 0x002e;
    card->type1_locked = card->key_present ? 0x1ff : 0;
    card->locked = card->key_present &&
        (card->variant == GNET_PCCARD_TAITO_TYPE2 ||
         card->variant == GNET_PCCARD_TAITO_COMPACT_FLASH);
    if (card->has_ata) {
        CallbackGuard guard(card);
        card->ata.reset(card->ata.opaque);
        return GNET_PCCARD_OK;
    }
    return GNET_PCCARD_NO_ATA_BACKEND;
}

extern "C" int gnet_pccard_has_ata_backend(const gnet_pccard_t *card)
{
    return card && card->has_ata;
}

extern "C" int gnet_pccard_is_locked(const gnet_pccard_t *card)
{
    if (!card) return 0;
    if (card->variant == GNET_PCCARD_TAITO_TYPE1)
        return card->type1_locked != 0;
    return card->locked;
}

extern "C" int gnet_pccard_is_ready(const gnet_pccard_t *card)
{
    return card && !gnet_pccard_is_locked(card);
}

extern "C" const rf5c296_card_ops_t *gnet_pccard_card_ops(gnet_pccard_t *card)
{
    return card ? &card->card_ops : nullptr;
}

extern "C" gnet_pccard_hook_result_t gnet_pccard_process_command(
    gnet_pccard_t *card, const gnet_pccard_taskfile_t *taskfile,
    gnet_pccard_state_delta_t *delta)
{
    clear_delta(delta);
    if (!card || !taskfile || !delta) return GNET_PCCARD_HOOK_ERROR;

    if (card->variant == GNET_PCCARD_TAITO_TYPE2) {
        if (taskfile->command == kType2UnlockCommand1) {
            delta->sector_count = 1;
            delta->sector_count_valid = 1;
            delta->status_set = GNET_PCCARD_STATUS_DRDY;
            delta->irq = GNET_PCCARD_IRQ_ASSERT;
            delta->irq_valid = 1;
            return GNET_PCCARD_HOOK_HANDLED;
        }
        if (taskfile->command == kType2UnlockCommand2) {
            delta->status_set = GNET_PCCARD_STATUS_DRQ;
            delta->irq = GNET_PCCARD_IRQ_ASSERT;
            delta->irq_valid = 1;
            delta->transfer_bytes = 512;
            delta->transfer_bytes_valid = 1;
            return GNET_PCCARD_HOOK_HANDLED;
        }
    } else if (card->variant == GNET_PCCARD_TAITO_COMPACT_FLASH &&
               taskfile->command == kCompactFlashUnlockCommand) {
        const bool valid = taskfile->feature == card->key[0] &&
            taskfile->sector_count == card->key[1] &&
            taskfile->sector_number == card->key[2] &&
            taskfile->cylinder_low == card->key[3] &&
            taskfile->cylinder_high == card->key[4];
        if (valid) card->locked = false;
        else delta->status_clear = GNET_PCCARD_STATUS_DRDY;
        delta->irq = GNET_PCCARD_IRQ_ASSERT;
        delta->irq_valid = 1;
        return GNET_PCCARD_HOOK_HANDLED;
    }

    if (card->variant == GNET_PCCARD_TAITO_TYPE1 &&
        card->type1_locked != 0) {
        set_locked_command_error(taskfile, delta);
        return GNET_PCCARD_HOOK_HANDLED;
    }
    if ((card->variant == GNET_PCCARD_TAITO_TYPE2 ||
         card->variant == GNET_PCCARD_TAITO_COMPACT_FLASH) && card->locked) {
        set_locked_command_error(taskfile, delta);
        return GNET_PCCARD_HOOK_HANDLED;
    }
    return GNET_PCCARD_HOOK_PASS;
}

extern "C" gnet_pccard_hook_result_t gnet_pccard_process_buffer(
    gnet_pccard_t *card, uint8_t command, const uint8_t *buffer,
    size_t buffer_size, gnet_pccard_state_delta_t *delta)
{
    clear_delta(delta);
    if (!card || !delta) return GNET_PCCARD_HOOK_ERROR;
    if (card->variant != GNET_PCCARD_TAITO_TYPE2 ||
        command != kType2UnlockCommand2)
        return GNET_PCCARD_HOOK_PASS;

    bool bad = !buffer || buffer_size != 512;
    for (size_t i = 0; !bad && i < 512; ++i) {
        if (i < 2 || i >= 7)
            bad = buffer[i] != 0;
        else
            bad = buffer[i] != card->key[i - 2];
    }
    if (bad) {
        delta->status_set = GNET_PCCARD_STATUS_ERR;
        delta->error = GNET_PCCARD_ERROR_NONE;
        delta->error_valid = 1;
    } else {
        card->locked = false;
    }
    return GNET_PCCARD_HOOK_HANDLED;
}

extern "C" int gnet_pccard_identify_word0(gnet_pccard_t *card,
                                            uint16_t *word0)
{
    if (!card || !word0) return GNET_PCCARD_ARGUMENT;
    *word0 = 0x848a;
    return GNET_PCCARD_OK;
}
