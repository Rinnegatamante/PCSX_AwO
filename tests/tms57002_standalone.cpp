#include "tms57002.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

struct Fixture {
    tms57002_t *dsp = nullptr;
    std::array<uint8_t, 1u << 20> ext{};
    unsigned reads = 0;
    unsigned writes = 0;
    uint64_t read_cycles[8]{};
    uint32_t read_addresses[8]{};
    uint64_t write_cycles[8]{};
    uint32_t write_addresses[8]{};
    int dready = -1, pc0 = -1, empty = -1;
    bool reenter_on_read = false;
    int64_t nested_run = 0;
    bool reenter_on_dready = false;
    int64_t line_nested_run = 0;
};

static uint8_t external_read(void *opaque, uint32_t address)
{
    Fixture *f = static_cast<Fixture *>(opaque);
    assert(address < f->ext.size());
    if (f->reads < 8) {
        f->read_cycles[f->reads] = tms57002_current_cycle(f->dsp);
        f->read_addresses[f->reads] = address;
    }
    ++f->reads;
    if (f->reenter_on_read) {
        f->reenter_on_read = false;
        f->nested_run = tms57002_run(f->dsp, 1);
        tms57002_reset(f->dsp);
        assert(tms57002_program_write(f->dsp, 4, 0) == -1);
        tms57002_data_w(f->dsp, 0);
    }
    return f->ext[address];
}

static void external_write(void *opaque, uint32_t address, uint8_t value)
{
    Fixture *f = static_cast<Fixture *>(opaque);
    assert(address < f->ext.size());
    if (f->writes < 8) {
        f->write_cycles[f->writes] = tms57002_current_cycle(f->dsp);
        f->write_addresses[f->writes] = address;
    }
    f->ext[address] = value;
    ++f->writes;
}

static void dready_line(void *opaque, int state)
{
    Fixture *f = static_cast<Fixture *>(opaque);
    f->dready = state;
    if (f->reenter_on_dready && f->dsp) {
        f->reenter_on_dready = false;
        f->line_nested_run = tms57002_run(f->dsp, 1);
        tms57002_reset(f->dsp);
        tms57002_data_w(f->dsp, 0);
    }
}

static void pc0_line(void *opaque, int state)
{
    static_cast<Fixture *>(opaque)->pc0 = state;
}

static void empty_line(void *opaque, int state)
{
    static_cast<Fixture *>(opaque)->empty = state;
}

static tms57002_t *create(Fixture &f, uint64_t origin = 0)
{
    tms57002_config_t config{};
    config.opaque = &f;
    config.cycle_origin = origin;
    config.data_read = external_read;
    config.data_write = external_write;
    config.dready_changed = dready_line;
    config.pc0_changed = pc0_line;
    config.empty_changed = empty_line;
    f.dsp = tms57002_create(&config);
    assert(f.dsp);
    return f.dsp;
}

static void send24(tms57002_t *dsp, uint32_t value)
{
    tms57002_data_w(dsp, static_cast<uint8_t>(value >> 16));
    tms57002_data_w(dsp, static_cast<uint8_t>(value >> 8));
    tms57002_data_w(dsp, static_cast<uint8_t>(value));
}

static void upload_program(tms57002_t *dsp, uint32_t st0, uint32_t st1,
                           const uint32_t *program, unsigned words)
{
    tms57002_pload_w(dsp, 0); // asserted-low PLOAD enters serial program load
    send24(dsp, st0);
    send24(dsp, st1);
    for (unsigned i = 0; i < words; ++i)
        send24(dsp, program[i]);
    tms57002_pload_w(dsp, 1);
}

static void upload_cvalue(tms57002_t *dsp, uint8_t address, uint32_t value)
{
    tms57002_cload_w(dsp, 0); // asserted-low CLOAD
    tms57002_data_w(dsp, address);
    tms57002_data_w(dsp, static_cast<uint8_t>(value >> 24));
    tms57002_data_w(dsp, static_cast<uint8_t>(value >> 16));
    tms57002_data_w(dsp, static_cast<uint8_t>(value >> 8));
    tms57002_data_w(dsp, static_cast<uint8_t>(value));
    tms57002_cload_w(dsp, 1);
}

