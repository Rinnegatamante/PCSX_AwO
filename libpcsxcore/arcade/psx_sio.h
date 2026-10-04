/* BSD-3-Clause; derived from MAME's PlayStation SIO, copyright smf 2003-2011. */
#ifndef PCSX_ARCADE_PSX_SIO_H
#define PCSX_ARCADE_PSX_SIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psx_sio psx_sio_t;

typedef enum psx_sio_variant {
	PSX_SIO0 = 0,
	PSX_SIO1 = 1
} psx_sio_variant_t;

typedef enum psx_sio_result {
	PSX_SIO_OK = 0,
	PSX_SIO_ARGUMENT = -1,
	PSX_SIO_TIME_REWIND = -2,
	PSX_SIO_TIME_OVERFLOW = -3,
	PSX_SIO_NOMEM = -4
} psx_sio_result_t;

#define PSX_SIO_STATUS_TX_RDY   (1u << 0)
#define PSX_SIO_STATUS_RX_RDY   (1u << 1)
#define PSX_SIO_STATUS_TX_EMPTY (1u << 2)
#define PSX_SIO_STATUS_OVERRUN  (1u << 4)
#define PSX_SIO_STATUS_DSR      (1u << 7)
#define PSX_SIO_STATUS_IRQ      (1u << 9)

#define PSX_SIO_CONTROL_TX_ENA    (1u << 0)
#define PSX_SIO_CONTROL_IACK      (1u << 4)
#define PSX_SIO_CONTROL_RESET     (1u << 6)
#define PSX_SIO_CONTROL_TX_IENA   (1u << 10)
#define PSX_SIO_CONTROL_RX_IENA   (1u << 11)
#define PSX_SIO_CONTROL_DSR_IENA  (1u << 12)
#define PSX_SIO_CONTROL_DTR       (1u << 13)

typedef struct psx_sio_callbacks {
	void (*irq)(void *opaque, int state);
	void (*sck)(void *opaque, int state);
	void (*txd)(void *opaque, int state);
	void (*dtr)(void *opaque, int state);
	void (*rts)(void *opaque, int state);
	void *opaque;
} psx_sio_callbacks_t;

/* Construct one of the two source variants. The upstream timer uses a fixed
 * 33.8688 MHz PSX clock; the MAME constructor clock argument is not consulted.
 * Outputs are not driven during construction, matching device_start(). */
psx_sio_result_t psx_sio_create(psx_sio_variant_t variant,
		const psx_sio_callbacks_t *callbacks, psx_sio_t **out_sio);
void psx_sio_destroy(psx_sio_t *sio);

/* `offset` is MAME's 32-bit word offset from the SIO register base; offsets
 * 0..3 select TX/RX, status, mode/control, and baud, then repeat modulo 4.
 * `mem_mask` uses MAME semantics: one bits are accessed/written. The caller
 * advances the device to each bus transaction's PSX cycle before read/write. */
psx_sio_result_t psx_sio_read(psx_sio_t *sio, uint32_t offset,
		uint32_t mem_mask, uint32_t *value);
psx_sio_result_t psx_sio_write(psx_sio_t *sio, uint32_t offset,
		uint32_t value, uint32_t mem_mask);

/* External input pins. Values are pin logic levels, as in MAME's write_rxd / write_dsr. */
void psx_sio_write_rxd(psx_sio_t *sio, int state);
void psx_sio_write_dsr(psx_sio_t *sio, int state);

/* Run all one-shot bit timers due at or before this absolute PSX cycle. */
psx_sio_result_t psx_sio_advance_to_cycle(psx_sio_t *sio, uint64_t cycle);
/* Absolute emulated PSX cycle at the current callback/event boundary. While
 * advance_to_cycle catches up, callbacks observe each due deadline, not its
 * later target cycle. */
uint64_t psx_sio_elapsed_cycles(const psx_sio_t *sio);

#ifdef __cplusplus
}
#endif

#endif
