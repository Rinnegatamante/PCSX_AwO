// SPDX-License-Identifier: BSD-3-Clause
// Complete QSound HLE algorithm adapted from MAME qsoundhle.cpp/.h.
// Copyright superctr, Valley Bell. ROM mapping copyright Olivier Galibert.
#include "qsound_hle.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
namespace {
int32_t wrap_add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
int32_t wrap_sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
int32_t wrap_shl(int32_t a, unsigned n) { return int32_t(uint32_t(a) << n); }
} // namespace
struct QSoundHLE {
    QSoundHLEConfig config;
    uint32_t clock = 60000000, rate = 24038, region_mask = 0, region_bank = 0, region_banks = 1;
    uint8_t crystal_remainder = 0;
    uint64_t audio_phase = 0, frames = 0, cycles = 0, commands = 0;
    uint8_t read_byte(uint32_t a) const {
        a &= 0xffffff;
        if (config.sample_bytes >= 0x1000000U)
            return config.samples[uint64_t(region_bank) * 0x1000000U + a];
        a &= region_mask;
        return a < config.sample_bytes ? config.samples[a] : 0;
    }
    explicit QSoundHLE(const QSoundHLEConfig &c) : config(c) {
        std::copy(c.firmware, c.firmware + 0x1000, m_dsp_rom.begin());
        clock = c.clock_hz ? c.clock_hz : 60000000;
        rate = clock / 2496;
        region_mask = c.sample_bytes - 1;
        region_mask |= region_mask >> 1;
        region_mask |= region_mask >> 2;
        region_mask |= region_mask >> 4;
        region_mask |= region_mask >> 8;
        region_mask |= region_mask >> 16;
        region_banks = std::max(1U, c.sample_bytes / 0x1000000U);
        init_register_map();
        device_reset();
    }
    void device_reset();
    void frame() {
        update_sample();
        ++frames;
        if (config.pcm)
            config.pcm(config.opaque, m_out[0], m_out[1], frames);
    }
    // DSP ROM sample map
    enum {
        DATA_PAN_TAB = 0x110,
        DATA_ADPCM_TAB = 0x9dc,
        DATA_FILTER_TAB = 0xd53,  // dual filter mode, 5 tables * 95 taps each
        DATA_FILTER_TAB2 = 0xf2e, // overlapping data (95+15+95)

        STATE_BOOT = 0x000,
        STATE_INIT1 = 0x288,
        STATE_INIT2 = 0x61a,
        STATE_REFRESH1 = 0x039,
        STATE_REFRESH2 = 0x04f,
        STATE_NORMAL1 = 0x314,
        STATE_NORMAL2 = 0x6b2
    };

    const uint16_t PAN_TABLE_DRY = 0;
    const uint16_t PAN_TABLE_WET = 98;
    const uint16_t PAN_TABLE_CH_OFFSET = 196;
    const uint16_t FILTER_ENTRY_SIZE = 95;
    const uint16_t DELAY_BASE_OFFSET = 0x554;
    const uint16_t DELAY_BASE_OFFSET2 = 0x53c;

    struct qsound_voice {
        uint16_t m_bank = 0;
        int16_t m_addr = 0; // top word is the sample address
        uint16_t m_phase = 0;
        uint16_t m_rate = 0;
        int16_t m_loop_len = 0;
        int16_t m_end_addr = 0;
        int16_t m_volume = 0;
        int16_t m_echo = 0;

        int16_t update(QSoundHLE &dsp, int32_t *echo_out);
    };

    struct qsound_adpcm {
        uint16_t m_start_addr = 0;
        uint16_t m_end_addr = 0;
        uint16_t m_bank = 0;
        int16_t m_volume = 0;
        uint16_t m_flag = 0;
        int16_t m_cur_vol = 0;
        int16_t m_step_size = 0;
        uint16_t m_cur_addr = 0;

        int16_t update(QSoundHLE &dsp, int16_t curr_sample, int nibble);
    };

