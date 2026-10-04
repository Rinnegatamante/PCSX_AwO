/*
 * Taito FX-1S base board adapter from MAME sony/zn.cpp and taitosnd.cpp.
 * Source revision: deb2f54fe7300e7a192690318beb860159bfd52a
 * Copyright smf, Philip Bennett (upstream does not state years).
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "taito_fx1.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct TaitoFx1 {
    taito_fx1_config_t config;
    taito_fx1_callbacks_t callbacks;
    Z80 *sound_cpu;
    YM2610Device *ym2610;
    TaitoSoundComm *comm;
    MB3773 *watchdog;
    uint8_t sound_ram[TAITO_FX1_AUDIO_RAM_SIZE];
    unsigned main_bank;
    unsigned sound_bank;
    int sound_reset_asserted;
    int reset_cpu_pending;
    int nmi_asserted;
    int ym_irq_asserted;
    int running_sound;
    int synchronization_pending;
    int soft_reset_pending;
    uint32_t faults;
    uint64_t sound_cycles;
    uint64_t ym_cycles;
    uint64_t ym_bus_events;
    uint64_t last_ym_bus_sound_time;
    uint64_t z80_cycle_origin;
    uint64_t z80_board_cycle_origin;
    uint64_t run_sound_end;
    uint64_t audio_samples;
    uint64_t synchronization_requests;
    uint64_t soft_reset_requests;
    uint64_t now_us;
};

static uint32_t canonical_address(uint32_t address)
{
    if (address >= 0x80000000u && address < 0xc0000000u)
        return address & 0x1fffffffu;
    return address;
}

static int valid_access(uint32_t address, unsigned width)
{
    return (width == 1 || width == 2 || width == 4) &&
        (address & (width - 1)) == 0;
}

static int16_t clamp_sample(int32_t value)
{
    if (value > INT16_MAX)
        value = INT16_MAX;
    else if (value < INT16_MIN)
        value = INT16_MIN;
    return (int16_t)value;
}

static void dispatch_soft_reset(TaitoFx1 *board)
{
    if (!board->soft_reset_pending)
        return;
    board->soft_reset_pending = 0;
    board->soft_reset_requests++;
    if (board->callbacks.request_soft_reset)
        board->callbacks.request_soft_reset(board->callbacks.opaque);
}

static void watchdog_reset_request(void *opaque)
{
    TaitoFx1 *board = (TaitoFx1 *)opaque;
    board->soft_reset_pending = 1;
}

static void synchronization_request(void *opaque)
{
    TaitoFx1 *board = (TaitoFx1 *)opaque;
    board->synchronization_pending = 1;
    board->synchronization_requests++;
    if (board->callbacks.request_sound_synchronization)
        board->callbacks.request_sound_synchronization(board->callbacks.opaque);
}

static void sound_nmi_line(void *opaque, int asserted)
{
    TaitoFx1 *board = (TaitoFx1 *)opaque;
    board->nmi_asserted = asserted != 0;
    if (board->sound_cpu)
        z80_nmi(board->sound_cpu, board->nmi_asserted);
}

static void sound_reset_line(void *opaque, int asserted)
{
    TaitoFx1 *board = (TaitoFx1 *)opaque;
    int next = asserted != 0;
    /* MAME's Z80 RESET callback suspends on ASSERT and calls device().reset()
     * only when RESET is released. */
    if (!next && board->sound_reset_asserted)
        board->reset_cpu_pending = 1;
    board->sound_reset_asserted = next;
}

static void ym_irq_line(void *opaque, int asserted)
{
    TaitoFx1 *board = (TaitoFx1 *)opaque;
    board->ym_irq_asserted = asserted != 0;
    if (board->sound_cpu)
        z80_irq(board->sound_cpu, board->ym_irq_asserted);
}

