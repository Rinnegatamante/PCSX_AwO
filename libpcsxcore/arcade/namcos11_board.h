// license:BSD-3-Clause
// copyright-holders:smf
#ifndef PCSX_ARCADE_NAMCOS11_BOARD_H
#define PCSX_ARCADE_NAMCOS11_BOARD_H
#include <stddef.h>
#include <stdint.h>
#include "m37710.h"
#include "at28c16.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Namcos11Board Namcos11Board;
enum { NAMCOS11_COH100, NAMCOS11_COH110 };
enum { NAMCOS11_BANK_NONE, NAMCOS11_BANK_ROM8, NAMCOS11_BANK_ROM8_64 };
enum { NAMCOS11_PLAYER4, NAMCOS11_SWITCH, NAMCOS11_PLAYER1, NAMCOS11_PLAYER2 };
typedef struct Namcos11Config {
    unsigned model, banking, keycus; /* KEYCUS: 406/409/410/411/430/431/432/442/443. */
    int lightgun;
    /* Borrowed assembled MAME regions; all stay valid for the board lifetime. */
    const uint8_t *main_rom, *bank_rom, *c76_data, *c76_bios, *wave_rom;
    size_t main_size, bank_size, c76_size, c76_bios_size, wave_size;
    void *opaque;
    uint16_t (*input)(void *, unsigned input);
    uint16_t (*analog)(void *, unsigned channel);
    uint16_t (*gun)(void *, unsigned channel); /* X1,Y1,X2,Y2 */
    void (*gun_outputs)(void *, unsigned leds, unsigned recoil);
    /* MAME returns machine().rand() for unknown KEYCUS reads. Explicit source
     * of randomness required for that fallback; absent callback faults. */
    uint16_t (*random16)(void *);
    void (*audio)(void *, const int16_t *interleaved_fl_fr_rl_rr, size_t frames);
} Namcos11Config;
typedef struct Namcos11Status {
    uint64_t cycles, samples, irq0_ticks, irq2_ticks;
    uint64_t unmapped_c76_reads, unmapped_c76_writes;
    uint64_t unknown_keycus_reads, unknown_keycus_writes;
    unsigned banks[8], bank_upper, fault;
} Namcos11Status;
Namcos11Board *namcos11_create(const Namcos11Config *config);
void namcos11_destroy(Namcos11Board *board);
/* RAM and EEPROM survive reset; EEPROM starts erased on creation. */
void namcos11_reset(Namcos11Board *board);
/* Physical PSX bus addresses. 1=handled, 0=delegate to core, -1=invalid/fault.
 * Width 1/2/4, naturally aligned, little-endian lanes. */
int namcos11_read(Namcos11Board *board, uint32_t address, unsigned width, uint32_t *value);
int namcos11_write(Namcos11Board *board, uint32_t address, unsigned width, uint32_t value);
/* Units are C76 CPU cycles (16.9344 MHz crystal / 2). */
void namcos11_run_cycles(Namcos11Board *board, uint64_t cycles);
void namcos11_get_status(const Namcos11Board *board, Namcos11Status *status);
M37710 *namcos11_c76(Namcos11Board *board);
unsigned namcos11_gpu_model(const Namcos11Board *board);
/* Persistent image includes ID storage: exactly AT28C16_IMAGE_BYTES (2080). */
int namcos11_eeprom_import(Namcos11Board *board, const uint8_t *data, size_t size);
int namcos11_eeprom_export(const Namcos11Board *board, uint8_t *data, size_t size);
#ifdef __cplusplus
}
#endif
#endif
