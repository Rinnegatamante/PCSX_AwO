#include "gnet_pccard.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
struct Backend {
    gnet_pccard_t *card = nullptr;
    unsigned resets = 0;
    unsigned read_calls = 0;
    unsigned write_calls = 0;
    unsigned control_read_calls = 0;
    unsigned control_write_calls = 0;
    uint32_t last_offset = 0;
    uint16_t last_data = 0;
    uint16_t last_mask = 0;
    int last_eight_bit = -1;
    int last_read_eight_bit = -1;
    int destroy_from_reset = 999;
    int destroy_from_read = 999;
    uint16_t memory_value = 0x7654;
};

void reset(void *opaque)
{
    auto *b = static_cast<Backend *>(opaque);
    ++b->resets;
    if (b->card) b->destroy_from_reset = gnet_pccard_try_destroy(b->card);
}

uint16_t command_r(void *opaque, uint32_t offset, uint16_t mask,
                   int eight_bit)
{
    auto *b = static_cast<Backend *>(opaque);
    ++b->read_calls;
    b->last_offset = offset;
    b->last_mask = mask;
    b->last_read_eight_bit = eight_bit;
    if (b->card) b->destroy_from_read = gnet_pccard_try_destroy(b->card);
    return b->memory_value;
}

void command_w(void *opaque, uint32_t offset, uint16_t data, uint16_t mask,
               int eight_bit)
{
    auto *b = static_cast<Backend *>(opaque);
    ++b->write_calls;
    b->last_offset = offset;
    b->last_data = data;
    b->last_mask = mask;
    b->last_eight_bit = eight_bit;
}

uint16_t control_r(void *opaque, uint32_t offset, uint16_t mask)
{
    auto *b = static_cast<Backend *>(opaque);
    ++b->control_read_calls;
    b->last_offset = offset;
    b->last_mask = mask;
    return static_cast<uint16_t>(0xa000 | offset);
}

void control_w(void *opaque, uint32_t offset, uint16_t data, uint16_t mask)
{
    auto *b = static_cast<Backend *>(opaque);
    ++b->control_write_calls;
    b->last_offset = offset;
    b->last_data = data;
    b->last_mask = mask;
}

gnet_pccard_ata_ops_t backend_ops(Backend *b)
{
    return {b, reset, command_r, command_w, control_r, control_w};
}

gnet_pccard_t *create(gnet_pccard_variant_t variant, const uint8_t *key,
                      size_t key_size, Backend *backend = nullptr,
                      const uint8_t *cis = nullptr, size_t cis_size = 0)
{
    auto ata = backend_ops(backend);
    gnet_pccard_config_t config{};
    config.variant = variant;
    config.cis = cis;
    config.cis_size = cis_size;
    config.key = key;
    config.key_size = key_size;
    config.ata = backend ? &ata : nullptr;
    gnet_pccard_t *card = nullptr;
    assert(gnet_pccard_create(&config, &card) == GNET_PCCARD_OK && card);
    if (backend) backend->card = card;
    return card;
}

uint16_t attr_read(gnet_pccard_t *card, uint32_t offset, uint16_t mask = 0xffff)
{
    const auto *ops = gnet_pccard_card_ops(card);
    return ops->read_reg(ops->opaque, offset, mask);
}

void attr_write(gnet_pccard_t *card, uint32_t offset, uint16_t data,
                uint16_t mask = 0xffff)
{
    const auto *ops = gnet_pccard_card_ops(card);
    ops->write_reg(ops->opaque, offset, data, mask);
}