    // Q1 Filter
    struct qsound_fir {
        int m_tap_count = 0; // usually 95
        int m_delay_pos = 0;
        uint16_t m_table_pos = 0;
        int16_t m_taps[95] = {0};
        int16_t m_delay_line[95] = {0};

        int32_t apply(int16_t input);
    };

    // Delay line
    struct qsound_delay {
        int16_t m_delay = 0;
        int16_t m_volume = 0;
        int16_t m_write_pos = 0;
        int16_t m_read_pos = 0;
        int16_t m_delay_line[51] = {0};

        int32_t apply(const int32_t input);
        void update();
    };

    struct qsound_echo {
        uint16_t m_end_pos = 0;

        int16_t m_feedback = 0;
        int16_t m_length = 0;
        int16_t m_last_sample = 0;
        int16_t m_delay_line[1024] = {0};
        int16_t m_delay_pos = 0;

        int16_t apply(int32_t input);
    };

    // MAME resources

    std::array<uint16_t, 0x1000> m_dsp_rom{};

    uint16_t m_data_latch = 0;
    int16_t m_out[2]{};

    qsound_voice m_voice[16];
    qsound_adpcm m_adpcm[3];

    uint16_t m_voice_pan[16 + 3]{};
    int16_t m_voice_output[16 + 3]{};

    qsound_echo m_echo;

    qsound_fir m_filter[2];
    qsound_fir m_alt_filter[2];

    qsound_delay m_wet[2];
    qsound_delay m_dry[2];

    uint16_t m_state{};
    uint16_t m_next_state{};

    uint16_t m_delay_update{};

    int m_state_counter{};
    int m_ready_flag{};

    uint16_t *m_register_map[256]{};

    inline uint16_t read_dsp_rom(uint16_t addr) { return m_dsp_rom[addr & 0xfff]; }

    void write_data(uint8_t addr, uint16_t data);

    void init_register_map();
    void update_sample();

    // DSP states
    void state_init();
    void state_refresh_filter_1();
    void state_refresh_filter_2();
    void state_normal_update();

    // sub functions
    int16_t read_sample(uint16_t bank, uint16_t address);
};
void QSoundHLE::device_reset() {
    m_ready_flag = 0;
    m_out[0] = m_out[1] = 0;
    m_state = STATE_BOOT;
    m_state_counter = 0;
}

void QSoundHLE::write_data(uint8_t address, uint16_t data) {
    uint16_t *destination = m_register_map[address];
    if (destination)
        *destination = data;
    m_ready_flag = 0;
}

