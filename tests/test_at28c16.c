/* Synthetic AT28C16 timelines, independent of game NVRAM. */
#include "at28c16.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void initialization(void) {
    uint8_t data[AT28C16_DATA_BYTES], image[AT28C16_IMAGE_BYTES], out[AT28C16_IMAGE_BYTES];
    AT28C16 *chip;
    unsigned i;
    for (i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 17 + 3);
    assert(!at28c16_create(NULL, 1));
    assert(!at28c16_create(data, 0));
    assert(!at28c16_create(data, AT28C16_DATA_BYTES - 1));
    assert(!at28c16_create(data, AT28C16_IMAGE_BYTES + 1));
    chip = at28c16_create(NULL, 0);
    assert(chip);
    assert(at28c16_get_image(chip, out, sizeof(out)) == 0);
    for (i = 0; i < sizeof(out); i++)
        assert(out[i] == 0xff);
    assert(!at28c16_busy(chip));
    at28c16_destroy(chip);
    chip = at28c16_create(data, sizeof(data));
    assert(chip);
    data[0] ^= 0xff; /* Caller buffer is copied. */
    assert(at28c16_read(chip, 0) == 3);
    assert(at28c16_get_image(chip, out, sizeof(out)) == 0);
    assert(out[AT28C16_DATA_BYTES] == 0xff && out[AT28C16_IMAGE_BYTES - 1] == 0xff);
    assert(at28c16_get_image(chip, out, sizeof(out) - 1) == -1);
    assert(at28c16_get_image(chip, NULL, sizeof(out)) == -1);
    at28c16_destroy(chip);
    for (i = 0; i < sizeof(image); i++)
        image[i] = (uint8_t)i;
    chip = at28c16_create(image, sizeof(image));
    assert(chip);
    assert(at28c16_read(chip, 0x800) == 0 && at28c16_read(chip, 0x81f) == 0x1f);
    assert(at28c16_get_image(chip, out, sizeof(out)) == 0 && !memcmp(out, image, sizeof(out)));
    at28c16_destroy(chip);
    puts("AT28C16 erased/default/full image validation and copied storage passed");
}

static void timeline(void) {
    AT28C16 *chip = at28c16_create(NULL, 0);
    uint8_t image[AT28C16_IMAGE_BYTES];
    AT28C16State state;
    assert(chip);
    assert(at28c16_advance_to_us(chip, 100) == 0);
    assert(at28c16_write(chip, 0x123, 0xff) == 0 && !at28c16_busy(chip));
    assert(at28c16_write(chip, 0x123, 0x35) == 1);
    assert(at28c16_get_image(chip, image, sizeof(image)) == 0 && image[0x123] == 0x35);
    assert(at28c16_read(chip, 0) == 0xb5 && at28c16_read(chip, 0x123) == 0xb5);
    assert(at28c16_read(chip, 0x800) == 0xb5 && at28c16_read(chip, UINT32_MAX) == 0xb5);
    assert(at28c16_write(chip, 0x124, 0x44) == 0);
    assert(at28c16_write(chip, 0x123, 0x66) == 0);
    assert(at28c16_advance_to_us(chip, 299) == 0 && at28c16_busy(chip));
    assert(at28c16_get_state(chip, &state) == 0 && state.busy_until_us == 300);
    assert(at28c16_advance_to_us(chip, 298) == -1);
    assert(at28c16_read(chip, 0) == 0xb5);
    assert(at28c16_advance_to_us(chip, 300) == 0 && !at28c16_busy(chip));
    assert(at28c16_read(chip, 0x123) == 0x35 && at28c16_read(chip, 0x124) == 0xff);
    assert(at28c16_write(chip, 0x123, 0x35) == 0 && !at28c16_busy(chip));
    assert(at28c16_write(chip, 0x124, 0x80) == 1 && at28c16_read(chip, 0) == 0);
    assert(at28c16_advance_to_us(chip, 500) == 0 && at28c16_read(chip, 0x124) == 0x80);
    assert(at28c16_read(chip, 0x820) == 0xff && at28c16_write(chip, 0x820, 0) == -1);
    assert(at28c16_advance_to_us(chip, UINT64_MAX - 199) == 0);
    assert(at28c16_write(chip, 0x125, 0x55) == -1 && !at28c16_busy(chip));
    assert(at28c16_read(chip, 0x125) == 0xff);
    assert(at28c16_write(chip, 0x123, 0x35) == 0); /* No event to overflow. */
    at28c16_destroy(chip);
    puts("AT28C16 exact 200us busy/global D7 polling/ignored writes/monotonic clock passed");
}