static uint64_t current_sound_time(TaitoFx1 *board)
{
    Z80Status status;
    uint64_t elapsed;
    if (!board->running_sound)
        return board->sound_cycles;
    z80_get_status(board->sound_cpu, &status);
    elapsed = status.cycles - board->z80_cycle_origin;
    if (elapsed > UINT64_MAX - board->z80_board_cycle_origin) {
        board->faults |= TAITO_FX1_FAULT_TIME;
        return board->run_sound_end;
    }
    elapsed += board->z80_board_cycle_origin;
    return elapsed < board->run_sound_end ? elapsed : board->run_sound_end;
}

static void ym_sync_to_sound_time(TaitoFx1 *board, uint64_t sound_time);

static uint64_t sync_ym_to_current_sound_time(TaitoFx1 *board)
{
    uint64_t sound_time = current_sound_time(board);
    if (board->ym_bus_events && sound_time < board->last_ym_bus_sound_time)
        board->faults |= TAITO_FX1_FAULT_TIME;
    board->last_ym_bus_sound_time = sound_time;
    board->ym_bus_events++;
    ym_sync_to_sound_time(board, sound_time);
    return sound_time;
}

static void ym_sync_to_sound_time(TaitoFx1 *board, uint64_t sound_time)
{
    YM2610Sample sample[1];
    if (sound_time > UINT64_MAX / 2) {
        board->faults |= TAITO_FX1_FAULT_TIME;
        return;
    }
    {
        uint64_t target = sound_time * 2;
        if (target <= board->ym_cycles)
            return;
        while (target > board->ym_cycles) {
            uint64_t remaining = target - board->ym_cycles;
            size_t produced = 0;
            int result;
            /* A host slice is one Z80 T-state, or two 8 MHz YM clocks. */
            if (remaining > 2)
                remaining = 2;
            result = ym2610_run_cycles(board->ym2610, remaining,
                                       sample, 1, &produced);
            if (result != YM2610_OK) {
                board->faults |= TAITO_FX1_FAULT_YM2610;
                return;
            }
            board->ym_cycles += remaining;
            if (produced) {
                int32_t left = (int32_t)sample[0].ssg * 3 / 4 + sample[0].fm_left;
                int32_t right = (int32_t)sample[0].ssg * 3 / 4 + sample[0].fm_right;
                board->audio_samples++;
                if (board->callbacks.audio_sample)
                    board->callbacks.audio_sample(board->callbacks.opaque,
                            clamp_sample(left), clamp_sample(right));
            }
        }
    }
}

static void apply_deferred_cpu_reset(TaitoFx1 *board)
{
    if (!board->reset_cpu_pending)
        return;
    board->reset_cpu_pending = 0;
    /* The TC0140SYT reset output is a level. MAME suspends the Z80 while
     * asserted and calls device().reset() after release; YM keeps running. */
    z80_nmi(board->sound_cpu, 0);
    z80_irq(board->sound_cpu, 0);
    z80_reset(board->sound_cpu);
    {
        Z80Status status;
        z80_get_status(board->sound_cpu, &status);
        board->z80_cycle_origin = status.cycles;
        board->z80_board_cycle_origin = board->sound_cycles;
    }
    if (board->ym_irq_asserted)
        z80_irq(board->sound_cpu, 1);
    if (board->nmi_asserted)
        z80_nmi(board->sound_cpu, 1);
}

static uint8_t sound_memory_read(void *opaque, uint16_t address);
static void sound_memory_write(void *opaque, uint16_t address, uint8_t value);

static uint8_t z80_io_read(void *opaque, uint16_t port)
{
    return taito_fx1_sound_port_read((TaitoFx1 *)opaque, port);
}

static void z80_io_write(void *opaque, uint16_t port, uint8_t value)
{
    taito_fx1_sound_port_write((TaitoFx1 *)opaque, port, value);
}