void QSoundHLE::init_register_map() {
    // unused registers
    std::fill(std::begin(m_register_map), std::end(m_register_map), nullptr);

    // PCM registers
    for (int i = 0; i < 16; i++) // PCM voices
    {
        m_register_map[(i << 3) + 0] =
            (uint16_t *)&m_voice[(i + 1) % 16].m_bank; // Bank applies to the next channel
        m_register_map[(i << 3) + 1] =
            (uint16_t *)&m_voice[i].m_addr; // Current sample position and start position.
        m_register_map[(i << 3) + 2] = (uint16_t *)&m_voice[i].m_rate; // 4.12 fixed point decimal.
        m_register_map[(i << 3) + 3] = (uint16_t *)&m_voice[i].m_phase;
        m_register_map[(i << 3) + 4] = (uint16_t *)&m_voice[i].m_loop_len;
        m_register_map[(i << 3) + 5] = (uint16_t *)&m_voice[i].m_end_addr;
        m_register_map[(i << 3) + 6] = (uint16_t *)&m_voice[i].m_volume;
        m_register_map[(i << 3) + 7] = nullptr; // unused
        m_register_map[i + 0x80] = (uint16_t *)&m_voice_pan[i];
        m_register_map[i + 0xba] = (uint16_t *)&m_voice[i].m_echo;
    }

    // ADPCM registers
    for (int i = 0; i < 3; i++) // ADPCM voices
    {
        // ADPCM sample rate is fixed to 8khz. (one channel is updated every third sample)
        m_register_map[(i << 2) + 0xca] = (uint16_t *)&m_adpcm[i].m_start_addr;
        m_register_map[(i << 2) + 0xcb] = (uint16_t *)&m_adpcm[i].m_end_addr;
        m_register_map[(i << 2) + 0xcc] = (uint16_t *)&m_adpcm[i].m_bank;
        m_register_map[(i << 2) + 0xcd] = (uint16_t *)&m_adpcm[i].m_volume;
        m_register_map[i + 0xd6] = (uint16_t *)&m_adpcm[i].m_flag; // non-zero to start ADPCM playback
        m_register_map[i + 0x90] = (uint16_t *)&m_voice_pan[16 + i];
    }

    // QSound registers
    m_register_map[0x93] = (uint16_t *)&m_echo.m_feedback;
    m_register_map[0xd9] = (uint16_t *)&m_echo.m_end_pos;
    m_register_map[0xe2] = (uint16_t *)&m_delay_update; // non-zero to update delays
    m_register_map[0xe3] = (uint16_t *)&m_next_state;
    for (int i = 0; i < 2; i++) // left, right
    {
        // Wet
        m_register_map[(i << 1) + 0xda] = (uint16_t *)&m_filter[i].m_table_pos;
        m_register_map[(i << 1) + 0xde] = (uint16_t *)&m_wet[i].m_delay;
        m_register_map[(i << 1) + 0xe4] = (uint16_t *)&m_wet[i].m_volume;
        // Dry
        m_register_map[(i << 1) + 0xdb] = (uint16_t *)&m_alt_filter[i].m_table_pos;
        m_register_map[(i << 1) + 0xdf] = (uint16_t *)&m_dry[i].m_delay;
        m_register_map[(i << 1) + 0xe5] = (uint16_t *)&m_dry[i].m_volume;
    }
}

int16_t QSoundHLE::read_sample(uint16_t bank, uint16_t address) {
    bank &= 0x7fff;
    const uint32_t rom_addr = (bank << 16) | (address << 0);
    const uint8_t sample_data = read_byte(rom_addr);
    return (int16_t)(sample_data << 8); // bit0-7 is tied to ground
}

/********************************************************************/

// updates one DSP sample
void QSoundHLE::update_sample() {
    switch (m_state) {
    default:
    case STATE_INIT1:
    case STATE_INIT2:
        return state_init();
    case STATE_REFRESH1:
        return state_refresh_filter_1();
    case STATE_REFRESH2:
        return state_refresh_filter_2();
    case STATE_NORMAL1:
    case STATE_NORMAL2:
        return state_normal_update();
    }
}

