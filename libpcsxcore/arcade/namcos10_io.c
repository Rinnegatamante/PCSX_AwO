/* SPDX-License-Identifier: BSD-3-Clause
 * copyright-holders:smf,windyfairy
 *
 * Namco System 10 base I/O map and state transitions adapted from the pinned
 * MAME src/mame/namco/namcos10.cpp driver. The standalone interface is C99.
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
#include "namcos10_io.h"

#include <stdlib.h>
#include <string.h>

enum {
    I2C_IDLE,
    I2C_RECEIVE_BYTE,
    I2C_RECEIVE_ACK_1,
    I2C_RECEIVE_ACK_0
};

enum {
    NS10_SYSTEM = 0x1fba0000u,
    NS10_EXIO_IDENT = 0x1fba0002u,
    NS10_INPUTS = 0x1fba0004u,
    NS10_I2C_CLOCK = 0x1fba0008u,
    NS10_I2C_DATA = 0x1fba000au,
    NS10_INT_ACK = 0x1fba0012u,
    NS10_INT_STATUS = 0x1fba001au,
    NS10_DMA_NOP = 0x1fb20000u,
    NS10_WATCHDOG = 0x1fb60000u,
    NS10_TICK_US = 10000u
};

struct NS10IO {
    NS10IOConfig config;
    uint16_t i2c_host_clock, i2c_host_data;
    uint16_t i2c_dev_clock, i2c_dev_data;
    uint16_t i2c_prev_clock, i2c_prev_data;
    int8_t i2c_mode;
    uint8_t i2c_byte;
    int32_t i2c_bit;
    int32_t exio_ident_bit;
    uint32_t exio_ident_byte;
    uint16_t int_status;
    uint64_t timer_fraction_us;
    uint8_t timer_initial_tick;
};

static uint32_t physical_address(uint32_t address)
{
    if (address >= 0x80000000u && address < 0xc0000000u)
        return address & 0x1fffffffu;
    return address;
}

static uint32_t width_mask(unsigned width)
{
    return width == 1 ? 0xffu : width == 2 ? 0xffffu : 0xffffffffu;
}

static uint16_t combine(uint16_t old_value, uint16_t data, uint16_t mem_mask)
{
    return (uint16_t)((old_value & (uint16_t)~mem_mask) | (data & mem_mask));
}

static void i2c_update(NS10IO *io)
{
    uint16_t clock = io->i2c_dev_clock & io->i2c_host_clock & 1;
    uint16_t data = io->i2c_dev_data & io->i2c_host_data & 1;

    if (io->i2c_prev_data == data && io->i2c_prev_clock == clock)
        return;

    switch (io->i2c_mode) {
    case I2C_IDLE:
        if (clock && !data) {
            io->i2c_byte = 0;
            io->i2c_bit = 7;
            io->i2c_mode = I2C_RECEIVE_BYTE;
        }
        break;
    case I2C_RECEIVE_BYTE:
        if (clock && data && !io->i2c_prev_data) {
            io->i2c_mode = I2C_IDLE;
        } else if (clock && !io->i2c_prev_clock) {
            io->i2c_byte |= (uint8_t)(data << io->i2c_bit);
            io->i2c_bit--;
            if (io->i2c_bit < 0) {
                io->i2c_mode = I2C_RECEIVE_ACK_1;
                io->i2c_dev_data = 0;
                data = 0;
            }
        }
        break;
    case I2C_RECEIVE_ACK_1:
        if (clock && !io->i2c_prev_clock)
            io->i2c_mode = I2C_RECEIVE_ACK_0;
        break;
    case I2C_RECEIVE_ACK_0:
        if (!clock && io->i2c_prev_clock) {
            io->i2c_dev_data = 1;
            data = io->i2c_host_data & 1;
            io->i2c_byte = 0;
            io->i2c_bit = 7;
            io->i2c_mode = I2C_RECEIVE_BYTE;
        }
        break;
    default:
        io->i2c_mode = I2C_IDLE;
        break;
    }

    io->i2c_prev_data = data;
    io->i2c_prev_clock = clock;
}

static uint16_t exio_ident_read(NS10IO *io)
{
    uint16_t result;
    uint8_t response = io->config.exio_ident_code;

    if (io->exio_ident_byte >= 0x20) {
        result = 3;
    } else {
        uint16_t bit = (response >> io->exio_ident_bit) & 1;
        result = bit ? 3 : 0;
    }

    io->exio_ident_bit--;
    if (io->exio_ident_bit == -1) {
        io->exio_ident_bit = 7;
        io->exio_ident_byte++;
    }
    return result;
}

static int read_word(NS10IO *io, uint32_t address, uint16_t *value)
{
    if (address == NS10_SYSTEM) {
        *value = io->config.read_system ? io->config.read_system(io->config.opaque) : 0xffff;
        return NS10_IO_OK;
    }
    if (address == NS10_EXIO_IDENT) {
        *value = exio_ident_read(io);
        return NS10_IO_OK;
    }
    if (address == NS10_INPUTS || address == NS10_INPUTS + 2) {
        uint32_t inputs = io->config.read_in1 ? io->config.read_in1(io->config.opaque) : 0xffffffffu;
        *value = (uint16_t)(address == NS10_INPUTS ? inputs : inputs >> 16);
        return NS10_IO_OK;
    }
    if (address == NS10_I2C_CLOCK) {
        *value = io->i2c_dev_clock & io->i2c_host_clock & 1;
        return NS10_IO_OK;
    }
    if (address == NS10_I2C_DATA) {
        *value = io->i2c_dev_data & io->i2c_host_data & 1;
        return NS10_IO_OK;
    }
    if (address == NS10_INT_STATUS) {
        *value = io->int_status;
        return NS10_IO_OK;
    }
    if (address >= NS10_WATCHDOG && address <= NS10_WATCHDOG + 2) {
        /* MAME maps this as noprw; the address-space unmap value is zero. */
        *value = 0;
        return NS10_IO_OK;
    }
    return NS10_IO_UNMAPPED;
}

