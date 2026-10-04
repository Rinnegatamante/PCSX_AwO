/* SPDX-License-Identifier: BSD-3-Clause
 * Standalone MAME-derived Konami cassette secure flash/serial ID (copyright smf).
 */
#ifndef PCSX_KONAMI_SECURITY_H
#define PCSX_KONAMI_SECURITY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KonamiSecureFlash KonamiSecureFlash;
typedef struct KonamiDS2401 KonamiDS2401;
enum {
    KONAMI_X76F041 = 41,
    KONAMI_X76F100 = 100,
    KONAMI_X76F041_IMAGE_SIZE = 548,
    KONAMI_X76F100_IMAGE_SIZE = 132,
    KONAMI_DS2401_IMAGE_SIZE = 8
};
typedef struct KonamiSecureFlashStatus {
    unsigned type;
    int cs, rst, scl, sda_in, sda_out, state, bit, byte, command, address;
    unsigned password_retry_counter;
    int password_matched;
} KonamiSecureFlashStatus;
/* Images are REQUIRED. No zero-password/ID fallback is generated.
 * X76F100: resetresponse[4],writepassword[8],readpassword[8],data[112].
 * X76F041: response[4],writepassword[8],readpassword[8],configpassword[8],
 *          configurationregisters[8],data[512]. Mutable internal copy.
 */
KonamiSecureFlash *konami_secure_flash_create(unsigned type, const uint8_t *image, size_t size);
void konami_secure_flash_destroy(KonamiSecureFlash *flash);
/* Reset only pin/protocol state; preserves image including mutated passwords. */
void konami_secure_flash_reset(KonamiSecureFlash *flash);
void konami_secure_flash_cs(KonamiSecureFlash *flash, int level);
void konami_secure_flash_rst(KonamiSecureFlash *flash, int level);
void konami_secure_flash_scl(KonamiSecureFlash *flash, int level);
void konami_secure_flash_sda(KonamiSecureFlash *flash, int level);
/* Matches MAME's device SDA output (CS deselected returns1). */
int konami_secure_flash_read_sda(KonamiSecureFlash *flash);
int konami_secure_flash_get_image(const KonamiSecureFlash *flash, uint8_t *image, size_t size);
void konami_secure_flash_get_status(const KonamiSecureFlash *flash,
                                    KonamiSecureFlashStatus *status);
/* DS2401 image uses MAME region order: wire emits image[7]..image[0],LSB first.
 * Absolute emulated microseconds must be monotonic. Rewinds return -1 and do
 * not change state. Supplied ROM bytes are returned verbatim, including CRC.
 */
/* Optional Dallas CRC-8 check:1 valid,0 mismatch,-1 invalid image shape.
 * Constructor preserves caller bytes even when CRC mismatches. */
int konami_ds2401_validate_image(const uint8_t *image, size_t size);
KonamiDS2401 *konami_ds2401_create(const uint8_t *image, size_t size);
void konami_ds2401_destroy(KonamiDS2401 *chip);
void konami_ds2401_reset(KonamiDS2401 *chip);
int konami_ds2401_advance_to_us(KonamiDS2401 *chip, uint64_t now_us);
int konami_ds2401_write_pin(KonamiDS2401 *chip, int level, uint64_t now_us);
int konami_ds2401_read_pin(KonamiDS2401 *chip, uint64_t now_us);
int konami_ds2401_get_image(const KonamiDS2401 *chip, uint8_t *image, size_t size);
#ifdef __cplusplus
}
#endif
#endif