// Initialization routine
void QSoundHLE::state_init() {
    int mode = (m_state == STATE_INIT2) ? 1 : 0;

    // we're busy for 4 samples, including the filter refresh.
    if (m_state_counter >= 2) {
        m_state_counter = 0;
        m_state = m_next_state;
        return;
    } else if (m_state_counter == 1) {
        m_state_counter++;
        return;
    }

    std::fill(std::begin(m_voice), std::end(m_voice), qsound_voice());
    std::fill(std::begin(m_adpcm), std::end(m_adpcm), qsound_adpcm());
    std::fill(std::begin(m_filter), std::end(m_filter), qsound_fir());
    std::fill(std::begin(m_alt_filter), std::end(m_alt_filter), qsound_fir());
    std::fill(std::begin(m_wet), std::end(m_wet), qsound_delay());
    std::fill(std::begin(m_dry), std::end(m_dry), qsound_delay());
    m_echo = qsound_echo();

    for (int i = 0; i < 19; i++) {
        m_voice_pan[i] = DATA_PAN_TAB + 0x10;
        m_voice_output[i] = 0;
    }

    for (int i = 0; i < 16; i++)
        m_voice[i].m_bank = 0x8000;
    for (int i = 0; i < 3; i++)
        m_adpcm[i].m_bank = 0x8000;

    if (mode == 0) {
        // mode 1
        m_wet[0].m_delay = 0;
        m_dry[0].m_delay = 46;
        m_wet[1].m_delay = 0;
        m_dry[1].m_delay = 48;
        m_filter[0].m_table_pos = DATA_FILTER_TAB + (FILTER_ENTRY_SIZE * 1);
        m_filter[1].m_table_pos = DATA_FILTER_TAB + (FILTER_ENTRY_SIZE * 2);
        m_echo.m_end_pos = DELAY_BASE_OFFSET + 6;
        m_next_state = STATE_REFRESH1;
    } else {
        // mode 2
        m_wet[0].m_delay = 1;
        m_dry[0].m_delay = 0;
        m_wet[1].m_delay = 0;
        m_dry[1].m_delay = 0;
        m_filter[0].m_table_pos = 0xf73;
        m_filter[1].m_table_pos = 0xfa4;
        m_alt_filter[0].m_table_pos = 0xf73;
        m_alt_filter[1].m_table_pos = 0xfa4;
        m_echo.m_end_pos = DELAY_BASE_OFFSET2 + 6;
        m_next_state = STATE_REFRESH2;
    }

    m_wet[0].m_volume = 0x3fff;
    m_dry[0].m_volume = 0x3fff;
    m_wet[1].m_volume = 0x3fff;
    m_dry[1].m_volume = 0x3fff;

    m_delay_update = 1;
    m_ready_flag = 0;
    m_state_counter = 1;
}

// Updates filter parameters for mode 1
void QSoundHLE::state_refresh_filter_1() {
    for (int ch = 0; ch < 2; ch++) {
        m_filter[ch].m_delay_pos = 0;
        m_filter[ch].m_tap_count = 95;

        for (int i = 0; i < 95; i++)
            m_filter[ch].m_taps[i] = read_dsp_rom(m_filter[ch].m_table_pos + i);
    }

    m_state = m_next_state = STATE_NORMAL1;
}

// Updates filter parameters for mode 2
void QSoundHLE::state_refresh_filter_2() {
    for (int ch = 0; ch < 2; ch++) {
        m_filter[ch].m_delay_pos = 0;
        m_filter[ch].m_tap_count = 45;

        for (int i = 0; i < 45; i++)
            m_filter[ch].m_taps[i] = (int16_t)read_dsp_rom(m_filter[ch].m_table_pos + i);

        m_alt_filter[ch].m_delay_pos = 0;
        m_alt_filter[ch].m_tap_count = 44;

        for (int i = 0; i < 44; i++)
            m_alt_filter[ch].m_taps[i] = (int16_t)read_dsp_rom(m_alt_filter[ch].m_table_pos + i);
    }

    m_state = m_next_state = STATE_NORMAL2;
}

// Updates a PCM voice. There are 16 voices, each are updated every sample
// with full rate and volume control.
int16_t QSoundHLE::qsound_voice::update(QSoundHLE &dsp, int32_t *echo_out) {
    // Read sample from rom and apply volume
    const int16_t output = (m_volume * dsp.read_sample(m_bank, m_addr)) >> 14;

    *echo_out = wrap_add(*echo_out, wrap_shl(output * m_echo, 2));

    // Add delta to the phase and loop back if required
    int32_t new_phase = m_rate + (int32_t(m_addr) * 4096) + (m_phase >> 4);

    if ((new_phase >> 12) >= m_end_addr)
        new_phase -= (int32_t(m_loop_len) * 4096);

    new_phase = std::clamp<int32_t>(new_phase, -0x8000000, 0x7FFFFFF);
    m_addr = new_phase >> 12;
    m_phase = (uint32_t(new_phase) << 4) & 0xffff;

    return output;
}

