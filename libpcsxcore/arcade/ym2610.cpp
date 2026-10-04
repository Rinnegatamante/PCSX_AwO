/* Copyright (c) 2021, Aaron Giles. All rights reserved.
 * BSD 3-Clause; see arcade/ymfm/LICENSE.
 */
#include "ym2610.h"

#include "ymfm/ymfm_opn.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>

struct YM2610Device final : ymfm::ymfm_interface {
    explicit YM2610Device(const YM2610Config &cfg)
        : config(cfg), clock_hz(cfg.clock_hz), model(static_cast<YM2610Model>(cfg.model))
    {
        if (model == YM2610_MODEL_YM2610)
            chip.reset(new (std::nothrow) ymfm::ym2610(*this));
        else
            chip.reset(new (std::nothrow) ymfm::ym2610b(*this));
        if (chip) {
            chip->set_fidelity(ymfm::OPN_FIDELITY_MED);
            chip->reset(); /* MAME starts devices, then resets their engines. */
        }
    }

    void ymfm_set_timer(uint32_t timer, int32_t duration) override
    {
        if (timer >= 2)
            return;
        if (duration < 0) {
            timer_active[timer] = false;
            return;
        }
        uint64_t end;
        if (uint64_t(duration) > std::numeric_limits<uint64_t>::max() - now_cycles)
            end = std::numeric_limits<uint64_t>::max();
        else
            end = now_cycles + uint64_t(duration);
        timer_deadline[timer] = end;
        timer_active[timer] = true;
    }

    void ymfm_set_busy_end(uint32_t clocks) override
    {
        if (uint64_t(clocks) > std::numeric_limits<uint64_t>::max() - now_cycles)
            busy_end = std::numeric_limits<uint64_t>::max();
        else
            busy_end = now_cycles + clocks;
    }

    bool ymfm_is_busy() override { return now_cycles < busy_end; }

    void expire_timer(uint32_t timer)
    {
        m_engine->engine_timer_expired(timer);
    }

    void ymfm_update_irq(bool asserted) override
    {
        if (irq_state == asserted)
            return;
        irq_state = asserted;
        if (config.irq_line != nullptr)
            config.irq_line(config.irq_opaque, asserted ? 1 : 0);
    }

    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
    {
        address &= 0xffffffu; /* MAME's 24-bit ADPCM memory spaces. */
        YM2610RomBank bank;
        const uint8_t *region;
        size_t region_size;
        if (type == ymfm::ACCESS_ADPCM_A) {
            bank = YM2610_ROM_ADPCM_A;
            region = config.adpcm_a;
            region_size = config.adpcm_a_size;
        } else if (type == ymfm::ACCESS_ADPCM_B) {
            bank = YM2610_ROM_ADPCM_B;
            region = config.adpcm_b;
            region_size = config.adpcm_b_size;
            if (region == nullptr && config.adpcm_a != nullptr) {
                region = config.adpcm_a;
                region_size = config.adpcm_a_size;
            }
        } else {
            return 0;
        }

        if (config.read_adpcm != nullptr)
            return config.read_adpcm(config.rom_opaque, bank, address);
        if (region == nullptr || address >= region_size)
            return 0; /* address_map's default unmapped read value. */
        return region[address];
    }

    YM2610Config config;
    uint32_t clock_hz;
    YM2610Model model;
    std::unique_ptr<ymfm::ym2610> chip;
    uint64_t now_cycles = 0;
    uint64_t busy_end = 0;
    uint64_t timer_deadline[2] = {0, 0};
    bool timer_active[2] = {false, false};
    uint64_t sample_phase = 0;
    bool irq_state = false;
};

static int16_t clamp_sample(int32_t sample)
{
    sample = std::max<int32_t>(sample, -32768);
    sample = std::min<int32_t>(sample, 32767);
    return static_cast<int16_t>(sample);
}

extern "C" int ym2610_create(const YM2610Config *config, YM2610Device **out_device)
{
    if (out_device == nullptr)
        return YM2610_ARGUMENT;
    *out_device = nullptr;
    if (config == nullptr || config->clock_hz < 144 ||
        (config->model != uint32_t(YM2610_MODEL_YM2610) &&
         config->model != uint32_t(YM2610_MODEL_YM2610B)) ||
        (config->adpcm_a == nullptr && config->adpcm_a_size != 0) ||
        (config->adpcm_b == nullptr && config->adpcm_b_size != 0))
        return config != nullptr && config->model != uint32_t(YM2610_MODEL_YM2610) &&
               config->model != uint32_t(YM2610_MODEL_YM2610B) ? YM2610_BAD_MODEL : YM2610_ARGUMENT;

    YM2610Device *device = new (std::nothrow) YM2610Device(*config);
    if (device == nullptr)
        return YM2610_NOMEM;
    if (!device->chip) {
        delete device;
        return YM2610_NOMEM;
    }
    *out_device = device;
    return YM2610_OK;
}

