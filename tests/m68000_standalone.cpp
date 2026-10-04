#include "m68000.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <limits>

struct Bus {
    struct Write { uint32_t address; uint16_t data, mask; M68kSpace space; };
    std::vector<uint8_t> memory = std::vector<uint8_t>(1u << 24, 0);
    std::vector<Write> writes;
    M68000 *owner = nullptr;
    uint32_t reads = 0;
    uint32_t fail_address = 0xffffffff;
    uint32_t fail_write_address = 0xffffffff;
    bool fail_write_once = false;
    unsigned failed_reads = 0;
    unsigned failed_writes = 0;
    unsigned critical_write_attempts = 0;
    bool tried_reentry = false;
    int64_t nested_run_result = 0;
};

static bool read_word(void *opaque, M68kSpace space, uint32_t address,
                      uint16_t mask, uint16_t *value)
{
    Bus *bus = static_cast<Bus *>(opaque);
    (void)mask;
    ++bus->reads;
    if (bus->owner && !bus->tried_reentry) {
        bus->tried_reentry = true;
        bus->nested_run_result = bus->owner->run(1);
        bus->owner->reset();
        bus->owner->set_irq_ack([](unsigned) {});
        bus->owner->set_irq(2, true);  // IRQ input changes are callback-safe
        bus->owner->set_irq(2, false);
        assert(bus->owner->callback_fault());
    }
    if (space == M68kSpace::Cpu)
        return false;
    if (address == bus->fail_address) {
        ++bus->failed_reads;
        return false;
    }
    if (address + 1 >= bus->memory.size())
        return false;
    *value = (uint16_t)((bus->memory[address] << 8) | bus->memory[address + 1]);
    return true;
}

static bool write_word(void *opaque, M68kSpace space, uint32_t address,
                       uint16_t data, uint16_t mask)
{
    Bus *bus = static_cast<Bus *>(opaque);
    if (bus->fail_write_once && address == bus->fail_write_address)
        ++bus->critical_write_attempts;
    if (bus->fail_write_once && address == bus->fail_write_address) {
        bus->fail_write_once = false;
        ++bus->failed_writes;
        return false;
    }
    if (address + 1 >= bus->memory.size())
        return false;
    if (mask & 0xff00)
        bus->memory[address] = (uint8_t)(data >> 8);
    if (mask & 0x00ff)
        bus->memory[address + 1] = (uint8_t)data;
    bus->writes.push_back({address, data, mask, space});
    return true;
}

static void put16(Bus &bus, uint32_t address, uint16_t value)
{
    bus.memory[address] = (uint8_t)(value >> 8);
    bus.memory[address + 1] = (uint8_t)value;
}

static void put32(Bus &bus, uint32_t address, uint32_t value)
{
    put16(bus, address, (uint16_t)(value >> 16));
    put16(bus, address + 2, (uint16_t)value);
}

static M68000 make_cpu(Bus &bus)
{
    M68000Bus interface{&bus, read_word, write_word};
    return M68000(interface);
}

static void load_reset_vectors(Bus &bus, uint32_t stack = 0x7000,
                               uint32_t pc = 0x100)
{
    put32(bus, 0, stack);
    put32(bus, 4, pc);
}