// Updates an ADPCM voice. There are 3 voices, one is updated every sample
// (effectively making the ADPCM rate 1/3 of the master sample rate), and
// volume is set when starting samples only.
// The ADPCM algorithm is supposedly similar to Yamaha ADPCM. It also seems
// like Capcom never used it, so this was not emulated in the earlier QSound
// emulators.
int16_t QSoundHLE::qsound_adpcm::update(QSoundHLE &dsp, int16_t curr_sample, int nibble) {
    int8_t step;
    if (!nibble) {
        // Mute voice when it reaches the end address.
        if (m_cur_addr == m_end_addr)
            m_cur_vol = 0;

        // Playback start flag
        if (m_flag) {
            curr_sample = 0;
            m_flag = 0;
            m_step_size = 10;
            m_cur_vol = m_volume;
            m_cur_addr = m_start_addr;
        }

        // get top nibble
        step = dsp.read_sample(m_bank, m_cur_addr) >> 8;
    } else {
        // get bottom nibble
        step = dsp.read_sample(m_bank, m_cur_addr++) >> 4;
    }

    // shift with sign extend
    step >>= 4;

    // delta = (0.5 + abs(step)) * m_step_size
    int32_t delta = ((1 + abs(int(step) * 2)) * m_step_size) >> 1;
    if (step <= 0)
        delta = -delta;
    delta += curr_sample;
    delta = std::clamp<int32_t>(delta, -32768, 32767);

    m_step_size = (dsp.read_dsp_rom(DATA_ADPCM_TAB + 8 + step) * m_step_size) >> 6;
    m_step_size = std::clamp<int16_t>(m_step_size, 1, 2000);

    return (delta * m_cur_vol) >> 16;
}

// The echo effect is pretty simple. A moving average filter is used on
// the output from the delay line to smooth samples over time.
int16_t QSoundHLE::qsound_echo::apply(int32_t input) {
    // get average of last 2 samples from the delay line
    int32_t old_sample = m_delay_line[m_delay_pos];
    const int32_t last_sample = m_last_sample;
    m_last_sample = old_sample;
    old_sample = (old_sample + last_sample) >> 1;

    // add current sample to the delay line
    int32_t new_sample = wrap_add(input, wrap_shl(old_sample * m_feedback, 2));
    m_delay_line[m_delay_pos++] = new_sample >> 16;

    if (m_delay_pos >= m_length)
        m_delay_pos = 0;

    return old_sample;
}