static void test_host_pload_cpload_status_and_reset()
{
    Fixture f;
    tms57002_t *dsp = create(f);
    const uint32_t idle[] = {0xfc4000}; // category-3 IDLE
    upload_program(dsp, 0, 0, idle, 1);
    assert(tms57002_program_read(dsp, 0) == idle[0]);
    assert(tms57002_pc(dsp) == 1);
    assert(tms57002_pc0_r(dsp) == 1);
    assert(tms57002_empty_r(dsp) == 1);
    assert(f.pc0 == 1 && f.empty == 1);
    tms57002_sync_w(dsp, 1);
    assert(tms57002_run(dsp, 5) == 5);
    assert(tms57002_pc(dsp) == 1);
    assert(tms57002_total_cycles(dsp) == 5);

    upload_cvalue(dsp, 0x20, 0x12345678);
    assert(tms57002_empty_r(dsp) == 0);
    // LPC C direct address 0x20 makes a four-byte host response available.
    const uint32_t response_program[] = {0x018d20, 0xfc4000};
    assert(tms57002_program_write(dsp, 0, response_program[0]) == 0);
    assert(tms57002_program_write(dsp, 1, response_program[1]) == 0);
    f.reenter_on_dready = true;
    tms57002_sync_w(dsp, 1);
    assert(tms57002_run(dsp, 1) == 1);
    assert(f.line_nested_run == -2 && tms57002_callback_fault(dsp));
    assert(tms57002_dready_r(dsp) == 0 && f.dready == 0);
    assert(tms57002_data_r(dsp) == 0x12);
    assert(tms57002_data_r(dsp) == 0x34);
    assert(tms57002_data_r(dsp) == 0x56);
    assert(tms57002_data_r(dsp) == 0x78);
    assert(tms57002_dready_r(dsp) == 1 && f.dready == 1);
    tms57002_reset(dsp);
    assert(tms57002_program_read(dsp, 0) == response_program[0]); // reset preserves RAM
    assert(tms57002_pc(dsp) == 0);
    assert(tms57002_empty_r(dsp) == 1); // device_reset clears pending CLOAD writes
    tms57002_destroy(dsp);
}

static void test_program_and_coefficient_memory_are_independent()
{
    Fixture f;
    tms57002_t *dsp = create(f);
    const uint32_t program[] = {0x480500, 0xfc4000}; // LACC C[0], IDLE
    upload_program(dsp, 0, 0, program, 2);
    upload_cvalue(dsp, 0, 0x12345678);
    tms57002_sync_w(dsp, 1);
    assert(tms57002_run(dsp, 2) == 2);
    assert(tms57002_aacc(dsp) == 0x12345678);
    assert(tms57002_program_read(dsp, 0) == program[0]);
    // Reloading instructions at the same address must retain coefficients.
    tms57002_reset(dsp);
    upload_program(dsp, 0, 0, program, 2);
    tms57002_sync_w(dsp, 1);
    assert(tms57002_run(dsp, 2) == 2);
    assert(tms57002_aacc(dsp) == 0x12345678);
    assert(tms57002_program_read(dsp, 0) == program[0]);
    tms57002_destroy(dsp);
}

static void load_constant(Fixture &f, uint32_t value)
{
    upload_cvalue(f.dsp, 0x10, value);
}

