/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright Ville Linde. See k056800_LICENSE.txt.
 * Adapted from src/devices/sound/k056800.cpp/.h, MAME revision
 * deb2f54fe7300e7a192690318beb860159bfd52a. */
#include "k056800.h"
#include <string.h>

static void interrupt(K056800 *chip, int state)
{
    chip->interrupt_line = (uint8_t)state;
    if (chip->interrupt) {
        chip->in_callback = 1;
        chip->interrupt(chip->opaque, state);
        chip->in_callback = 0;
    }
}

int k056800_init(K056800 *chip, void (*handler)(void *, int), void *opaque)
{
    if (!chip) return -1;
    memset(chip, 0, sizeof(*chip));
    chip->interrupt = handler;
    chip->opaque = opaque;
    return 0;
}

int k056800_reset(K056800 *chip)
{
    if (!chip || chip->in_callback) return -1;
    chip->interrupt_pending = chip->interrupt_enabled = 0;
    memset(chip->host_to_sound, 0, sizeof(chip->host_to_sound));
    memset(chip->sound_to_host, 0, sizeof(chip->sound_to_host));
    return 0;
}

uint8_t k056800_host_read(const K056800 *chip, uint32_t offset)
{
    unsigned reg = offset & 7;
    return chip && reg < 2 ? chip->sound_to_host[reg] : 0;
}

uint8_t k056800_sound_read(const K056800 *chip, uint32_t offset)
{
    unsigned reg = offset & 7;
    return chip && reg < 4 ? chip->host_to_sound[reg] : 0;
}

int k056800_host_write(K056800 *chip, uint32_t offset, uint8_t data)
{
    unsigned reg = offset & 7;
    if (!chip || chip->in_callback) return -1;
    if (reg < 4) chip->host_to_sound[reg] = data;
    else if (reg == 7 && chip->interrupt_enabled) {
        chip->interrupt_pending = 1;
        interrupt(chip, 1);
    }
    return 0;
}

int k056800_sound_write(K056800 *chip, uint32_t offset, uint8_t data)
{
    unsigned reg = offset & 7;
    if (!chip || chip->in_callback) return -1;
    if (reg < 2) chip->sound_to_host[reg] = data;
    else if (reg == 4) {
        chip->interrupt_enabled = data & 1;
        if (chip->interrupt_enabled) {
            if (chip->interrupt_pending) interrupt(chip, 1);
        } else {
            chip->interrupt_pending = 0;
            interrupt(chip, 0);
        }
    }
    return 0;
}
