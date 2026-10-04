/* SPDX-License-Identifier: BSD-3-Clause
 * MAME MB3773 watchdog adapter, copyright smf. */
#ifndef PCSX_ARCADE_MB3773_H
#define PCSX_ARCADE_MB3773_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MB3773 MB3773;
typedef struct MB3773Config {
    void *opaque;
    /* Schedule a soft machine reset after the current bus/scheduler call.
     * Do not synchronously reset/reenter or destroy this device here. */
    void (*request_soft_reset)(void *opaque);
} MB3773Config;
enum { MB3773_OK = 0, MB3773_ARGUMENT = -1, MB3773_TIME = -2, MB3773_NOMEM = -3 };
/* Creation represents MAME start + initial reset: CK low, deadline 5 s. */
int mb3773_create(const MB3773Config *config, MB3773 **out);
void mb3773_destroy(MB3773 *device);
/* Rearm five seconds from current time. CK is retained across reset. */
int mb3773_reset(MB3773 *device);
/* Bring the clock to the bus timestamp before writing CK. Falling edges
 * alone rearm; values must be 0 or 1. */
int mb3773_write_ck(MB3773 *device, int state);
int mb3773_advance_to_us(MB3773 *device, uint64_t now_us);
uint64_t mb3773_elapsed_us(const MB3773 *device);
uint64_t mb3773_reset_requests(const MB3773 *device);
int mb3773_ck(const MB3773 *device);
int mb3773_armed(const MB3773 *device);
#ifdef __cplusplus
}
#endif
#endif
