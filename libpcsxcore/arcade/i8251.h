/* license:BSD-3-Clause
 * copyright-holders:smf, Robbbert
 * Standalone C port of MAME's Intel 8251 USART behavior.
 */
#ifndef PCSXR_ARCADE_I8251_H
#define PCSXR_ARCADE_I8251_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct i8251 i8251_t;
typedef void (*i8251_line_cb)(void *user, int level);

typedef struct i8251_callbacks {
  void *user;
  i8251_line_cb txd, dtr, rts, rxrdy, txrdy, txempty, syndet;
} i8251_callbacks_t;

i8251_t *i8251_create(const i8251_callbacks_t *callbacks);
void i8251_destroy(i8251_t *uart);
void i8251_reset(i8251_t *uart);
uint8_t i8251_read(i8251_t *uart, unsigned offset);
void i8251_write(i8251_t *uart, unsigned offset, uint8_t data);
void i8251_write_rxd(i8251_t *uart, int level);
void i8251_write_cts(i8251_t *uart, int level);
void i8251_write_dsr(i8251_t *uart, int level);
void i8251_write_rxc(i8251_t *uart, int level);
void i8251_write_txc(i8251_t *uart, int level);
void i8251_write_syn(i8251_t *uart, int level);
/* Internal wrapper hook for MB89371 loopback; safe inside the TX callback. */
void i8251_loopback_rxd(i8251_t *uart, int level);
int i8251_txrdy(const i8251_t *uart);
int i8251_rxrdy(const i8251_t *uart);

#ifdef __cplusplus
}
#endif
#endif
