/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright Ville Linde. See k056800_LICENSE.txt.
 * MAME K056800 MIRAC sound interface, standalone C adaptation. */
#ifndef PCSX_ARCADE_K056800_H
#define PCSX_ARCADE_K056800_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct K056800 {
    uint8_t host_to_sound[4], sound_to_host[2];
    uint8_t interrupt_pending, interrupt_enabled, interrupt_line, in_callback;
    void (*interrupt)(void *opaque, int asserted);
    void *opaque;
} K056800;

/* Fresh constructor state, no interrupt callback. Caller owns this storage. */
int k056800_init(K056800 *chip, void (*interrupt)(void *, int), void *opaque);
/* Source reset clears registers/pending/enable without driving the IRQ pin. */
int k056800_reset(K056800 *chip);
uint8_t k056800_host_read(const K056800 *chip, uint32_t offset);
uint8_t k056800_sound_read(const K056800 *chip, uint32_t offset);
/* Offsets mirror through offset & 7. Return -1 for NULL or callback reentry.
 * Source volume/mute and unknown writes are no-ops. No timers in this device. */
int k056800_host_write(K056800 *chip, uint32_t offset, uint8_t data);
int k056800_sound_write(K056800 *chip, uint32_t offset, uint8_t data);
#ifdef __cplusplus
}
#endif
#endif
