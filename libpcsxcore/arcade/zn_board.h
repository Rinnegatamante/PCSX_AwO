/*
 * Sony ZN-1/ZN-2 common board I/O, based on MAME src/mame/sony/zn.cpp.
 * MAME source revision: deb2f54fe7300e7a192690318beb860159bfd52a
 *
 * Copyright smf, R. Belmont (upstream does not specify years).
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LIBPCSXCORE_ARCADE_ZN_BOARD_H
#define LIBPCSXCORE_ARCADE_ZN_BOARD_H

#include <stddef.h>
#include <stdint.h>

#include "cat702.h"
#include "at28c16.h"
#include "zn_mcu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZN_BOARD_EEPROM_SIZE 0x800u

typedef enum zn_board_model {
	ZN_BOARD_ZN1 = 1,
	ZN_BOARD_ZN2 = 2
} zn_board_model_t;

typedef enum zn_board_input {
	ZN_INPUT_P1,
	ZN_INPUT_P2,
	ZN_INPUT_SERVICE,
	ZN_INPUT_SYSTEM,
	ZN_INPUT_P3,
	ZN_INPUT_P4
} zn_board_input_t;

typedef enum zn_board_result {
	ZN_BOARD_UNMAPPED = 0,
	ZN_BOARD_HANDLED = 1
} zn_board_result_t;

typedef struct zn_board_callbacks {
	void *opaque;
	/* Core/peripheral map delegation; return nonzero if the address was handled. */
	int (*read_external)(void *opaque, uint32_t address, uint8_t *value);
	int (*write_external)(void *opaque, uint32_t address, uint8_t value);
	/* Active-low raw input bytes from MAME's P1/P2/SERVICE/SYSTEM/P3/P4 ports. */
	uint8_t (*read_input)(void *opaque, zn_board_input_t input);
	/* Board's ZNMCU mux outputs and CAT702 serial data output. */
	void (*mcu_select)(void *opaque, int selected);
	void (*analog_select)(void *opaque, int selected);
	void (*trackball_select)(void *opaque, int selected);
	void (*cat702_dataout)(void *opaque, unsigned chip, int state);
} zn_board_callbacks_t;

typedef struct zn_board_config {
	zn_board_model_t model;
	/* MAME defaults main RAM to 4 MiB; accepted boardconfig values are 4/8/16 MiB. */
	size_t ram_bytes;
	/* MAME ZN-1 supports 1 or 2 MiB; ZN-2 source config uses 2 MiB. */
	size_t vram_bytes;
	/* Daughterboard-provided CAT702s, indexed as in MAME's cat702_0/1. */
	unsigned cat702_count;
	const uint8_t (*cat702_keys)[8];
	/* Copied initial AT28C16 image. NULL/0 starts erased; size 0 with a
	 * non-NULL image means legacy 2048 bytes; 2080 also includes ID storage. */
	const uint8_t *eeprom;
	size_t eeprom_size;
	/* Optional borrowed MCU. The board drives mux/select/SCK and absolute time;
	 * caller connects its DSR/TXD outputs to PSX SIO and retains ownership. */
	ZNMCU *mcu;
} zn_board_config_t;

typedef struct zn_board {
	zn_board_model_t model;
	size_t ram_bytes;
	size_t vram_bytes;
	uint8_t coin;
	uint8_t znsecsel;
	uint8_t zn2_spu_hack;
	AT28C16 *eeprom;
	ZNMCU *mcu;
	unsigned cat702_count;
	cat702_t cat702[2];
	zn_board_callbacks_t callbacks;
} zn_board_t;

/* Initializes board-level devices only. It does not map a BIOS, RAM, scratchpad,
 * GPU, SPU, or a game-specific daughterboard. */
int zn_board_init(zn_board_t *board, const zn_board_config_t *config,
		const zn_board_callbacks_t *callbacks);
/* Initialization requires fresh storage or a board already destroyed. */
void zn_board_destroy(zn_board_t *board);
void zn_board_reset(zn_board_t *board);
/* Absolute emulated microseconds for EEPROM and the connected MCU. */
int zn_board_advance_to_us(zn_board_t *board, uint64_t now_us);
int zn_board_eeprom_import(zn_board_t *board, const uint8_t *image, size_t size);
int zn_board_eeprom_export(const zn_board_t *board, uint8_t *image, size_t size);

/* Byte-addressed PSX physical bus. Unrecognized addresses fail closed, unless
 * the host's external-map callback explicitly handles them. */
zn_board_result_t zn_board_read8(zn_board_t *board, uint32_t address, uint8_t *value);
zn_board_result_t zn_board_write8(zn_board_t *board, uint32_t address, uint8_t value);
/* Width-aware adapter for naturally aligned PSX reads/writes (width 1, 2, 4).
 * It preserves one side effect for a native 16-bit ZN-2 workaround read. */
zn_board_result_t zn_board_read(zn_board_t *board, uint32_t address,
		unsigned width, uint32_t *value);
zn_board_result_t zn_board_write(zn_board_t *board, uint32_t address,
		unsigned width, uint32_t value);

/* Drive the PSX SIO0 shared clock and TX data wires into populated CAT702s. */
/* Clock returns 0 on success or the connected MCU's timer error. */
int zn_board_sio_clock(zn_board_t *board, int state);
void zn_board_sio_tx(zn_board_t *board, int state);

#ifdef __cplusplus
}
#endif

#endif