static int write_word(NS10IO *io, uint32_t address, uint16_t data, uint16_t mem_mask)
{
    if (address == NS10_EXIO_IDENT) {
        io->exio_ident_bit = 7;
        io->exio_ident_byte = 0;
        return NS10_IO_OK;
    }
    if (address == NS10_I2C_CLOCK) {
        io->i2c_host_clock = combine(io->i2c_host_clock, data, mem_mask);
        i2c_update(io);
        return NS10_IO_OK;
    }
    if (address == NS10_I2C_DATA) {
        io->i2c_host_data = combine(io->i2c_host_data, data, mem_mask);
        i2c_update(io);
        return NS10_IO_OK;
    }
    if (address >= NS10_INT_ACK && address <= NS10_INT_ACK + 6) {
        if (mem_mask)
            io->int_status &= (uint16_t)~(1u << ((address - NS10_INT_ACK) >> 1));
        return NS10_IO_OK;
    }
    if ((address >= NS10_DMA_NOP && address <= NS10_DMA_NOP + 2) ||
        (address >= NS10_WATCHDOG && address <= NS10_WATCHDOG + 2))
        return NS10_IO_OK;
    return NS10_IO_UNMAPPED;
}

static int read_byte(NS10IO *io, uint32_t address, uint8_t *value)
{
    uint32_t aligned = address & ~1u;
    uint16_t word;
    int result = read_word(io, aligned, &word);
    if (result == NS10_IO_OK)
        *value = (uint8_t)(word >> ((address & 1) * 8));
    return result;
}

static int write_byte(NS10IO *io, uint32_t address, uint8_t value, uint8_t byte_mask)
{
    uint16_t data = (uint16_t)value << ((address & 1) * 8);
    uint16_t mask = (uint16_t)byte_mask << ((address & 1) * 8);
    return write_word(io, address & ~1u, data, mask);
}

int ns10_io_create(const NS10IOConfig *config, NS10IO **out_io)
{
    NS10IO *io;
    if (out_io)
        *out_io = NULL;
    if (!out_io)
        return NS10_IO_ARGUMENT;
    io = (NS10IO *)calloc(1, sizeof(*io));
    if (!io)
        return NS10_IO_NOMEM;
    if (config)
        io->config = *config;
    else
        io->config.exio_ident_code = 0xff;
    ns10_io_reset(io);
    *out_io = io;
    return NS10_IO_OK;
}