static int create_devices(TaitoFx1 *board)
{
    Z80Config cpu_config;
    YM2610Config ym_config;
    TaitoSoundCommConfig comm_config;
    MB3773Config watchdog_config;

    memset(&comm_config, 0, sizeof(comm_config));
    comm_config.opaque = board;
    comm_config.sound_nmi_line = sound_nmi_line;
    comm_config.sound_reset_line = sound_reset_line;
    comm_config.request_synchronization = synchronization_request;
    if (taito_sound_comm_create(&comm_config, &board->comm) != TAITO_SOUND_COMM_OK)
        return 0;

    memset(&watchdog_config, 0, sizeof(watchdog_config));
    watchdog_config.opaque = board;
    watchdog_config.request_soft_reset = watchdog_reset_request;
    if (mb3773_create(&watchdog_config, &board->watchdog) != MB3773_OK)
        return 0;

    memset(&ym_config, 0, sizeof(ym_config));
    ym_config.model = YM2610_MODEL_YM2610B;
    ym_config.clock_hz = TAITO_FX1_YM_CLOCK_HZ;
    ym_config.adpcm_a = board->config.adpcm_a;
    ym_config.adpcm_a_size = board->config.adpcm_a_size;
    ym_config.irq_line = ym_irq_line;
    ym_config.irq_opaque = board;
    if (ym2610_create(&ym_config, &board->ym2610) != YM2610_OK)
        return 0;

    memset(&cpu_config, 0, sizeof(cpu_config));
    cpu_config.opaque = board;
    cpu_config.memory_read = sound_memory_read;
    cpu_config.memory_write = sound_memory_write;
    cpu_config.io_read = z80_io_read;
    cpu_config.io_write = z80_io_write;
    cpu_config.irq_ack = NULL; /* The Z80 IM0/IM1/IM2 default acknowledge is the bus 0xff. */
    board->sound_cpu = z80_create(&cpu_config);
    return board->sound_cpu != NULL;
}

TaitoFx1 *taito_fx1_create(const taito_fx1_config_t *config,
                           const taito_fx1_callbacks_t *callbacks)
{
    TaitoFx1 *board;
    if (!config || !callbacks || !callbacks->request_soft_reset ||
        !callbacks->request_sound_synchronization ||
        !config->banked_rom || config->banked_rom_size != TAITO_FX1_MAIN_BANKED_ROM_SIZE ||
        !config->audio_rom || config->audio_rom_size != TAITO_FX1_AUDIO_ROM_SIZE ||
        !config->adpcm_a || (config->adpcm_a_size != 0x200000 &&
                            config->adpcm_a_size != 0x400000))
        return NULL;
    board = (TaitoFx1 *)calloc(1, sizeof(*board));
    if (!board)
        return NULL;
    board->config = *config;
    board->callbacks = *callbacks;
    if (!create_devices(board)) {
        taito_fx1_destroy(board);
        return NULL;
    }
    taito_fx1_reset(board);
    return board;
}

void taito_fx1_destroy(TaitoFx1 *board)
{
    if (!board)
        return;
    z80_destroy(board->sound_cpu);
    ym2610_destroy(board->ym2610);
    taito_sound_comm_destroy(board->comm);
    mb3773_destroy(board->watchdog);
    free(board);
}

void taito_fx1_reset(TaitoFx1 *board)
{
    Z80Status status;
    if (!board)
        return;
    board->main_bank = 0;
    board->sound_bank = 1;
    board->sound_reset_asserted = 0;
    board->reset_cpu_pending = 0;
    board->nmi_asserted = 0;
    board->ym_irq_asserted = 0;
    board->running_sound = 0;
    board->synchronization_pending = 0;
    board->soft_reset_pending = 0;
    board->faults = 0;
    board->sound_cycles = 0;
    board->ym_cycles = 0;
    board->ym_bus_events = 0;
    board->last_ym_bus_sound_time = 0;
    board->run_sound_end = 0;
    board->z80_board_cycle_origin = 0;
    board->audio_samples = 0;
    board->synchronization_requests = 0;
    board->soft_reset_requests = 0;
    z80_nmi(board->sound_cpu, 0);
    z80_irq(board->sound_cpu, 0);
    z80_reset(board->sound_cpu);
    ym2610_reset(board->ym2610);
    taito_sound_comm_reset(board->comm); /* Reset itself deliberately does not drive callbacks. */
    if (mb3773_reset(board->watchdog) != MB3773_OK)
        board->faults |= TAITO_FX1_FAULT_TIME;
    z80_get_status(board->sound_cpu, &status);
    board->z80_cycle_origin = status.cycles;
    board->z80_board_cycle_origin = board->sound_cycles;
}

