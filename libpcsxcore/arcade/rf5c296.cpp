/* RF5C296 PC Card controller, adapted from MAME.
 * Source: src/devices/machine/rf5c296.cpp, revision
 * deb2f54fe7300e7a192690318beb860159bfd52a. Copyright smf.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "rf5c296.h"

#include <new>

struct rf5c296_device {
    rf5c296_card_ops_t card{};
    uint8_t selected_register = 0;
};

extern "C" rf5c296_device_t *rf5c296_create(const rf5c296_card_ops_t *card)
{
    if (!card || !card->read_memory || !card->write_memory ||
        !card->read_reg || !card->write_reg || !card->reset)
        return nullptr;
    auto *device = new (std::nothrow) rf5c296_device;
    if (device) device->card = *card;
    return device;
}

extern "C" void rf5c296_destroy(rf5c296_device_t *device)
{
    delete device;
}

extern "C" void rf5c296_io_write(rf5c296_device_t *device,
                                  uint32_t offset, uint16_t data,
                                  uint16_t mem_mask)
{
    if (!device) return;

    /* Source io_w: offsets are 16-bit units, then expanded to the PC Card
     * byte address. An upper-byte access addresses the next byte register. */
    offset *= 2;
    if (mem_mask == 0xff00) {
        mem_mask >>= 8;
        data >>= 8;
        offset++;
    }

    switch (offset) {
    case 0x3e0:
        device->selected_register = static_cast<uint8_t>(data);
        break;
    case 0x3e1:
        /* MAME only implements the card-reset side effect of this register. */
        if (device->selected_register == 0x03 && !(data & 0x40))
            device->card.reset(device->card.opaque);
        break;
    default:
        device->card.write_memory(device->card.opaque, offset, data,
                                  mem_mask);
        break;
    }
}

extern "C" uint16_t rf5c296_io_read(rf5c296_device_t *device,
                                    uint32_t offset, uint16_t mem_mask)
{
    if (!device) return 0xffff;

    /* Source io_r, including its byte-lane conversion and zero-valued
     * register reads. */
    offset *= 2;
    unsigned shift = 0;
    if (mem_mask == 0xff00) {
        shift = 8;
        mem_mask >>= 8;
        offset++;
    }

    uint16_t data;
    switch (offset) {
    case 0x3e0:
        data = device->selected_register;
        break;
    case 0x3e1:
        data = 0x00; // MAME's reg_r currently returns zero for all registers.
        break;
    default:
        data = device->card.read_memory(device->card.opaque, offset,
                                        mem_mask);
        break;
    }
    return static_cast<uint16_t>(data << shift);
}

extern "C" uint16_t rf5c296_mem_read(rf5c296_device_t *device,
                                     uint32_t offset, uint16_t mem_mask)
{
    return device ? device->card.read_reg(device->card.opaque, offset,
                                          mem_mask) : 0xffff;
}

extern "C" void rf5c296_mem_write(rf5c296_device_t *device,
                                  uint32_t offset, uint16_t data,
                                  uint16_t mem_mask)
{
    if (device)
        device->card.write_reg(device->card.opaque, offset, data, mem_mask);
}