void ns10_io_destroy(NS10IO *io)
{
    free(io);
}

void ns10_io_reset(NS10IO *io)
{
    if (!io)
        return;
    io->i2c_dev_clock = io->i2c_dev_data = 1;
    io->i2c_host_clock = io->i2c_host_data = 1;
    io->i2c_prev_clock = io->i2c_prev_data = 1;
    io->i2c_mode = I2C_IDLE;
    io->i2c_byte = 0;
    io->i2c_bit = 0;
    io->int_status = 0;
    io->exio_ident_bit = 0;
    io->exio_ident_byte = 0;
    io->timer_fraction_us = 0;
    io->timer_initial_tick = 1; /* timer_device::device_reset starts at t=0. */
}

int ns10_io_read(NS10IO *io, uint32_t address, unsigned width, uint32_t *value)
{
    uint32_t result = 0;
    unsigned i;
    if (!io || !value || (width != 1 && width != 2 && width != 4) ||
        (width > 1 && (address & 1)))
        return NS10_IO_ARGUMENT;
    if (width == 4 && physical_address(address) == NS10_INPUTS) {
        *value = io->config.read_in1 ? io->config.read_in1(io->config.opaque) : 0xffffffffu;
        return NS10_IO_OK;
    }
    for (i = 0; i < width;) {
        if (width - i >= 2) {
            uint16_t word;
            int status = read_word(io, physical_address(address + i), &word);
            if (status != NS10_IO_OK)
                return status;
            result |= (uint32_t)word << (i * 8);
            i += 2;
        } else {
            uint8_t byte;
            int status = read_byte(io, physical_address(address + i), &byte);
            if (status != NS10_IO_OK)
                return status;
            result |= (uint32_t)byte << (i * 8);
            i++;
        }
    }
    *value = result;
    return NS10_IO_OK;
}

int ns10_io_write(NS10IO *io, uint32_t address, unsigned width,
                  uint32_t value, uint32_t mem_mask)
{
    unsigned i;
    if (!io || (width != 1 && width != 2 && width != 4) ||
        (width > 1 && (address & 1)) || (mem_mask & ~width_mask(width)))
        return NS10_IO_ARGUMENT;
    for (i = 0; i < width;) {
        int status;
        if (width - i >= 2) {
            uint16_t data = (uint16_t)(value >> (i * 8));
            uint16_t mask = (uint16_t)(mem_mask >> (i * 8));
            status = mask ? write_word(io, physical_address(address + i), data, mask) : NS10_IO_OK;
            i += 2;
        } else {
            uint8_t mask = (uint8_t)(mem_mask >> (i * 8));
            status = mask ? write_byte(io, physical_address(address + i),
                (uint8_t)(value >> (i * 8)), mask) : NS10_IO_OK;
            i++;
        }
        if (status != NS10_IO_OK)
            return status;
    }
    return NS10_IO_OK;
}

void ns10_io_advance_us(NS10IO *io, uint64_t elapsed_us)
{
    uint64_t ticks;
    if (!io)
        return;
    ticks = elapsed_us / NS10_TICK_US;
    if (io->timer_initial_tick) {
        ticks++;
        io->timer_initial_tick = 0;
    }
    elapsed_us %= NS10_TICK_US;
    io->timer_fraction_us += elapsed_us;
    if (io->timer_fraction_us >= NS10_TICK_US) {
        io->timer_fraction_us -= NS10_TICK_US;
        ticks++;
    }
    while (ticks) {
        io->int_status |= 8;
        if (io->config.irq10)
            io->config.irq10(io->config.opaque, 1);
        ticks--;
    }
}

void ns10_io_cdrom_interrupt(NS10IO *io, int state)
{
    if (!io) return;
    if (state) io->int_status |= 4;
    else io->int_status &= (uint16_t)~4;
    if (io->config.irq10) io->config.irq10(io->config.opaque, !!state);
}
