/*
 * Standalone 93C46 16-bit serial EEPROM model.
 * Derived from MAME eepromser.cpp/eeprom.cpp (BSD-3-Clause; see
 * eeprom93c46_LICENSE.txt), by Aaron Giles.
 */
#ifndef PCSX_ARCADE_EEPROM93C46_H
#define PCSX_ARCADE_EEPROM93C46_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EEPROM93C46_WORDS = 64,
    EEPROM93C46_WORD_BYTES = 2,
    EEPROM93C46_IMAGE_BYTES = EEPROM93C46_WORDS * EEPROM93C46_WORD_BYTES,
    EEPROM93C46_WRITE_US = 1750,
    EEPROM93C46_WRITE_ALL_US = 8000,
    EEPROM93C46_ERASE_US = 1000,
    EEPROM93C46_ERASE_ALL_US = 8000
};

#define EEPROM93C46_WRITE_NS       (UINT64_C(1750000))
#define EEPROM93C46_WRITE_ALL_NS   (UINT64_C(8000000))
#define EEPROM93C46_ERASE_NS       (UINT64_C(1000000))
#define EEPROM93C46_ERASE_ALL_NS   (UINT64_C(8000000))

typedef struct EEPROM93C46 EEPROM93C46;

/* NULL/0 creates an erased chip; otherwise require an exact 128-byte image.
 * Image words are little-endian, as in MAME's 16-bit eeprom_base storage. */
EEPROM93C46 *eeprom93c46_create(const uint8_t *image, size_t image_size);
void eeprom93c46_destroy(EEPROM93C46 *chip);

/* Advance absolute emulated time before each pin transition. Rewind is
 * rejected (-1) without changing the current time. */
int eeprom93c46_advance_to_us(EEPROM93C46 *chip, uint64_t now_us);
uint64_t eeprom93c46_now_us(const EEPROM93C46 *chip);
/* Nanosecond API retains MAME's distinct line edges and exact time ordering. */
int eeprom93c46_advance_to_ns(EEPROM93C46 *chip, uint64_t now_ns);
uint64_t eeprom93c46_now_ns(const EEPROM93C46 *chip);

/* Active-high CS, CLK and DI. Pin writes are edges only when the normalized
 * line value changes. Advance with either time API before changing lines. */
/* Pin writes return -1 for a null device. A rising CLK that completes a
 * write/erase whose deadline overflows uint64 nanoseconds returns -2 and
 * performs no memory mutation. */
int eeprom93c46_set_cs(EEPROM93C46 *chip, int state);
int eeprom93c46_set_clk(EEPROM93C46 *chip, int state);
int eeprom93c46_set_di(EEPROM93C46 *chip, int state);
int eeprom93c46_do(const EEPROM93C46 *chip);
int eeprom93c46_busy(const EEPROM93C46 *chip);

/* Device reset matches eeprom_base_device::device_reset: it clears the
 * pending completion time, but preserves memory, pins and serial state. */
void eeprom93c46_reset(EEPROM93C46 *chip);

/* Exact-size image transfer. Import changes only stored memory and preserves
 * pins, protocol state, lock state and virtual time. */
int eeprom93c46_export_image(const EEPROM93C46 *chip, uint8_t *image,
                             size_t image_size);
int eeprom93c46_import_image(EEPROM93C46 *chip, const uint8_t *image,
                             size_t image_size);

#ifdef __cplusplus
}
#endif
#endif
