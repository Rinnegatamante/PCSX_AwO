/* GPL-2.0-or-later. Native G-Net host integration. */
#ifndef PCSX_SYSTEMGNET_H
#define PCSX_SYSTEMGNET_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int systemgnet_load(const char *directory);
/* Test-only in-memory BIOS fixture; never accepted by the production loader. */
int systemgnet_test_load(void);
void systemgnet_unload(void);
int systemgnet_active(void);
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
