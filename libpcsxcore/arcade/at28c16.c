// license:BSD-3-Clause
// copyright-holders:smf
// Standalone adaptation of MAME src/devices/machine/at28c16.cpp.
#include "at28c16.h"
#include <stdlib.h>
#include <string.h>

struct AT28C16 {
    AT28C16State state;
};

AT28C16 *at28c16_create(const uint8_t *image, size_t size) {
    AT28C16 *chip;
    if ((!image && size) || (image && size != AT28C16_DATA_BYTES && size != AT28C16_IMAGE_BYTES))
        return NULL;
    chip = (AT28C16 *)calloc(1, sizeof(*chip));
    if (!chip)
        return NULL;
    memset(chip->state.image, 0xff, sizeof(chip->state.image));
    if (size)
        memcpy(chip->state.image, image, size);
    chip->state.last_write = -1;
    return chip;
}

void at28c16_destroy(AT28C16 *chip) { free(chip); }

int at28c16_advance_to_us(AT28C16 *chip, uint64_t now_us) {
    if (!chip || now_us < chip->state.now_us)
        return -1;
    chip->state.now_us = now_us;
    if (chip->state.last_write >= 0 && now_us >= chip->state.busy_until_us) {
        chip->state.last_write = -1;
        chip->state.busy_until_us = 0;
    }
    return 0;
}

static uint32_t mapped_offset(const AT28C16 *chip, uint32_t offset) {
    if (chip->state.a9_12v && offset >= AT28C16_DATA_BYTES - AT28C16_ID_BYTES) {
        if (offset > UINT32_MAX - AT28C16_ID_BYTES)
            return UINT32_MAX;
        offset += AT28C16_ID_BYTES;
    }
    return offset;
}

uint8_t at28c16_read(const AT28C16 *chip, uint32_t offset) {
    if (!chip)
        return 0xff;
    // Polling status appears at every address, including the ID area and unused offsets.
    if (chip->state.last_write >= 0)
        return (uint8_t)(chip->state.last_write ^ 0x80);
    offset = mapped_offset(chip, offset);
    return offset < AT28C16_IMAGE_BYTES ? chip->state.image[offset] : 0xff;
}

int at28c16_write(AT28C16 *chip, uint32_t offset, uint8_t value) {
    if (!chip)
        return -1;
    if (chip->state.last_write >= 0)
        return 0;
    if (!chip->state.oe_12v) {
        offset = mapped_offset(chip, offset);
        if (offset >= AT28C16_IMAGE_BYTES)
            return -1;
        if (chip->state.image[offset] == value)
            return 0;
    }
    if (chip->state.now_us > UINT64_MAX - AT28C16_WRITE_US)
        return -1;
    if (chip->state.oe_12v) {
        // Upstream erases all 2080 bytes on any write with OE at 12V, even if already FF.
        memset(chip->state.image, 0xff, sizeof(chip->state.image));
        chip->state.last_write = 0xff;
    } else {
        chip->state.image[offset] = value;
        chip->state.last_write = value;
    }
    chip->state.busy_until_us = chip->state.now_us + AT28C16_WRITE_US;
    return 1;
}

void at28c16_set_a9_12v(AT28C16 *chip, int state) {
    if (chip)
        chip->state.a9_12v = (uint8_t)(state & 1);
}

void at28c16_set_oe_12v(AT28C16 *chip, int state) {
    if (chip)
        chip->state.oe_12v = (uint8_t)(state & 1);
}

int at28c16_busy(const AT28C16 *chip) { return chip && chip->state.last_write >= 0; }

int at28c16_get_image(const AT28C16 *chip, uint8_t *image, size_t size) {
    if (!chip || !image || size != AT28C16_IMAGE_BYTES)
        return -1;
    memcpy(image, chip->state.image, size);
    return 0;
}

int at28c16_import_image(AT28C16 *chip, const uint8_t *image, size_t size) {
    if (!chip || !image || size != AT28C16_IMAGE_BYTES)
        return -1;
    memcpy(chip->state.image, image, size);
    return 0;
}

int at28c16_get_state(const AT28C16 *chip, AT28C16State *state) {
    if (!chip || !state)
        return -1;
    *state = chip->state;
    return 0;
}

int at28c16_set_state(AT28C16 *chip, const AT28C16State *state) {
    if (!chip || !state || state->a9_12v > 1 || state->oe_12v > 1 || state->last_write < -1 ||
        state->last_write > 255)
        return -1;
    if (state->last_write < 0) {
        if (state->busy_until_us)
            return -1;
    } else if (state->busy_until_us <= state->now_us ||
               state->busy_until_us - state->now_us > AT28C16_WRITE_US) {
        return -1;
    }
    chip->state = *state;
    return 0;
}
