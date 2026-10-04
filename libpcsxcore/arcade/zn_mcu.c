/* SPDX-License-Identifier: BSD-3-Clause
 * copyright-holders:smf
 *
 * Standalone C99 adaptation of MAME src/mame/sony/znmcu.cpp/.h at the pinned
 * MAME revision documented in docs/zn-mcu.md.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "zn_mcu.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    ZN_MCU_SEND_BYTES = 9,
    ZN_MCU_INITIAL_DELAY_US = 50,
    ZN_MCU_DSR_PULSE_US = 5
};

struct ZNMCU {
    ZNMCUConfig config;
    int sck;
    int select;
    int bit;
    int byte;
    int databytes;
    int analog_read;
    int trackball_read;
    uint16_t trackball_previous[4];
    uint8_t send[ZN_MCU_SEND_BYTES];
    uint64_t now_us;
    uint64_t timer_deadline_us;
    int timer_active;
    int timer_param;
};

static uint8_t analog_value(const ZNMCU *mcu, unsigned index)
{
    return mcu->config.analog[index]
        ? mcu->config.analog[index](mcu->config.opaque) : 0xff;
}

static uint16_t trackball_value(const ZNMCU *mcu, unsigned index)
{
    return mcu->config.trackball[index]
        ? mcu->config.trackball[index](mcu->config.opaque) : 0;
}

static uint8_t dsw_value(const ZNMCU *mcu)
{
    return mcu->config.dsw ? mcu->config.dsw(mcu->config.opaque) : 0xff;
}

static int schedule_timer(ZNMCU *mcu, uint64_t delay_us, int param)
{
    if (mcu->now_us > UINT64_MAX - delay_us)
        return ZN_MCU_TIME_OVERFLOW;
    mcu->timer_deadline_us = mcu->now_us + delay_us;
    mcu->timer_param = param;
    mcu->timer_active = 1;
    return ZN_MCU_OK;
}

static int timer_callback(ZNMCU *mcu, int param)
{
    if (mcu->config.dsr)
        mcu->config.dsr(mcu->config.opaque, param);

    if (!param) {
        if (mcu->byte == 0) {
            mcu->databytes = mcu->analog_read ? 8 : mcu->trackball_read ? 6 : 1;
            mcu->send[0] = (uint8_t)((mcu->databytes << 4) | (dsw_value(mcu) & 0x0f));

            if (mcu->analog_read) {
                for (int i = 0; i < 8; i++)
                    mcu->send[1 + i] = analog_value(mcu, (unsigned)i);
            } else if (mcu->trackball_read) {
                uint16_t delta[4];
                for (int i = 0; i < 4; i++) {
                    uint16_t current = trackball_value(mcu, (unsigned)i);
                    delta[i] = (uint16_t)(current - mcu->trackball_previous[i]);
                    mcu->trackball_previous[i] = current;
                }

                mcu->send[1] = (uint8_t)delta[0];
                mcu->send[2] = (uint8_t)(((delta[0] & 0x0f00) >> 8) |
                                          ((delta[1] & 0x0f00) >> 4));
                mcu->send[3] = (uint8_t)delta[1];
                mcu->send[4] = (uint8_t)delta[2];
                mcu->send[5] = (uint8_t)(((delta[2] & 0x0f00) >> 8) |
                                          ((delta[3] & 0x0f00) >> 4));
                mcu->send[6] = (uint8_t)delta[3];
            }
        }
        return schedule_timer(mcu, ZN_MCU_DSR_PULSE_US, 1);
    }
    return ZN_MCU_OK;
}

int zn_mcu_create(const ZNMCUConfig *config, ZNMCU **out_mcu)
{
    ZNMCU *mcu;
    if (out_mcu)
        *out_mcu = NULL;
    if (!out_mcu)
        return ZN_MCU_ARGUMENT;
    mcu = (ZNMCU *)calloc(1, sizeof(*mcu));
    if (!mcu)
        return ZN_MCU_NOMEM;
    if (config)
        mcu->config = *config;
    mcu->sck = 1;
    mcu->select = 1;
    memset(mcu->send, 0, sizeof(mcu->send));
    memset(mcu->trackball_previous, 0, sizeof(mcu->trackball_previous));
    if (mcu->config.dsr)
        mcu->config.dsr(mcu->config.opaque, 1);
    if (mcu->config.txd)
        mcu->config.txd(mcu->config.opaque, 1);
    *out_mcu = mcu;
    return ZN_MCU_OK;
}

void zn_mcu_destroy(ZNMCU *mcu)
{
    free(mcu);
}

int zn_mcu_select(ZNMCU *mcu, int state)
{
    if (!mcu)
        return ZN_MCU_ARGUMENT;
    state = !!state;
    if (mcu->select != state) {
        int result = ZN_MCU_OK;
        if (!state) {
            mcu->bit = 0;
            mcu->byte = 0;
            result = schedule_timer(mcu, ZN_MCU_INITIAL_DELAY_US, 0);
        } else {
            if (mcu->config.txd)
                mcu->config.txd(mcu->config.opaque, 1);
            if (mcu->config.dsr)
                mcu->config.dsr(mcu->config.opaque, 1);
            mcu->timer_active = 0;
        }
        mcu->select = state;
        return result;
    }
    return ZN_MCU_OK;
}

int zn_mcu_sck(ZNMCU *mcu, int state)
{
    if (!mcu)
        return ZN_MCU_ARGUMENT;
    state = !!state;
    if (mcu->sck != state) {
        int result = ZN_MCU_OK;
        if (!state && !mcu->select) {
            uint8_t data = 0;
            if (mcu->byte <= mcu->databytes && mcu->byte < ZN_MCU_SEND_BYTES)
                data = mcu->send[mcu->byte];
            if (mcu->config.txd)
                mcu->config.txd(mcu->config.opaque, (data >> mcu->bit) & 1);

            mcu->bit++;
            if (mcu->bit == 8) {
                if (mcu->byte < mcu->databytes)
                    result = schedule_timer(mcu, ZN_MCU_INITIAL_DELAY_US, 0);
                mcu->bit = 0;
                mcu->byte++;
            }
        }
        mcu->sck = state;
        return result;
    }
    return ZN_MCU_OK;
}

void zn_mcu_analog_read(ZNMCU *mcu, int state)
{
    if (mcu)
        mcu->analog_read = state;
}

void zn_mcu_trackball_read(ZNMCU *mcu, int state)
{
    if (mcu)
        mcu->trackball_read = state;
}

int zn_mcu_advance_to_us(ZNMCU *mcu, uint64_t now_us)
{
    if (!mcu)
        return ZN_MCU_ARGUMENT;
    if (now_us < mcu->now_us)
        return ZN_MCU_TIME_REWIND;

    while (mcu->timer_active && mcu->timer_deadline_us <= now_us) {
        int param = mcu->timer_param;
        mcu->now_us = mcu->timer_deadline_us;
        mcu->timer_active = 0;
        {
            int result = timer_callback(mcu, param);
            if (result)
                return result;
        }
    }
    mcu->now_us = now_us;
    return ZN_MCU_OK;
}
