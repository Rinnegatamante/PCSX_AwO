/* Standalone host and clock adapter for the MAME TMS57002 DASP core. */
#include "tms57002.h"
#include "tms57002/tms57002_core.h"

#include <cmath>
#include <limits>
#include <memory>

struct tms57002 {
    tms57002_config_t config{};
    uint64_t cycles = 0;
    uint64_t callback_cycle_base = 0;
    bool running = false;
    unsigned callback_depth = 0;
    bool callback_fault = false;
    tms57002_device core;

    explicit tms57002(const tms57002_config_t *cfg)
        : config(cfg ? *cfg : tms57002_config_t{}),
          cycles(cfg ? cfg->cycle_origin : 0),
          core(
              [this](uint32_t address) -> uint8_t {
                  if (!config.data_read) return 0xff;
                  return call_data_read(address);
              },
              [this](uint32_t address, uint8_t value) {
                  if (config.data_write) call_data_write(address, value);
              },
              [this](int value) { call_line(config.dready_changed, value); },
              [this](int value) { call_line(config.pc0_changed, value); },
              [this](int value) { call_line(config.empty_changed, value); })
    {
    }

    bool reject_reentry()
    {
        if (!callback_depth) return false;
        callback_fault = true;
        return true;
    }

    uint64_t current_cycle() const
    {
        return running ? callback_cycle_base + core.cycles_elapsed() : cycles;
    }

    uint8_t call_data_read(uint32_t address)
    {
        ++callback_depth;
        uint8_t value = config.data_read(config.opaque, address);
        --callback_depth;
        return value;
    }

    void call_data_write(uint32_t address, uint8_t value)
    {
        ++callback_depth;
        config.data_write(config.opaque, address, value);
        --callback_depth;
    }

    void call_line(tms57002_line_cb callback, int value)
    {
        if (!callback) return;
        ++callback_depth;
        callback(config.opaque, value);
        --callback_depth;
    }
};

extern "C" {

tms57002_t *tms57002_create(const tms57002_config_t *config)
{
    try {
        return new tms57002(config);
    } catch (...) {
        return nullptr;
    }
}

void tms57002_destroy(tms57002_t *dsp)
{
    if (!dsp) return;
    if (dsp->reject_reentry()) return;
    delete dsp;
}

void tms57002_reset(tms57002_t *dsp)
{
    if (!dsp || dsp->reject_reentry()) return;
    dsp->core.reset();
}

int64_t tms57002_run(tms57002_t *dsp, uint64_t cycles)
{
    if (!dsp) return -1;
    if (dsp->reject_reentry()) return -2;
    if (cycles > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        cycles > std::numeric_limits<uint64_t>::max() - dsp->cycles)
        return -1;
    uint64_t remaining = cycles;
    dsp->running = true;
    while (remaining) {
        const uint32_t slice = remaining > 0x7fffffffULL
                                   ? 0x7fffffffU
                                   : static_cast<uint32_t>(remaining);
        dsp->callback_cycle_base = dsp->cycles + (cycles - remaining);
        dsp->core.execute_cycles(slice);
        remaining -= slice;
    }
    dsp->cycles += cycles;
    dsp->running = false;
    return static_cast<int64_t>(cycles);
}

uint64_t tms57002_total_cycles(const tms57002_t *dsp)
{
    return dsp ? dsp->cycles : 0;
}

uint64_t tms57002_current_cycle(const tms57002_t *dsp)
{
    return dsp ? dsp->current_cycle() : 0;
}

int tms57002_callback_fault(const tms57002_t *dsp)
{
    return dsp && dsp->callback_fault;
}

int tms57002_unsupported_instruction(const tms57002_t *dsp)
{
    return dsp && dsp->core.unsupported_instruction();
}

uint8_t tms57002_data_r(tms57002_t *dsp)
{
    if (!dsp || dsp->reject_reentry()) return 0xff;
    return dsp->core.data_r();
}

void tms57002_data_w(tms57002_t *dsp, uint8_t value)
{
    if (!dsp || dsp->reject_reentry()) return;
    dsp->core.data_w(value);
}

void tms57002_pload_w(tms57002_t *dsp, int state)
{
    if (!dsp || dsp->reject_reentry()) return;
    dsp->core.pload_w(state);
}

void tms57002_cload_w(tms57002_t *dsp, int state)
{
    if (!dsp || dsp->reject_reentry()) return;
    dsp->core.cload_w(state);
}

int tms57002_dready_r(const tms57002_t *dsp)
{
    return dsp ? dsp->core.dready_r() : 1;
}

int tms57002_pc0_r(const tms57002_t *dsp)
{
    return dsp ? dsp->core.pc0_r() : 1;
}

int tms57002_empty_r(const tms57002_t *dsp)
{
    return dsp ? dsp->core.empty_r() : 1;
}

void tms57002_sync_w(tms57002_t *dsp, int state)
{
    if (!dsp || dsp->reject_reentry()) return;
    dsp->core.sync_w(state);
}

int tms57002_process_sample(tms57002_t *dsp, const float input[4],
                            float output[4])
{
    if (!dsp || !input || !output || dsp->reject_reentry()) return -1;
    const float scale = dsp->core.st0_value() & 8 ? 32768.0f * 256.0f : 32768.0f;
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(input[i]) ||
            static_cast<double>(input[i]) * scale < INT32_MIN ||
            static_cast<double>(input[i]) * scale > INT32_MAX)
            return -1;
    }
    dsp->core.process_sample(input, output);
    return 0;
}

int tms57002_program_write(tms57002_t *dsp, uint8_t address, uint32_t opcode)
{
    if (!dsp || dsp->reject_reentry()) return -1;
    dsp->core.program_write(address, opcode & 0xffffff);
    return 0;
}

uint32_t tms57002_program_read(const tms57002_t *dsp, uint8_t address)
{
    return dsp ? dsp->core.program_read(address) : 0;
}

uint8_t tms57002_pc(const tms57002_t *dsp)
{
    return dsp ? dsp->core.pc_value() : 0;
}

uint32_t tms57002_st0(const tms57002_t *dsp)
{
    return dsp ? dsp->core.st0_value() : 0;
}

uint32_t tms57002_st1(const tms57002_t *dsp)
{
    return dsp ? dsp->core.st1_value() : 0;
}

uint32_t tms57002_aacc(const tms57002_t *dsp)
{
    return dsp ? dsp->core.aacc_value() : 0;
}

int64_t tms57002_macc(const tms57002_t *dsp)
{
    return dsp ? dsp->core.macc_value() : 0;
}

uint32_t tms57002_dmem_read(const tms57002_t *dsp, unsigned bank,
                            unsigned address)
{
    return dsp ? dsp->core.dmem_read(bank, address) : 0;
}

} // extern "C"
