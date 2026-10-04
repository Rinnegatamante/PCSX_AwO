/* Standalone AT28C16. BSD-3-Clause, copyright smf; see at28c16_LICENSE.txt. */
#ifndef PCSX_ARCADE_AT28C16_H
#define PCSX_ARCADE_AT28C16_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
    AT28C16_DATA_BYTES = 2048,
    AT28C16_ID_BYTES = 32,
    AT28C16_IMAGE_BYTES = 2080,
    AT28C16_WRITE_US = 200
};
typedef struct AT28C16 AT28C16;
typedef struct AT28C16State {
    uint8_t image[AT28C16_IMAGE_BYTES];
    uint8_t a9_12v, oe_12v;
    int16_t last_write; /* -1 idle, 0..255 busy */
    uint64_t now_us, busy_until_us;
} AT28C16State;
/* NULL/0 => erased FF; 2048 bytes => default data with erased ID; 2080 => full NVRAM. */
AT28C16 *at28c16_create(const uint8_t *image, size_t size);
void at28c16_destroy(AT28C16 *chip);
/* No reset operation: upstream device has no device_reset; keep it alive across board reset. */
/* Absolute emulated microseconds; 0 success, -1 for a rewind. */
int at28c16_advance_to_us(AT28C16 *chip, uint64_t now_us);
uint8_t at28c16_read(const AT28C16 *chip, uint32_t offset);
/* 1 changed/erase and busy started; 0 ignored/same; -1 invalid offset/time overflow/null. */
int at28c16_write(AT28C16 *chip, uint32_t offset, uint8_t value);
void at28c16_set_a9_12v(AT28C16 *chip, int state);
void at28c16_set_oe_12v(AT28C16 *chip, int state);
int at28c16_busy(const AT28C16 *chip);
/* Exact full-size NVRAM import/export. Import preserves pins and pending write timing. */
int at28c16_get_image(const AT28C16 *chip, uint8_t *image, size_t size);
int at28c16_import_image(AT28C16 *chip, const uint8_t *image, size_t size);
/* In-memory structured snapshots. Restore explicitly replaces the emulated time origin. */
int at28c16_get_state(const AT28C16 *chip, AT28C16State *state);
int at28c16_set_state(AT28C16 *chip, const AT28C16State *state);
#ifdef __cplusplus
}
#endif
#endif