static void test_reset_alu_mmio_and_slice_equivalence()
{
    Bus a, b;
    load_reset_vectors(a);
    load_reset_vectors(b);
    const uint16_t program[] = {
        0x7005,             // MOVEQ #5,D0
        0x7203,             // MOVEQ #3,D1
        0xd081,             // ADD.W D1,D0
        0x33c0, 0x0000, 0x0200, // MOVE.W D0,$000200
        0x13c0, 0x0000, 0x0202, // MOVE.B D0,$000202 (upper lane)
        0x13c0, 0x0000, 0x0203, // MOVE.B D0,$000203 (lower lane)
        0x5280,             // ADDQ.L #1,D0
        0x23c0, 0x0000, 0x0204, // MOVE.L D0,$000204
        0x60fe              // BRA.S *
    };
    for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i) {
        put16(a, 0x100 + i * 2, program[i]);
        put16(b, 0x100 + i * 2, program[i]);
    }
    M68000 cpu_a = make_cpu(a), cpu_b = make_cpu(b);
    cpu_a.run(300);
    for (unsigned i = 0; i < 300; ++i)
        cpu_b.run(1);
    assert(cpu_a.pc() == cpu_b.pc());
    assert(cpu_a.sr() == cpu_b.sr());
    assert(cpu_a.data_register(0) == cpu_b.data_register(0));
    assert(cpu_a.data_register(0) == 9);
    assert(a.memory[0x200] == 0 && a.memory[0x201] == 8);
    assert(a.memory[0x202] == 8 && a.memory[0x203] == 8);
    assert(a.memory[0x204] == 0 && a.memory[0x205] == 0 &&
           a.memory[0x206] == 0 && a.memory[0x207] == 9);
    bool saw_mmio_lane = false;
    bool saw_upper_byte = false, saw_lower_byte = false;
    for (const auto &write : a.writes)
        if (write.address == 0x200 && write.mask == 0xffff &&
            write.space == M68kSpace::Program)
            saw_mmio_lane = true;
        else if (write.address == 0x202 && write.mask == 0xff00)
            saw_upper_byte = true;
        else if (write.address == 0x202 && write.mask == 0x00ff)
            saw_lower_byte = true;
    assert(saw_mmio_lane);
    assert(saw_upper_byte && saw_lower_byte);
}

static void test_stop_irq_autovector_rte()
{
    Bus bus;
    load_reset_vectors(bus, 0x7000, 0x100);
    const uint16_t main_program[] = {
        0x46fc, 0x2000, // MOVE #$2000,SR (unmask interrupts)
        0x4e72, 0x2000, // STOP #$2000
        0x60fe          // BRA.S * after RTE
    };
    for (unsigned i = 0; i < sizeof(main_program) / sizeof(main_program[0]); ++i)
        put16(bus, 0x100 + i * 2, main_program[i]);
    put32(bus, 0x6c, 0x200); // level-3 autovector 27 * 4
    put16(bus, 0x200, 0x5242); // ADDQ.W #1,D2
    put16(bus, 0x202, 0x4e73); // RTE
    M68000 cpu = make_cpu(bus);
    cpu.run(200);
    assert(cpu.stopped());
    assert(cpu.address_register(7) == 0x7000);
    unsigned acks = 0;
    cpu.set_irq_ack([&](unsigned level) {
        assert(level == 3);
        ++acks;
        assert(cpu.run(1) == -2);
        cpu.reset();
        cpu.set_irq_ack([](unsigned) {});
        cpu.set_irq(3, false);
    });
    cpu.set_irq(3, true);
    cpu.run(300);
    assert(acks == 1);
    assert(cpu.callback_fault());
    assert(cpu.data_register(2) == 1);
    assert(cpu.pc() >= 0x104 && cpu.pc() <= 0x10a);
    assert(cpu.address_register(7) == 0x7000); // RTE restored the exception frame
    bool saw_exception_stack = false;
    for (const auto &write : bus.writes)
        if (write.address == 0x6ffa || write.address == 0x6ffc)
            saw_exception_stack = true;
    assert(saw_exception_stack);
}

static void test_bus_error()
{
    Bus bus;
    load_reset_vectors(bus, 0x7000, 0x100);
    bus.fail_address = 0x3000;
    put16(bus, 0x100, 0x3039); // MOVE.W ($3000).L,D0
    put16(bus, 0x102, 0x0000);
    put16(bus, 0x104, 0x3000);
    put32(bus, 8, 0x200); // bus-error exception vector
    put16(bus, 0x200, 0x60fe);
    M68000 cpu = make_cpu(bus);
    cpu.run(300);
    assert(bus.failed_reads != 0);
    assert(cpu.bus_error());
    assert(cpu.pc() >= 0x200 && cpu.pc() <= 0x202); // reached bus-error vector
    assert(cpu.address_register(7) < 0x7000);       // a real supervisor frame was stacked
    bool saw_frame = false;
    for (const auto &write : bus.writes)
        if (write.address >= 0x6fe0 && write.address < 0x7000)
            saw_frame = true;
    assert(saw_frame);
}

