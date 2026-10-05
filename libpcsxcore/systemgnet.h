/* GPL-2.0-or-later. Native G-Net host integration. */
#ifndef PCSX_SYSTEMGNET_H
#define PCSX_SYSTEMGNET_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef void (*systemgnet_audio_frame_fn)(void *opaque, float left, float right,
                                           uint64_t zsg_cycle);

/* Loads a strict source-profile package. board.ini selects one of the 41
 * pinned MAME taitogn sets; game sets also resolve BIOS option and verified
 * CHD/PC Card media from the generated registry. BIOS-root packages remain
 * media-free. */
int systemgnet_load(const char *directory);
/* Test-only in-memory BIOS fixture; never accepted by the production loader. */
int systemgnet_test_load(void);
void systemgnet_unload(void);
int systemgnet_active(void);
/* Install an optional Zoom PCM sink. The sink is disabled by default and is
 * retained across board reset, unload, and subsequent loads until replaced.
 * Frames are stereo normalized floats with a ZSG timestamp in its 25 MHz
 * clock domain. Calls are synchronous from systemgnet_sync(), including calls
 * made while routing CPU MMIO; keep the callback bounded and do not re-enter
 * or destroy the board. Timestamps are monotonic within a ZSG reset epoch and
 * may restart when the Zoom sound devices reset. The client owns opaque and
 * must unregister with NULL before freeing it. */
void systemgnet_set_audio_output(systemgnet_audio_frame_fn callback, void *opaque);
void systemgnet_mem_reset(void);
void systemgnet_reset(void);
int systemgnet_sync(uint32_t psx_cycle);
int systemgnet_read(uint32_t address, unsigned width, uint32_t *value);
int systemgnet_write(uint32_t address, unsigned width, uint32_t value);
int systemgnet_set_input(const char *control, int pressed);
int systemgnet_eeprom_import(const unsigned char *image, size_t size);
int systemgnet_eeprom_export(unsigned char *image, size_t size);
int systemgnet_selftest(void);
void systemgnet_report(FILE *out);

#endif
