/* SPDX-License-Identifier: BSD-3-Clause
 * copyright-holders:smf
 */
#ifndef PCSX_ARCADE_ZN_MCU_H
#define PCSX_ARCADE_ZN_MCU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZNMCU ZNMCU;

enum {
    ZN_MCU_OK = 0,
    ZN_MCU_ARGUMENT = -1,
    ZN_MCU_TIME_REWIND = -2,
    ZN_MCU_TIME_OVERFLOW = -3,
    ZN_MCU_NOMEM = -4
};

typedef struct ZNMCUConfig {
    uint8_t (*analog[8])(void *opaque);
    uint16_t (*trackball[4])(void *opaque);
    uint8_t (*dsw)(void *opaque);
    void (*dsr)(void *opaque, int state);
    void (*txd)(void *opaque, int state);
    void *opaque;
} ZNMCUConfig;

/* Callbacks omitted from the config use the MAME device's defaults:
 * analog=0xff, trackball=0, DSW=0xff, and disconnected output lines. */
int zn_mcu_create(const ZNMCUConfig *config, ZNMCU **out_mcu);
void zn_mcu_destroy(ZNMCU *mcu);

/* Input selector pins. Values are treated as logic levels. */
int zn_mcu_select(ZNMCU *mcu, int state);
int zn_mcu_sck(ZNMCU *mcu, int state);
void zn_mcu_analog_read(ZNMCU *mcu, int state);
void zn_mcu_trackball_read(ZNMCU *mcu, int state);

/* Advance absolute emulated microseconds. Calls must be monotonic. Timer
 * callbacks scheduled by select/SCK occur at their exact 50/5 us deadlines. */
int zn_mcu_advance_to_us(ZNMCU *mcu, uint64_t now_us);

#ifdef __cplusplus
}
#endif
#endif