static void test_flags_shift_movem_and_privilege()
{
    Bus bus;
    load_reset_vectors(bus);
    const uint16_t p[] = {
        0x303c, 0x7fff,             // MOVE.W #$7fff,D0
        0x5240,                     // ADDQ.W #1,D0 => N,V
        0x40f9, 0x0000, 0x0300,    // MOVE SR,$300
        0x72ff,                     // MOVEQ #-1,D1
        0x5201,                     // ADDQ.B #1,D1 => X,Z
        0x40f9, 0x0000, 0x0302,    // MOVE SR,$302
        0x7440,                     // MOVEQ #$40,D2
        0xe30a,                     // LSL.B #1,D2 => N,V
        0x40f9, 0x0000, 0x0304,    // MOVE SR,$304
        0x203c, 0x1234, 0x5678,    // MOVE.L #$12345678,D0
        0x223c, 0xaabb, 0xccdd,    // MOVE.L #$aabbccdd,D1
        0x48f9, 0x0003, 0x0000, 0x0310, // MOVEM.L D0-D1,$310
        0x46fc, 0x0000,             // switch to user mode
        0x46fc, 0x2000,             // user-mode MOVE to SR => privilege vector
        0x60fe
    };
    for (unsigned i = 0; i < sizeof(p) / sizeof(p[0]); ++i)
        put16(bus, 0x100 + i * 2, p[i]);
    put32(bus, 0x20, 0x240); // privilege violation vector 8
    put16(bus, 0x240, 0x7655); // MOVEQ #$55,D3
    put16(bus, 0x242, 0x60fe);
    M68000 cpu = make_cpu(bus);
    cpu.run(700);
    assert(cpu.data_register(0) == 0x12345678);
    assert(cpu.data_register(1) == 0xaabbccdd);
    assert(cpu.data_register(2) == 0x00000080);
    assert(cpu.data_register(3) == 0x55);
    assert((bus.memory[0x301] & 0x1f) == 0x0a); // N|V after signed overflow
    assert((bus.memory[0x303] & 0x1f) == 0x15); // X|Z|C after byte carry
    assert((bus.memory[0x305] & 0x1f) == 0x08); // LSL sets N; shifted-out bit was zero
    assert(bus.memory[0x310] == 0x12 && bus.memory[0x311] == 0x34 &&
           bus.memory[0x312] == 0x56 && bus.memory[0x313] == 0x78);
    assert(bus.memory[0x314] == 0xaa && bus.memory[0x315] == 0xbb &&
           bus.memory[0x316] == 0xcc && bus.memory[0x317] == 0xdd);
    assert(cpu.sr() & 0x2000); // privilege exception ran on supervisor stack
}