void test_cis_config_and_backend_dispatch()
{
    Backend backend;
    gnet_pccard_t *card = create(GNET_PCCARD_ATA_FLASH, nullptr, 0, &backend);
    assert(backend.resets == 1);
    assert(attr_read(card, 0) == 0x01);
    assert(attr_read(card, 1) == 0x04);
    assert(attr_read(card, 18) == 0xff);
    assert(attr_read(card, 19) == 0xff);
    assert(attr_read(card, 0x100) == 0);
    assert(attr_read(card, 0x101) == 0);
    assert(attr_read(card, 0x102) == 0x002e);
    attr_write(card, 0x100, 0xabcd, 0x00ff);
    attr_write(card, 0x101, 0x0056, 0x00ff);
    assert(attr_read(card, 0x100) == 0xcd);
    assert(attr_read(card, 0x101) == 0x56);
    attr_write(card, 0x100, 0x12ab, 0xff00);
    assert(attr_read(card, 0x100) == 0xab); // MAME ignores mem_mask here.
    uint16_t identify = 0;
    assert(gnet_pccard_identify_word0(card, &identify) == GNET_PCCARD_OK);
    assert(identify == 0x848a);

    const auto *ops = gnet_pccard_card_ops(card);
    rf5c296_device_t *rf = rf5c296_create(ops);
    assert(rf);
    assert(rf5c296_mem_read(rf, 0, 0xffff) == 0x01);
    rf5c296_mem_write(rf, 0x100, 0x0077, 0x00ff);
    assert(rf5c296_mem_read(rf, 0x100, 0x00ff) == 0x77);
    rf5c296_io_write(rf, 0x1f0, 0x0003, 0x00ff);
    rf5c296_io_write(rf, 0x1f0, 0x0000, 0xff00);
    assert(backend.resets == 2);
    rf5c296_destroy(rf);

    assert(ops->read_memory(ops->opaque, 6, 0xff00) == 0x7654);
    assert(backend.last_offset == 6 && backend.last_mask == 0xff00);
    assert(backend.last_read_eight_bit == 0);
    assert(ops->read_memory(ops->opaque, 6, 0x00ff) == 0x7654);
    assert(backend.last_read_eight_bit == 1);
    ops->write_memory(ops->opaque, 3, 0x1234, 0x00ff);
    assert(backend.last_offset == 3 && backend.last_data == 0x1234);
    assert(backend.last_mask == 0x00ff && backend.last_eight_bit == 1);
    ops->write_memory(ops->opaque, 3, 0x5678, 0xffff);
    assert(backend.last_eight_bit == 0);
    assert(ops->read_memory(ops->opaque, 8, 0xff00) == 0xa000);
    assert(backend.last_offset == 0);
    ops->write_memory(ops->opaque, 15, 0x3344, 0x00ff);
    assert(backend.last_offset == 7 && backend.last_mask == 0x00ff);

    const uint8_t custom[] = {0x20, 0x21, 0x22};
    assert(gnet_pccard_try_destroy(card) == GNET_PCCARD_OK);
    card = create(GNET_PCCARD_ATA_FLASH, nullptr, 0, nullptr,
                  custom, sizeof(custom));
    assert(attr_read(card, 0) == 0x20 && attr_read(card, 2) == 0x22);
    assert(attr_read(card, 3) == 0xff);
    assert(attr_read(card, 0x100) == 0);
    assert(gnet_pccard_try_destroy(card) == GNET_PCCARD_OK);

    const uint8_t empty_cis_storage = 0;
    card = create(GNET_PCCARD_ATA_FLASH, nullptr, 0, nullptr,
                  &empty_cis_storage, 0);
    assert(attr_read(card, 0) == 0xff); // Present empty CIS is padded, not defaulted.
    assert(gnet_pccard_try_destroy(card) == GNET_PCCARD_OK);
}

void test_no_backend_is_explicit()
{
    auto *card = create(GNET_PCCARD_ATA_FLASH, nullptr, 0);
    assert(!gnet_pccard_has_ata_backend(card));
    assert(gnet_pccard_reset(card) == GNET_PCCARD_NO_ATA_BACKEND);
    const auto *ops = gnet_pccard_card_ops(card);
    assert(ops->read_memory(ops->opaque, 0, 0xffff) == 0xffff);
    ops->write_memory(ops->opaque, 0, 0xbeef, 0xffff);
    assert(gnet_pccard_try_destroy(card) == GNET_PCCARD_OK);
}