static void high_voltage(void) {
    AT28C16 *chip = at28c16_create(NULL, 0);
    uint8_t image[AT28C16_IMAGE_BYTES];
    unsigned i;
    assert(chip);
    assert(at28c16_write(chip, 0x7e0, 0x12) == 1);
    assert(at28c16_advance_to_us(chip, 200) == 0);
    at28c16_set_a9_12v(chip, 1);
    assert(at28c16_read(chip, 0x7e0) == 0xff); /* Logical 7e0 redirects to physical 800. */
    assert(at28c16_write(chip, 0x7e0, 0x34) == 1);
    at28c16_set_a9_12v(chip, 0);
    assert(at28c16_read(chip, 0x7e0) == 0xb4); /* Pin changes cannot defeat global busy. */
    assert(at28c16_advance_to_us(chip, 400) == 0);
    assert(at28c16_read(chip, 0x7e0) == 0x12);
    at28c16_set_a9_12v(chip, 3);
    assert(at28c16_read(chip, 0x7e0) == 0x34);
    assert(at28c16_write(chip, 0x7ff, 0x56) == 1);
    assert(at28c16_advance_to_us(chip, 600) == 0);
    assert(at28c16_read(chip, 0x7ff) == 0x56 && at28c16_read(chip, 0x7df) == 0xff);
    assert(at28c16_get_image(chip, image, sizeof(image)) == 0);
    assert(image[0x7e0] == 0x12 && image[0x800] == 0x34 && image[0x81f] == 0x56);
    at28c16_set_a9_12v(chip, 2);
    assert(at28c16_read(chip, 0x7e0) == 0x12);
    assert(at28c16_write(chip, 7, 0x99) == 1);
    at28c16_set_oe_12v(chip, 1);
    assert(at28c16_write(chip, 0, 0) == 0); /* Busy erase ignored. */
    assert(at28c16_advance_to_us(chip, 800) == 0 && at28c16_read(chip, 7) == 0x99);
    assert(at28c16_write(chip, UINT32_MAX, 0x55) == 1); /* OE erase ignores address/data. */
    assert(at28c16_read(chip, 0) == 0x7f && at28c16_read(chip, 0x7ff) == 0x7f);
    assert(at28c16_get_image(chip, image, sizeof(image)) == 0);
    for (i = 0; i < sizeof(image); i++)
        assert(image[i] == 0xff);
    assert(at28c16_advance_to_us(chip, 999) == 0 && at28c16_read(chip, 7) == 0x7f);
    assert(at28c16_advance_to_us(chip, 1000) == 0 && at28c16_read(chip, 7) == 0xff);
    assert(at28c16_write(chip, 0, 0xff) == 1); /* Already-erased erase still has a busy interval. */
    at28c16_set_oe_12v(chip, 2);
    assert(at28c16_advance_to_us(chip, 1200) == 0);
    assert(at28c16_write(chip, 0, 0xff) == 0 && !at28c16_busy(chip));
    at28c16_destroy(chip);
    puts("AT28C16 high-voltage ID redirection/pin low-bit rules/OE full erase passed");
}

static void nvram_snapshot(void) {
    uint8_t image[AT28C16_IMAGE_BYTES], out[AT28C16_IMAGE_BYTES];
    AT28C16 *chip = at28c16_create(NULL, 0), *restored = at28c16_create(NULL, 0);
    AT28C16State state, bad, after;
    unsigned i;
    assert(chip && restored);
    for (i = 0; i < sizeof(image); i++)
        image[i] = (uint8_t)(i * 23 + 7);
    at28c16_set_a9_12v(chip, 1);
    assert(at28c16_advance_to_us(chip, 1000) == 0);
    assert(at28c16_write(chip, 0x7e1, 0x67) == 1);
    assert(at28c16_import_image(chip, image, sizeof(image)) == 0);
    assert(at28c16_read(chip, 0) == 0xe7 &&
           at28c16_busy(chip)); /* NVRAM import does not reset write/pins. */
    assert(at28c16_get_image(chip, out, sizeof(out)) == 0 && !memcmp(out, image, sizeof(out)));
    assert(at28c16_import_image(chip, image, sizeof(image) - 1) == -1);
    assert(at28c16_import_image(chip, NULL, sizeof(image)) == -1);
    assert(at28c16_advance_to_us(chip, 1100) == 0);
    assert(at28c16_get_state(chip, &state) == 0 && state.now_us == 1100 &&
           state.busy_until_us == 1200);
    assert(at28c16_advance_to_us(restored, 5000) == 0);
    assert(at28c16_set_state(restored, &state) ==
           0); /* Explicit save-state restore can replace time origin. */
    assert(at28c16_read(restored, 0x7e1) == 0xe7);
    assert(at28c16_advance_to_us(restored, 1199) == 0 && at28c16_busy(restored));
    assert(at28c16_advance_to_us(restored, 1200) == 0 && !at28c16_busy(restored));
    assert(at28c16_read(restored, 0x7e1) == image[0x801]);
    assert(at28c16_advance_to_us(chip, 1200) == 0);
    assert(at28c16_get_state(chip, &state) == 0 && state.last_write == -1 && !state.busy_until_us);
    assert(at28c16_set_state(restored, &state) == 0);
    bad = state;
    bad.last_write = -2;
    assert(at28c16_set_state(restored, &bad) == -1);
    bad = state;
    bad.last_write = 256;
    assert(at28c16_set_state(restored, &bad) == -1);
    bad = state;
    bad.a9_12v = 2;
    assert(at28c16_set_state(restored, &bad) == -1);
    bad = state;
    bad.oe_12v = 2;
    assert(at28c16_set_state(restored, &bad) == -1);
    bad = state;
    bad.busy_until_us = 1201;
    assert(at28c16_set_state(restored, &bad) == -1);
    bad = state;
    bad.last_write = 1;
    bad.busy_until_us = bad.now_us;
    assert(at28c16_set_state(restored, &bad) == -1);
    bad = state;
    bad.last_write = 1;
    bad.busy_until_us = bad.now_us + 201;
    assert(at28c16_set_state(restored, &bad) == -1);
    assert(at28c16_get_state(restored, &after) == 0 && !memcmp(&after, &state, sizeof(state)));
    at28c16_destroy(chip);
    at28c16_destroy(restored);
    assert(at28c16_advance_to_us(NULL, 0) == -1 && at28c16_read(NULL, 0) == 0xff);
    assert(at28c16_write(NULL, 0, 0) == -1 && at28c16_get_state(NULL, &state) == -1);
    puts("AT28C16 full NVRAM import-export/in-flight snapshots/invalid-state rejection passed");
}
int main(void) {
    initialization();
    timeline();
    high_voltage();
    nvram_snapshot();
    return 0;
}