static void test_address_error_and_nmi()
{
    Bus addr;
    load_reset_vectors(addr);
    put16(addr, 0x100, 0x3039); // MOVE.W ($3001).L,D0 (odd word access)
    put16(addr, 0x102, 0x0000);
    put16(addr, 0x104, 0x3001);
    put32(addr, 12, 0x220); // address error vector 3
    put16(addr, 0x220, 0x7666); // MOVEQ #$66,D3 proves vector handler ran
    put16(addr, 0x222, 0x60fe);
    M68000 address_cpu = make_cpu(addr);
    address_cpu.run(300);
    assert(address_cpu.pc() >= 0x220 && address_cpu.pc() <= 0x224);
    assert(address_cpu.data_register(3) == 0x66);
    assert(address_cpu.address_register(7) < 0x7000);

    Bus nmi;
    load_reset_vectors(nmi);
    put16(nmi, 0x100, 0x46fc); // MOVE #$2000,SR
    put16(nmi, 0x102, 0x2000);
    put16(nmi, 0x104, 0x4e72); // STOP #$2000
    put16(nmi, 0x106, 0x2000);
    put16(nmi, 0x108, 0x60fe);
    put32(nmi, 31 * 4, 0x260); // level-7 autovector
    put16(nmi, 0x260, 0x5242); // ADDQ.W #1,D2
    put16(nmi, 0x262, 0x4e73); // RTE
    M68000 nmi_cpu = make_cpu(nmi);
    nmi_cpu.run(160);
    assert(nmi_cpu.stopped());
    unsigned acks = 0;
    nmi_cpu.set_irq_ack([&](unsigned level) {
        assert(level == 7);
        ++acks;
        assert(nmi_cpu.run(1) == -2);
        nmi_cpu.reset();
        nmi_cpu.set_irq_ack([](unsigned) {});
        nmi_cpu.set_irq(7, false);
    });
    nmi_cpu.set_irq(7, true);
    nmi_cpu.run(220);
    assert(acks == 1 && nmi_cpu.data_register(2) == 1);
    assert(nmi_cpu.callback_fault());
}

static void test_critical_stack_bus_error_literal_path()
{
    Bus bus;
    load_reset_vectors(bus);
    bus.fail_address = 0x3000;
    put16(bus, 0x100, 0x3039); // trigger bus error on data read
    put16(bus, 0x102, 0x0000);
    put16(bus, 0x104, 0x3000);
    put32(bus, 8, 0x200);
    put16(bus, 0x200, 0x60fe);
    // The first write at the format/vector stack location itself also fails.
    bus.fail_write_address = 0x6ffa;
    bus.fail_write_once = true;
    M68000 cpu = make_cpu(bus);
    cpu.run(400);
    assert(bus.failed_writes == 1);
    assert(bus.critical_write_attempts == 1);
    assert(cpu.bus_error());
    // The generated MAME core retries by entering its bus-error state again;
    // it does not synthesize a second/double-fault path for callback failure.
    assert(cpu.pc() >= 0x200 && cpu.pc() <= 0x202);
}

static void test_callback_guards_and_cycle_overflow(void)
{
    Bus bus;
    load_reset_vectors(bus);
    put16(bus, 0x100, 0x4e71); // NOP
    M68000Bus interface{&bus, read_word, write_word};
    M68000 cpu(interface);
    bus.owner = &cpu;
    cpu.set_irq_ack([&](unsigned level) {
        assert(level == 3);
        cpu.set_irq(3, false); // the ack callback may lower its line
    });
    cpu.run(120);
    assert(bus.tried_reentry);
    assert(bus.nested_run_result == -2);
    assert(cpu.callback_fault());

    Bus overflow_bus;
    load_reset_vectors(overflow_bus);
    M68000Bus near_limit{&overflow_bus, read_word, write_word, true,
                         std::numeric_limits<uint64_t>::max() - 3};
    M68000 overflow_cpu(near_limit);
    const uint64_t before = overflow_cpu.total_cycles();
    assert(overflow_cpu.run(4) == -1);
    assert(overflow_cpu.total_cycles() == before);
    assert(overflow_cpu.pc() == 0 && overflow_bus.reads == 0);
    assert(overflow_cpu.run(3) == 3);
    assert(overflow_cpu.total_cycles() == std::numeric_limits<uint64_t>::max());
}

int main()
{
    test_reset_alu_mmio_and_slice_equivalence();
    test_stop_irq_autovector_rte();
    test_bus_error();
    test_flags_shift_movem_and_privilege();
    test_address_error_and_nmi();
    test_critical_stack_bus_error_literal_path();
    test_callback_guards_and_cycle_overflow();
    std::puts("MAME MC68000 reset/ALU/bus/IRQ/STOP tests PASS");
    return 0;
}