void test_taito_type1_unlock()
{
    const uint8_t key[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    auto *card = create(GNET_PCCARD_TAITO_TYPE1, key, sizeof(key));
    assert(gnet_pccard_is_locked(card) && !gnet_pccard_is_ready(card));
    assert(attr_read(card, 0x201) == 1);
    gnet_pccard_taskfile_t tf{};
    tf.command = 0xec;
    gnet_pccard_state_delta_t delta{};
    assert(gnet_pccard_process_command(card, &tf, &delta) ==
           GNET_PCCARD_HOOK_HANDLED);
    assert(delta.status_set == GNET_PCCARD_STATUS_ERR);
    assert(delta.status_clear == GNET_PCCARD_STATUS_DRDY);
    assert(delta.error_valid && delta.error == GNET_PCCARD_ERROR_NONE);
    for (unsigned i = 0; i < 9; ++i) attr_write(card, 0x280 + i, 0x100);
    assert(gnet_pccard_is_locked(card));
    for (unsigned i = 0; i < 9; ++i)
        attr_write(card, 0x280 + i, i < 5 ? key[i] : 0);
    assert(!gnet_pccard_is_locked(card) && gnet_pccard_is_ready(card));
    assert(attr_read(card, 0x201) == 0);
    tf.command = 0xec;
    assert(gnet_pccard_process_command(card, &tf, &delta) ==
           GNET_PCCARD_HOOK_PASS);
    attr_write(card, 0x284, 0xff);
    assert(gnet_pccard_is_locked(card));
    assert(gnet_pccard_try_destroy(card) == GNET_PCCARD_OK);
}

void test_taito_type2_unlock_and_reset()
{
    const uint8_t key[] = {1, 2, 3, 4, 5};
    auto *card = create(GNET_PCCARD_TAITO_TYPE2, key, sizeof(key));
    assert(gnet_pccard_is_locked(card));
    gnet_pccard_taskfile_t tf{};
    gnet_pccard_state_delta_t delta{};
    tf.command = 0xfe;
    assert(gnet_pccard_process_command(card, &tf, &delta) ==
           GNET_PCCARD_HOOK_HANDLED);
    assert(delta.sector_count_valid && delta.sector_count == 1);
    assert(delta.status_set == GNET_PCCARD_STATUS_DRDY);
    assert(delta.irq_valid && delta.irq == GNET_PCCARD_IRQ_ASSERT);
    tf.command = 0xfc;
    assert(gnet_pccard_process_command(card, &tf, &delta) ==
           GNET_PCCARD_HOOK_HANDLED);
    assert(delta.transfer_bytes_valid && delta.transfer_bytes == 512);
    assert(delta.status_set == GNET_PCCARD_STATUS_DRQ);
    assert(delta.irq_valid);
    std::array<uint8_t, 512> buffer{};
    buffer[2] = 1; buffer[3] = 2; buffer[4] = 3;
    buffer[5] = 4; buffer[6] = 0xff;
    assert(gnet_pccard_process_buffer(card, 0xfc, buffer.data(), buffer.size(),
                                      &delta) == GNET_PCCARD_HOOK_HANDLED);
    assert(delta.status_set == GNET_PCCARD_STATUS_ERR && delta.error_valid);
    assert(gnet_pccard_is_locked(card));
    buffer[6] = 5;
    buffer[511] = 0x80; // MAME requires all bytes outside key[0..4] to be zero.
    assert(gnet_pccard_process_buffer(card, 0xfc, buffer.data(), buffer.size(),
                                      &delta) == GNET_PCCARD_HOOK_HANDLED);
    assert(delta.status_set == GNET_PCCARD_STATUS_ERR);
    assert(gnet_pccard_is_locked(card));
    buffer[511] = 0;
    assert(gnet_pccard_process_buffer(card, 0xfc, buffer.data(), buffer.size(),
                                      &delta) == GNET_PCCARD_HOOK_HANDLED);
    assert(!gnet_pccard_is_locked(card) && gnet_pccard_is_ready(card));
    assert(gnet_pccard_reset(card) == GNET_PCCARD_NO_ATA_BACKEND);
    assert(gnet_pccard_is_locked(card));
    assert(gnet_pccard_try_destroy(card) == GNET_PCCARD_OK);
}

void test_taito_compact_flash_unlock()
{
    const uint8_t key[] = {0x11, 0x23, 0x33, 0x44, 0x55};
    auto *card = create(GNET_PCCARD_TAITO_COMPACT_FLASH, key, sizeof(key));
    gnet_pccard_taskfile_t tf{};
    gnet_pccard_state_delta_t delta{};
    tf.command = 0x0f;
    tf.feature = 0x11;
    tf.sector_count = 0x100; // ATA taskfile value zero expands to HLE count 256.
    tf.sector_number = 0x33;
    tf.cylinder_low = 0x44;
    tf.cylinder_high = 0x54;
    assert(gnet_pccard_process_command(card, &tf, &delta) ==
           GNET_PCCARD_HOOK_HANDLED);
    assert(delta.status_clear == GNET_PCCARD_STATUS_DRDY);
    assert(delta.irq_valid && gnet_pccard_is_locked(card));
    tf.sector_count = 0x23;
    tf.cylinder_high = 0x55;
    assert(gnet_pccard_process_command(card, &tf, &delta) ==
           GNET_PCCARD_HOOK_HANDLED);
    assert(!gnet_pccard_is_locked(card) && gnet_pccard_is_ready(card));
    tf.command = 0x0f;
    tf.feature ^= 1;
    assert(gnet_pccard_process_command(card, &tf, &delta) ==
           GNET_PCCARD_HOOK_HANDLED);
    assert(gnet_pccard_is_ready(card)); // Bad unlock does not relock an open card.
    tf.command = 0xec;
    assert(gnet_pccard_process_command(card, &tf, &delta) ==
           GNET_PCCARD_HOOK_PASS);
    assert(gnet_pccard_try_destroy(card) == GNET_PCCARD_OK);
}

void test_borrowed_backend_lifetime_guard()
{
    Backend backend;
    auto *card = create(GNET_PCCARD_ATA_FLASH, nullptr, 0, &backend);
    const auto *ops = gnet_pccard_card_ops(card);
    ops->reset(ops->opaque);
    assert(backend.destroy_from_reset == GNET_PCCARD_BUSY);
    (void)ops->read_memory(ops->opaque, 0, 0xffff);
    assert(backend.destroy_from_read == GNET_PCCARD_BUSY);
    assert(gnet_pccard_try_destroy(card) == GNET_PCCARD_OK);
}
} // namespace

int main()
{
    test_cis_config_and_backend_dispatch();
    test_no_backend_is_explicit();
    test_taito_type1_unlock();
    test_taito_type2_unlock_and_reset();
    test_taito_compact_flash_unlock();
    test_borrowed_backend_lifetime_guard();
    std::puts("G-Net PC Card tests passed (synthetic backend fixture; no ATA media)");
    return 0;
}