int taito_fx1_advance_to_us(TaitoFx1 *board, uint64_t now_us)
{
    if (!board)
        return TAITO_FX1_INVALID;
    if (now_us < board->now_us) {
        board->faults |= TAITO_FX1_FAULT_TIME;
        return TAITO_FX1_FAULT;
    }
    if (mb3773_advance_to_us(board->watchdog, now_us) != MB3773_OK) {
        board->faults |= TAITO_FX1_FAULT_TIME;
        return TAITO_FX1_FAULT;
    }
    board->now_us = now_us;
    dispatch_soft_reset(board);
    return TAITO_FX1_HANDLED;
}

void taito_fx1_sound_synchronization_completed(TaitoFx1 *board)
{
    if (board)
        board->synchronization_pending = 0;
}

static int contains(uint32_t address, unsigned width, uint32_t base, uint32_t size)
{
    uint64_t end = (uint64_t)address + width;
    return address >= base && end <= (uint64_t)base + size;
}

static uint8_t main_read8(TaitoFx1 *board, uint32_t address,
                         int *handled, int *fault)
{
    if (address >= 0x1f000000u && address <= 0x1f7fffffu) {
        size_t offset = (size_t)board->main_bank * TAITO_FX1_BANK_WINDOW_SIZE +
                        (address - 0x1f000000u);
        if (offset >= board->config.banked_rom_size) {
            board->faults |= TAITO_FX1_FAULT_MAIN_BANK_BOUNDS;
            *fault = 1;
            return 0xff;
        }
        *handled = 1;
        return board->config.banked_rom[offset];
    }
    if (address == 0x1fb80002u) {
        uint8_t value = 0;
        if (taito_sound_comm_master_comm_read(board->comm, 1, &value) != TAITO_SOUND_COMM_OK) {
            *fault = 1;
            return 0xff;
        }
        *handled = 1;
        return value;
    }
    return 0xff;
}

taito_fx1_result_t taito_fx1_main_read(TaitoFx1 *board, uint32_t address,
                                      unsigned width, uint32_t *value)
{
    uint32_t physical, result = 0;
    unsigned lane;
    int handled = 0, fault = 0;
    if (!board || !value)
        return TAITO_FX1_INVALID;
    physical = canonical_address(address);
    if (!valid_access(physical, width))
        return TAITO_FX1_INVALID;
    if (physical >= 0x1f000000u && physical <= 0x1f7fffffu) {
        if (!contains(physical, width, 0x1f000000u, 0x800000u))
            return TAITO_FX1_UNMAPPED;
        for (lane = 0; lane < width; lane++) {
            uint8_t byte = main_read8(board, physical + lane, &handled, &fault);
            if (fault)
                return TAITO_FX1_FAULT;
            result |= (uint32_t)byte << (lane * 8);
        }
        *value = result;
        return TAITO_FX1_HANDLED;
    }
    result = width == 4 ? 0xffffffffu : ((1u << (width * 8)) - 1u);
    for (lane = 0; lane < width; lane++) {
        int lane_handled = 0;
        uint8_t byte = main_read8(board, physical + lane, &lane_handled, &fault);
        if (fault)
            return TAITO_FX1_FAULT;
        if (lane_handled) {
            result &= ~(0xffu << (lane * 8));
            result |= (uint32_t)byte << (lane * 8);
            handled = 1;
        }
    }
    if (!handled)
        return TAITO_FX1_UNMAPPED;
    *value = result;
    return TAITO_FX1_HANDLED;
}

