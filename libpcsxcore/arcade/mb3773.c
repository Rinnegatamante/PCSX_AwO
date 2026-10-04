/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright smf. Based on MAME src/devices/machine/mb3773.cpp at
 * deb2f54fe7300e7a192690318beb860159bfd52a. Full terms: mb3773_LICENSE.txt.
 */
#include "mb3773.h"
#include <stdlib.h>

#define TIMEOUT_US UINT64_C(5000000)
struct MB3773 {
    MB3773Config config;
    uint64_t now, deadline, requests;
    int ck, armed;
};
int mb3773_create(const MB3773Config *config, MB3773 **out)
{
    MB3773 *device;
    if (!out) return MB3773_ARGUMENT;
    *out = NULL;
    if (!config) return MB3773_ARGUMENT;
    device = calloc(1, sizeof(*device));
    if (!device) return MB3773_NOMEM;
    device->config = *config;
    device->deadline = TIMEOUT_US;
    device->armed = 1;
    *out = device;
    return MB3773_OK;
}
void mb3773_destroy(MB3773 *device) { free(device); }
int mb3773_reset(MB3773 *device)
{
    if (!device) return MB3773_ARGUMENT;
    if (device->now > UINT64_MAX - TIMEOUT_US) return MB3773_TIME;
    device->deadline = device->now + TIMEOUT_US;
    device->armed = 1;
    return MB3773_OK;
}
int mb3773_write_ck(MB3773 *device, int state)
{
    int result;
    if (!device || (state != 0 && state != 1)) return MB3773_ARGUMENT;
    if (!state && device->ck) {
        result = mb3773_reset(device);
        if (result) return result;
    }
    device->ck = state;
    return MB3773_OK;
}
int mb3773_advance_to_us(MB3773 *device, uint64_t now_us)
{
    if (!device) return MB3773_ARGUMENT;
    if (now_us < device->now) return MB3773_TIME;
    if (device->armed && now_us >= device->deadline) {
        device->now = device->deadline;
        device->armed = 0; /* Upstream timer is one-shot. */
        device->requests++;
        if (device->config.request_soft_reset)
            device->config.request_soft_reset(device->config.opaque);
    }
    device->now = now_us;
    return MB3773_OK;
}
uint64_t mb3773_elapsed_us(const MB3773 *device) { return device ? device->now : 0; }
uint64_t mb3773_reset_requests(const MB3773 *device) { return device ? device->requests : 0; }
int mb3773_ck(const MB3773 *device) { return device ? device->ck : 0; }
int mb3773_armed(const MB3773 *device) { return device ? device->armed : 0; }
