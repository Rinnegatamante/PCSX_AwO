// SPDX-License-Identifier: BSD-3-Clause
#include "../libpcsxcore/arcade/h8.h"
#include <cassert>
#include <cstdio>
#include <vector>

struct Access { uint32_t address; uint64_t timestamp, committed; bool edge; };
struct Bus {
    H8CPU *cpu=nullptr;
    std::vector<uint8_t> memory=std::vector<uint8_t>(0x10000,0);
    std::vector<Access> accesses;
    bool irq_edge=false, adc_trigger=false;
    uint64_t adc_sample_cycle=0;
};
static uint8_t read8(void *p,uint32_t a) {
    auto &b=*static_cast<Bus*>(p);
    if(a==0x800) {
        H8Status s{};h8_get_status(b.cpu,&s);
        b.accesses.push_back({a,h8_current_cycle(b.cpu),s.cycles,b.irq_edge});
        return b.irq_edge?0x22:0x11;
    }
    return b.memory[a&0xffff];
}
static void write8(void *p,uint32_t a,uint8_t v) {static_cast<Bus*>(p)->memory[a&0xffff]=v;}
static uint16_t adc_read(void *p,unsigned) {
    auto &b=*static_cast<Bus*>(p);b.adc_trigger=true;b.adc_sample_cycle=h8_current_cycle(b.cpu);return 0x155;
}
static void word(Bus &b,uint32_t a,uint16_t v) {b.memory[a]=v>>8;b.memory[a+1]=v;}
static H8CPU *create(Bus &b) {
    word(b,0,0);word(b,2,0x100);
    // MOV.L #$00000800,ER0; MOV.B @ER0,R0L; SLEEP.
    word(b,0x100,0x7a00);word(b,0x102,0);word(b,0x104,0x0800);
    word(b,0x106,0x6800);word(b,0x108,0x0180);
    H8Config cfg{};cfg.opaque=&b;cfg.read8=read8;cfg.write8=write8;cfg.adc_read=adc_read;
    b.cpu=h8_create(&cfg);assert(b.cpu);return b.cpu;
}
static H8Status status(H8CPU *c) {H8Status s{};h8_get_status(c,&s);return s;}

static void resume_after_external_edge() {
    Bus b;H8CPU *c=create(b);
    assert(h8_run_until(c,14)==0);
    assert(status(c).cycles==14&&b.accesses.empty());
    // The external IRQ/ADTRG edge is delivered at the deadline before the
    // pending data access is resumed.
    b.irq_edge=true;h8_set_irq(c,1,1);
    h8_write_byte(c,0xffffe9,0x80);
    h8_adc_trigger(c,1);h8_adc_trigger(c,0);
    assert(b.adc_trigger&&b.adc_sample_cycle==14);
    assert(h8_run_until(c,20)==0);
    assert(b.accesses.size()==1&&b.accesses[0].edge);
    assert(b.accesses[0].timestamp>=16&&b.accesses[0].committed<=16);
    assert(((status(c).er[0]>>8)&0xff)==0x22);
    h8_destroy(c);
}

static void source_bus_overshoot_is_carried() {
    Bus b;H8CPU *c=create(b);
    // The data read itself costs two cycles and its source callback runs at
    // cycle 16. MAME's generated checkpoint returns after that single access,
    // so cycle 15 is committed while one cycle of CPU charge remains.
    assert(h8_run_until(c,15)==0);
    assert(status(c).cycles==15&&b.accesses.size()==1);
    assert(b.accesses[0].timestamp==16&&b.accesses[0].committed<16&&!b.accesses[0].edge);
    b.irq_edge=true;h8_set_irq(c,1,1);
    assert(h8_run_until(c,17)==0);
    assert(status(c).cycles==17&&b.accesses.size()==1);
    assert(((status(c).er[0]>>8)&0xff)==0x11); // Resume does not repeat the access.
    h8_destroy(c);

    // The legacy whole-state entry point can safely finish a partial state.
    Bus mixed; c=create(mixed);
    assert(h8_run_until(c,15)==0&&mixed.accesses.size()==1);
    mixed.irq_edge=true;h8_set_irq(c,1,1);h8_run_cycles(c,2);
    assert(mixed.accesses.size()==1&&!status(c).halted);
    h8_destroy(c);
}

static void adc_deadline_is_monotonic() {
    Bus b;H8CPU *c=create(b);
    assert(h8_run_until(c,40)==0);
    h8_write_byte(c,0xffffe8,3);h8_write_byte(c,0xffffe9,0x80);
    h8_adc_trigger(c,1);
    assert(h8_run_until(c,50)==0);
    h8_adc_trigger(c,0);
    assert(b.adc_trigger&&b.adc_sample_cycle==50);
    assert(h8_run_until(c,315)==0);
    assert(!(h8_read_byte(c,0xffffe8)&0x80));
    assert(h8_run_until(c,316)==0);
    assert(h8_read_byte(c,0xffffe8)&0x80);
    assert(h8_read_byte(c,0xffffe6)==0x55&&h8_read_byte(c,0xffffe7)==0x40);
    h8_destroy(c);
}

int main() {
    resume_after_external_edge();source_bus_overshoot_is_carried();adc_deadline_is_monotonic();
    std::puts("H8 absolute deadline, bus continuation and ADTRG tests passed");
}