static int main_write8(TaitoFx1 *board, uint32_t address, uint8_t value)
{
    if (address == 0x1fb40000u) {
        int result;
        board->main_bank = value & 0x03;
        if (mb3773_advance_to_us(board->watchdog, board->now_us) != MB3773_OK ||
            mb3773_write_ck(board->watchdog, (value >> 5) & 1) != MB3773_OK) {
            board->faults |= TAITO_FX1_FAULT_TIME;
            return -1;
        }
        result = 1;
        return result;
    }
    if (address == 0x1fb80000u) {
        taito_sound_comm_master_port_write(board->comm, value);
        return 1;
    }
    if (address == 0x1fb80002u) {
        taito_sound_comm_master_comm_write(board->comm, value);
        return 1;
    }
    return 0;
}

taito_fx1_result_t taito_fx1_main_write(TaitoFx1 *board, uint32_t address,
                                       unsigned width, uint32_t value)
{
    uint32_t physical;
    unsigned lane;
    int handled = 0, fault = 0;
    if (!board)
        return TAITO_FX1_INVALID;
    physical = canonical_address(address);
    if (!valid_access(physical, width))
        return TAITO_FX1_INVALID;
    for (lane = 0; lane < width; lane++) {
        int result = main_write8(board, physical + lane,
                                 (uint8_t)(value >> (lane * 8)));
        if (result < 0)
            fault = 1;
        else if (result > 0)
            handled = 1;
    }
    apply_deferred_cpu_reset(board);
    dispatch_soft_reset(board);
    if (fault)
        return TAITO_FX1_FAULT;
    return handled ? TAITO_FX1_HANDLED : TAITO_FX1_UNMAPPED;
}

static uint8_t sound_memory_read(void *opaque, uint16_t address)
{
    TaitoFx1 *board = (TaitoFx1 *)opaque;
    if (address < 0x4000)
        return board->config.audio_rom[address];
    if (address < 0x8000) {
        size_t offset = (size_t)board->sound_bank * TAITO_FX1_AUDIO_BANK_SIZE +
                        (address - 0x4000);
        if (offset >= board->config.audio_rom_size) {
            board->faults |= TAITO_FX1_FAULT_SOUND_ROM_BOUNDS;
            return 0xff;
        }
        return board->config.audio_rom[offset];
    }
    if (address >= 0xc000 && address < 0xe000)
        return board->sound_ram[address - 0xc000];
    if (address >= 0xe000 && address <= 0xe003) {
        sync_ym_to_current_sound_time(board);
        return ym2610_read(board->ym2610, address - 0xe000);
    }
    if (address == 0xe200)
        return 0; /* MAME map nopr() read. */
    if (address == 0xe201) {
        uint8_t result = 0;
        if (taito_sound_comm_slave_comm_read(board->comm, 1, &result) != TAITO_SOUND_COMM_OK)
            return 0xff;
        return result;
    }
    if (address == 0xee00)
        return 0; /* MAME map noprw(). */
    board->faults |= TAITO_FX1_FAULT_SOUND_UNMAPPED;
    return 0xff; /* MAME default unmapped bus value. */
}

static void sound_memory_write(void *opaque, uint16_t address, uint8_t value)
{
    TaitoFx1 *board = (TaitoFx1 *)opaque;
    if (address >= 0xc000 && address < 0xe000) {
        board->sound_ram[address - 0xc000] = value;
        return;
    }
    if (address >= 0xe000 && address <= 0xe003) {
        sync_ym_to_current_sound_time(board);
        ym2610_write(board->ym2610, address - 0xe000, value);
        return;
    }
    if (address == 0xe200) {
        taito_sound_comm_slave_port_write(board->comm, value);
        return;
    }
    if (address == 0xe201) {
        taito_sound_comm_slave_comm_write(board->comm, value);
        return;
    }
    if ((address >= 0xe400 && address <= 0xe403) || address == 0xee00 ||
        address == 0xf000)
        return; /* MAME nopw/noprw ranges. */
    if (address == 0xf200) {
        board->sound_bank = value & 0x07;
        return;
    }
    board->faults |= TAITO_FX1_FAULT_SOUND_UNMAPPED;
}