static void test_accumulator_audio_and_slice_equivalence()
{
    Fixture whole, sliced;
    tms57002_t *a = create(whole);
    tms57002_t *b = create(sliced);
    load_constant(whole, 0x123456);
    load_constant(sliced, 0x123456);
    const uint32_t program[] = {
        0x480510, // LACC C direct 0x10
        0,        // NOP pipeline cycle
        0xfc4000  // IDLE
    };
    upload_program(a, 0, 0, program, 3);
    upload_program(b, 0, 0, program, 3);
    tms57002_sync_w(a, 1);
    tms57002_sync_w(b, 1);
    assert(tms57002_run(a, 3) == 3);
    assert(tms57002_run(b, 1) == 1);
    assert(tms57002_run(b, 1) == 1);
    assert(tms57002_run(b, 1) == 1);
    assert(tms57002_aacc(a) == 0x123456);
    assert(tms57002_aacc(b) == tms57002_aacc(a));
    assert(tms57002_pc(a) == tms57002_pc(b));

    tms57002_destroy(a);
    tms57002_destroy(b);

    // An uploaded LMHC + DOMH routine sends a signed accumulator result to
    // output 0 using the upstream two-stage MAC pipeline.
    Fixture tone;
    tms57002_t *tone_dsp = create(tone);
    load_constant(tone, 0x123456);
    const uint32_t audio_program[] = {
        0xcc0510, // LMHC C direct 0x10
        0x010000, // DOMH SO0_L (pre-category-2)
        0xfc4000  // IDLE
    };
    upload_program(tone_dsp, 0, 0, audio_program, 3);
    tms57002_sync_w(tone_dsp, 1);
    tms57002_run(tone_dsp, 3);
    const float zero[4] = {};
    float output[4] = {};
    assert(tms57002_process_sample(tone_dsp, zero, output) == 0);
    assert(std::isfinite(output[0]) && output[0] > 0.0f);
    assert(output[1] == 0.0f && output[2] == 0.0f && output[3] == 0.0f);
    tms57002_destroy(tone_dsp);
}

static void test_mac_serial_input_audio_and_saturation()
{
    Fixture f;
    tms57002_t *dsp = create(f);
    upload_cvalue(dsp, 0, 0x007fffff);
    const uint32_t program[] = {
        0x008100, // DIS SI0_L,D0[0]
        0x840100, // MPY D0[0],C[CA]
        0,
        0,
        0x010000, // DOMH SO0_L
        0xfc4000  // IDLE
    };
    upload_program(dsp, 1, 0, program, 6);
    const float input[4] = {1.0f, 0, 0, 0};
    float ignored[4]{};
    assert(tms57002_process_sample(dsp, input, ignored) == 0);
    assert(tms57002_run(dsp, 6) == 6);
    assert(tms57002_macc(dsp) == 2147483392LL);
    float output[4]{};
    assert(tms57002_process_sample(dsp, input, output) == 0);
    assert(output[0] > 1.50e-5f && output[0] < 1.53e-5f);
    assert(output[1] == 0 && output[2] == 0 && output[3] == 0);
    tms57002_destroy(dsp);

    Fixture saturated, wrapping;
    tms57002_t *sat = create(saturated);
    tms57002_t *wrap = create(wrapping);
    upload_cvalue(sat, 0x10, 0x7fffffff);
    upload_cvalue(wrap, 0x10, 0x7fffffff);
    const uint32_t add_program[] = {
        0x480510, // LACC C direct 0x10
        0x100510, // ADD C direct 0x10,A
        0xfc4000
    };
    upload_program(sat, 0, 8, add_program, 3); // ST1_AOVM saturating mode
    upload_program(wrap, 0, 0, add_program, 3);
    tms57002_sync_w(sat, 1);
    tms57002_sync_w(wrap, 1);
    tms57002_run(sat, 3);
    tms57002_run(wrap, 3);
    assert(tms57002_aacc(sat) == 0x7fffffff);
    assert((tms57002_st1(sat) & 1) != 0);
    assert(tms57002_aacc(wrap) == 0xfffffffe);
    assert((tms57002_st1(wrap) & 1) != 0);
    tms57002_destroy(sat);
    tms57002_destroy(wrap);
}

static void test_external_memory_read_and_callback_guard_timestamp()
{
    Fixture f;
    tms57002_t *dsp = create(f);
    f.ext[0x100] = 0x12;
    f.ext[0x101] = 0x34;
    // SEL transfers one 16-bit word in two external byte cycles. The unusual
    // XOA value compensates for MAME's LRCLK XBA decrement and address scaling.
    const uint32_t program[] = {
        0xe40510, // RDE C direct 0x10
        0,        // external transfer advances one byte per DSP cycle
        0,
        0x07902,  // SRBD D0[2]
        0xfc4000  // IDLE
    };
    upload_cvalue(dsp, 0x10, 0x00080081);
    upload_program(dsp, 0x8001, 0, program, 5); // select 16-bit, preserve D addressing
    tms57002_sync_w(dsp, 1);
    f.reenter_on_read = true;
    assert(tms57002_run(dsp, 8) == 8);
    assert(f.reads == 2);
    assert(f.read_cycles[0] < 8 && f.read_cycles[1] < 8);
    assert(f.read_cycles[0] == 1 && f.read_cycles[1] == 2);
    assert(f.nested_run == -2);
    assert(tms57002_callback_fault(dsp));
    assert(tms57002_dmem_read(dsp, 0, 2) == 0x123400);
    tms57002_destroy(dsp);
}