// Process a sample update
void QSoundHLE::state_normal_update() {
    m_ready_flag = 0x80;

    // recalculate echo length
    if (m_state == STATE_NORMAL2)
        m_echo.m_length = m_echo.m_end_pos - DELAY_BASE_OFFSET2;
    else
        m_echo.m_length = m_echo.m_end_pos - DELAY_BASE_OFFSET;

    m_echo.m_length = std::clamp<int16_t>(m_echo.m_length, 0, 1024);

    // update PCM voices
    int32_t echo_input = 0;
    for (int i = 0; i < 16; i++)
        m_voice_output[i] = m_voice[i].update(*this, &echo_input);

    // update ADPCM voices (one every third sample)
    const int adpcm_voice = m_state_counter % 3;
    m_voice_output[16 + adpcm_voice] =
        m_adpcm[adpcm_voice].update(*this, m_voice_output[16 + adpcm_voice], m_state_counter / 3);

    int16_t echo_output = m_echo.apply(echo_input);

    // now, we do the magic stuff
    for (int ch = 0; ch < 2; ch++) {
        // Echo is output on the unfiltered component of the left channel and
        // the filtered component of the right channel.
        int32_t wet = (ch == 1) ? int32_t(echo_output) * 16384 : 0;
        int32_t dry = (ch == 0) ? int32_t(echo_output) * 16384 : 0;

        for (int i = 0; i < 19; i++) {
            uint16_t pan_index = m_voice_pan[i] + (ch * PAN_TABLE_CH_OFFSET);

            // Apply different volume tables on the dry and wet inputs.
            dry = wrap_sub(dry, m_voice_output[i] * (int16_t)read_dsp_rom(pan_index + PAN_TABLE_DRY));
            wet = wrap_sub(wet, m_voice_output[i] * (int16_t)read_dsp_rom(pan_index + PAN_TABLE_WET));
        }
        // Saturate accumulated voices
        dry = std::clamp<int32_t>(dry, -0x1fffffff, 0x1fffffff) * 4;
        wet = std::clamp<int32_t>(wet, -0x1fffffff, 0x1fffffff) * 4;

        // Apply FIR filter on 'wet' input
        wet = m_filter[ch].apply(wet >> 16);

        // in mode 2, we do this on the 'dry' input too
        if (m_state == STATE_NORMAL2)
            dry = m_alt_filter[ch].apply(dry >> 16);

        // output goes through a delay line and attenuation
        int32_t output = wrap_add(m_wet[ch].apply(wet), m_dry[ch].apply(dry));

        // DSP round function
        output = (wrap_add(output, 0x2000)) & ~0x3fff;
        m_out[ch] = std::clamp<int32_t>(output >> 14, -0x7fff, 0x7fff);

        if (m_delay_update) {
            m_wet[ch].update();
            m_dry[ch].update();
        }
    }

    m_delay_update = 0;

    // after 6 samples, the next state is executed.
    m_state_counter++;
    if (m_state_counter > 5) {
        m_state_counter = 0;
        m_state = m_next_state;
    }
}

// Apply the FIR filter used as the Q1 transfer function
int32_t QSoundHLE::qsound_fir::apply(int16_t input) {
    int32_t output = 0, tap = 0;
    for (; tap < (m_tap_count - 1); tap++) {
        output = wrap_sub(output, wrap_shl(m_taps[tap] * m_delay_line[m_delay_pos++], 2));

        if (m_delay_pos >= m_tap_count - 1)
            m_delay_pos = 0;
    }

    output = wrap_sub(output, wrap_shl(m_taps[tap] * input, 2));

    m_delay_line[m_delay_pos++] = input;
    if (m_delay_pos >= m_tap_count - 1)
        m_delay_pos = 0;

    return output;
}

// Apply delay line and component volume
int32_t QSoundHLE::qsound_delay::apply(const int32_t input) {
    m_delay_line[m_write_pos++] = input >> 16;
    if (m_write_pos >= 51)
        m_write_pos = 0;

    const int32_t output = m_delay_line[m_read_pos++] * m_volume;
    if (m_read_pos >= 51)
        m_read_pos = 0;

    return output;
}