taito_fx1_result_t taito_fx1_run_sound_cycles(TaitoFx1 *board, uint64_t cycles)
{
    if (!board)
        return TAITO_FX1_INVALID;
    if (cycles > UINT64_MAX - board->sound_cycles ||
        board->sound_cycles > UINT64_MAX / 2 ||
        cycles > UINT64_MAX / 2 - board->sound_cycles) {
        board->faults |= TAITO_FX1_FAULT_TIME;
        return TAITO_FX1_FAULT;
    }
    while (cycles) {
        if (!board->sound_reset_asserted) {
            board->run_sound_end = board->sound_cycles + 1;
            board->running_sound = 1;
            z80_run_cycles(board->sound_cpu, 1);
            board->running_sound = 0;
        }
        board->sound_cycles++;
        ym_sync_to_sound_time(board, board->sound_cycles);
        cycles--;
    }
    if (board->faults)
        return TAITO_FX1_FAULT;
    return TAITO_FX1_HANDLED;
}

uint8_t taito_fx1_sound_read(TaitoFx1 *board, uint16_t address)
{
    return board ? sound_memory_read(board, address) : 0xff;
}

void taito_fx1_sound_write(TaitoFx1 *board, uint16_t address, uint8_t value)
{
    if (board)
        sound_memory_write(board, address, value);
}

uint8_t taito_fx1_sound_port_read(TaitoFx1 *board, uint16_t port)
{
    (void)port;
    if (board)
        board->faults |= TAITO_FX1_FAULT_SOUND_UNMAPPED;
    return 0xff;
}

void taito_fx1_sound_port_write(TaitoFx1 *board, uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;
    if (board)
        board->faults |= TAITO_FX1_FAULT_SOUND_UNMAPPED;
}

void taito_fx1_get_status(const TaitoFx1 *board, taito_fx1_status_t *status)
{
    Z80Status cpu_status;
    if (!board || !status)
        return;
    memset(status, 0, sizeof(*status));
    status->main_bank = board->main_bank;
    status->main_bank_offset = (size_t)board->main_bank * TAITO_FX1_BANK_WINDOW_SIZE;
    status->sound_bank = board->sound_bank;
    status->sound_bank_offset = (size_t)board->sound_bank * TAITO_FX1_AUDIO_BANK_SIZE;
    status->sound_reset_asserted = board->sound_reset_asserted;
    status->nmi_asserted = board->nmi_asserted;
    status->ym_irq_asserted = board->ym_irq_asserted;
    status->synchronization_pending = board->synchronization_pending;
    status->soft_reset_pending = board->soft_reset_pending;
    status->sound_cycles = board->sound_cycles;
    status->ym_cycles = board->ym_cycles;
    status->ym_bus_events = board->ym_bus_events;
    status->last_ym_bus_sound_time = board->last_ym_bus_sound_time;
    status->audio_samples = board->audio_samples;
    status->watchdog_time_us = mb3773_elapsed_us(board->watchdog);
    status->watchdog_reset_requests = mb3773_reset_requests(board->watchdog);
    status->synchronization_requests = board->synchronization_requests;
    status->faults = board->faults;
    status->tc0140_status = taito_sound_comm_status(board->comm);
    z80_get_status(board->sound_cpu, &cpu_status);
    status->z80_halted = cpu_status.halt;
    status->z80_pc = cpu_status.pc;
    status->z80_instructions = cpu_status.instructions;
}
