/* Experimental COH-700 board adapter. See docs/system12.md. */
#ifndef PCSX_SYSTEM12_H
#define PCSX_SYSTEM12_H
#include <stdio.h>
#include <stddef.h>
#include "psxcommon.h"

int system12_load(const char *directory);
void system12_unload(void);
int system12_active(void);
void system12_mem_reset(void);
void system12_reset(void);
void system12_sync(u32 psx_cycle);
void system12_vblank(int level);
int system12_set_input(const char *control, int pressed);
/* Exact 2080-byte AT28C16 data+ID image import/export; board must be active. */
int system12_eeprom_export(unsigned char *image, size_t size);
int system12_eeprom_import(const unsigned char *image, size_t size);
int system12_open_audio(const char *path);
int system12_close_audio(void);
unsigned char system12_jamma_read(unsigned offset);
void system12_audio_test_fixture(void);
int system12_read(u32 address, unsigned width, u32 *value);
int system12_write(u32 address, unsigned width, u32 value);
int system12_selftest(void);
void system12_dma5(u32 madr, u32 bcr, u32 chcr);
void system12_report(FILE *out);
#endif