// Update the delay read position to match new delay length
void QSoundHLE::qsound_delay::update() {
    const int16_t new_read_pos = (m_write_pos - m_delay) % 51;
    if (new_read_pos < 0)
        m_read_pos = new_read_pos + 51;
    else
        m_read_pos = new_read_pos;
}
extern "C" QSoundHLE *qsound_hle_create(const QSoundHLEConfig *c) {
    if (!c || !c->firmware || c->firmware_words != 0x1000 || !c->samples || !c->sample_bytes ||
        (c->clock_hz && c->clock_hz < 2496))
        return nullptr;
    try {
        return new QSoundHLE(*c);
    } catch (...) {
        return nullptr;
    }
}
extern "C" void qsound_hle_destroy(QSoundHLE *q) { delete q; }
extern "C" void qsound_hle_reset(QSoundHLE *q) {
    if (q)
        q->device_reset();
}
extern "C" int qsound_hle_write(QSoundHLE *q, unsigned offset, uint8_t data) {
    if (!q)
        return -1;
    switch (offset) {
    case 0:
        q->m_data_latch = (q->m_data_latch & 0xff) | (uint16_t(data) << 8);
        return 0;
    case 1:
        q->m_data_latch = (q->m_data_latch & 0xff00) | data;
        return 0;
    case 2:
        q->write_data(data, q->m_data_latch);
        ++q->commands;
        return 0;
    default:
        if (q->config.diagnostic)
            q->config.diagnostic(q->config.opaque, "QSound HLE: unknown host write register");
        return -1;
    }
}
extern "C" uint8_t qsound_hle_read(const QSoundHLE *q) { return q ? q->m_ready_flag : 0; }
extern "C" uint64_t qsound_hle_run_cycles(QSoundHLE *q, uint64_t cycles) {
    if (!q)
        return 0;
    uint64_t initial = cycles;
    while (cycles) {
        uint64_t increment = uint64_t(q->rate) * 2;
        uint64_t boundary = (q->clock - q->audio_phase + increment - 1) / increment;
        uint64_t n = std::min(cycles, boundary);
        q->audio_phase += n * increment;
        q->cycles += n;
        cycles -= n;
        if (q->audio_phase >= q->clock) {
            q->audio_phase -= q->clock;
            q->frame();
        }
    }
    return initial;
}
extern "C" uint64_t qsound_hle_run_clocks(QSoundHLE *q, uint64_t clocks) {
    if (!q)
        return 0;
    uint64_t cycles = clocks >> 1;
    unsigned remaining = unsigned(clocks & 1) + q->crystal_remainder;
    cycles += remaining >> 1;
    q->crystal_remainder = remaining & 1;
    return qsound_hle_run_cycles(q, cycles);
}
extern "C" void qsound_hle_run_frames(QSoundHLE *q, uint64_t frames) {
    if (q)
        while (frames--)
            q->frame();
}
extern "C" int qsound_hle_set_clock(QSoundHLE *q, uint32_t clock) {
    if (!q || clock < 2496)
        return -1;
    q->audio_phase = (q->audio_phase * clock) / q->clock;
    q->clock = clock;
    q->rate = clock / 2496;
    return 0;
}
extern "C" int qsound_hle_set_sample_region_bank(QSoundHLE *q, unsigned bank) {
    if (!q || q->config.sample_bytes < 0x1000000U)
        return -1;
    q->region_bank = bank % q->region_banks;
    return 0;
}
extern "C" int qsound_hle_read_register(const QSoundHLE *q, unsigned addr, uint16_t *v) {
    if (!q || !v || addr > 255)
        return -1;
    if (!q->m_register_map[addr])
        return -1;
    *v = *q->m_register_map[addr];
    return 0;
}
extern "C" int qsound_hle_get_status(const QSoundHLE *q, QSoundHLEStatus *s) {
    if (!q || !s)
        return -1;
    *s = {};
    s->frames = q->frames;
    s->cycles = q->cycles;
    s->commands = q->commands;
    s->clock_hz = q->clock;
    s->sample_rate = q->rate;
    s->sample_region_bank = q->region_bank;
    s->state = q->m_state;
    s->next_state = q->m_next_state;
    s->data_latch = q->m_data_latch;
    s->echo_end = q->m_echo.m_end_pos;
    s->echo_length = q->m_echo.m_length;
    s->delay_update = q->m_delay_update;
    std::copy(q->m_out, q->m_out + 2, s->output);
    std::copy(q->m_voice_output, q->m_voice_output + 19, s->voice_output);
    s->ready = q->m_ready_flag;
    s->state_counter = q->m_state_counter;
    s->crystal_remainder = q->crystal_remainder;
    for (int i = 0; i < 2; ++i) {
        s->filter_table[i] = q->m_filter[i].m_table_pos;
        s->alt_filter_table[i] = q->m_alt_filter[i].m_table_pos;
        s->filter_taps[i] = q->m_filter[i].m_tap_count;
        s->alt_filter_taps[i] = q->m_alt_filter[i].m_tap_count;
    }
    return 0;
}