static void test_external_memory_write()
{
    Fixture f;
    tms57002_t *dsp = create(f);
    upload_cvalue(dsp, 0x10, 0x00080081);
    const uint32_t program[] = {
        0x008100, // DIS SI0_L,D0[0]
        0xe00510, // WRE D0[ID],C direct 0x10
        0,
        0,
        0xfc4000
    };
    upload_program(dsp, 0x8001, 0, program, 5);
    const float input[4] = {1.0f, 0, 0, 0};
    float output[4]{};
    assert(tms57002_process_sample(dsp, input, output) == 0);
    assert(tms57002_run(dsp, 8) == 8);
    assert(f.writes == 2);
    assert(f.write_addresses[0] == 0x100 && f.write_addresses[1] == 0x101);
    assert(f.write_cycles[0] == 2 && f.write_cycles[1] == 3);
    assert(f.ext[0x100] == 0x00 && f.ext[0x101] == 0x80);
    tms57002_destroy(dsp);
}

static void test_cycle_overflow_and_sample_validation()
{
    Fixture f;
    tms57002_t *dsp = create(f);
    const float bad[4] = {NAN, 0, 0, 0};
    float output[4]{};
    assert(tms57002_process_sample(dsp, bad, output) == -1);
    assert(tms57002_total_cycles(dsp) == 0);
    assert(tms57002_run(dsp, std::numeric_limits<uint64_t>::max()) == -1);
    assert(tms57002_total_cycles(dsp) == 0);
    assert(tms57002_run(dsp, 11) == 11);
    assert(tms57002_total_cycles(dsp) == 11);
    tms57002_reset(dsp);
    assert(tms57002_total_cycles(dsp) == 11); // external clock is monotonic over reset
    tms57002_destroy(dsp);
}

static void test_absolute_clock_overflow_is_atomic()
{
    Fixture f;
    const uint64_t end = UINT64_MAX;
    tms57002_t *dsp = create(f, end - 3);
    const uint32_t program[] = {0x480500, 0xfc4000};
    upload_program(dsp, 0, 0, program, 2);
    upload_cvalue(dsp, 0, 0x55);
    tms57002_sync_w(dsp, 1);
    const auto pc = tms57002_pc(dsp);
    assert(tms57002_run(dsp, 4) == -1);
    assert(tms57002_total_cycles(dsp) == end - 3);
    assert(tms57002_pc(dsp) == pc && tms57002_aacc(dsp) == 0);
    assert(tms57002_run(dsp, 3) == 3);
    assert(tms57002_total_cycles(dsp) == end);
    assert(tms57002_aacc(dsp) == 0x55);
    assert(tms57002_run(dsp, 1) == -1);
    assert(tms57002_run(dsp, 0) == 0);
    assert(tms57002_current_cycle(dsp) == end);
    tms57002_destroy(dsp);
}

static void test_unknown_opcode_is_reported()
{
    Fixture f;
    tms57002_t *dsp = create(f);
    const uint32_t program[] = {0xf80000, 0xfc4000}; // cat-1 slot absent in source list
    upload_program(dsp, 0, 0, program, 2);
    tms57002_sync_w(dsp, 1);
    assert(tms57002_run(dsp, 1) == 1);
    assert(tms57002_unsupported_instruction(dsp));
    tms57002_destroy(dsp);
}

int main()
{
    test_host_pload_cpload_status_and_reset();
    test_program_and_coefficient_memory_are_independent();
    test_accumulator_audio_and_slice_equivalence();
    test_mac_serial_input_audio_and_saturation();
    test_external_memory_read_and_callback_guard_timestamp();
    test_external_memory_write();
    test_cycle_overflow_and_sample_validation();
    test_absolute_clock_overflow_is_atomic();
    test_unknown_opcode_is_reported();
    std::puts("MAME TMS57002 DASP host/program/audio/memory tests PASS");
}