extern "C" void ym2610_destroy(YM2610Device *device)
{
    delete device;
}

extern "C" void ym2610_reset(YM2610Device *device)
{
    if (device == nullptr)
        return;
    device->timer_active[0] = false;
    device->timer_active[1] = false;
    device->chip->reset();
}

extern "C" uint8_t ym2610_read(YM2610Device *device, uint32_t offset)
{
    return device == nullptr ? 0xff : device->chip->read(offset & 3);
}

extern "C" void ym2610_write(YM2610Device *device, uint32_t offset, uint8_t value)
{
    if (device != nullptr)
        device->chip->write(offset & 3, value);
}

extern "C" int ym2610_run_cycles(YM2610Device *device, uint64_t cycles,
                                 YM2610Sample *samples, size_t capacity,
                                 size_t *samples_generated)
{
    if (samples_generated != nullptr)
        *samples_generated = 0;
    if (device == nullptr || samples_generated == nullptr)
        return YM2610_ARGUMENT;
    if (cycles > std::numeric_limits<uint64_t>::max() - device->now_cycles)
        return YM2610_TIME_OVERFLOW;
    const uint32_t rate = device->clock_hz / 144;
    uint64_t total = (cycles / device->clock_hz) * rate;
    uint64_t remainder = (cycles % device->clock_hz) * rate + device->sample_phase;
    total += remainder / device->clock_hz;
    if (total > capacity || (total != 0 && samples == nullptr))
        return YM2610_CAPACITY;

    uint64_t remaining = cycles;
    size_t produced = 0;
    while (remaining != 0) {
        int timer = -1;
        for (int i = 0; i < 2; ++i) {
            if (device->timer_active[i] && device->timer_deadline[i] <= device->now_cycles &&
                (timer < 0 || device->timer_deadline[i] < device->timer_deadline[timer]))
                timer = i;
        }
        if (timer >= 0) {
            device->timer_active[timer] = false;
            device->expire_timer(static_cast<uint32_t>(timer));
            continue;
        }

        uint64_t clocks_to_sample =
            (device->clock_hz - device->sample_phase + rate - 1) / rate;
        uint64_t step = std::min<uint64_t>(remaining, clocks_to_sample);
        for (int i = 0; i < 2; ++i) {
            if (device->timer_active[i] && device->timer_deadline[i] > device->now_cycles) {
                step = std::min<uint64_t>(step, device->timer_deadline[i] - device->now_cycles);
            }
        }
        if (step == 0)
            continue;
        device->now_cycles += step;
        remaining -= step;
        device->sample_phase += step * rate;

        if (device->sample_phase >= device->clock_hz) {
            ymfm::ym2610::output_data output;
            device->chip->generate(&output, 1);
            samples[produced].ssg = clamp_sample(output.data[2]);
            samples[produced].fm_left = clamp_sample(output.data[0]);
            samples[produced].fm_right = clamp_sample(output.data[1]);
            ++produced;
            device->sample_phase -= device->clock_hz;
        }
    }
    /* A timer may expire exactly on the last clock even when no later clock
     * is requested. MAME's scheduler observes it at that deadline. */
    for (;;) {
        int timer = -1;
        for (int i = 0; i < 2; ++i) {
            if (device->timer_active[i] && device->timer_deadline[i] <= device->now_cycles &&
                (timer < 0 || device->timer_deadline[i] < device->timer_deadline[timer]))
                timer = i;
        }
        if (timer < 0)
            break;
        device->timer_active[timer] = false;
        device->expire_timer(static_cast<uint32_t>(timer));
    }
    *samples_generated = produced;
    return YM2610_OK;
}

extern "C" uint64_t ym2610_elapsed_cycles(const YM2610Device *device)
{
    return device == nullptr ? 0 : device->now_cycles;
}

extern "C" uint32_t ym2610_sample_rate(const YM2610Device *device)
{
    return device == nullptr ? 0 : device->clock_hz / 144;
}
