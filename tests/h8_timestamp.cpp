// SPDX-License-Identifier: BSD-3-Clause
#include "../libpcsxcore/arcade/h8.h"
#include <cassert>
#include <cstdio>
#include <vector>

struct Access {
    char kind;
    uint32_t address;
    uint64_t current, committed;
    bool operator==(const Access &o) const {
        return kind==o.kind&&address==o.address&&current==o.current&&committed==o.committed;
    }
};
struct Bus {
    H8CPU *cpu=nullptr;
    bool atomic=true;
    std::vector<uint8_t> memory=std::vector<uint8_t>(0x100000,0);
    std::vector<Access> accesses;
    void observe(char kind,uint32_t address) {
        H8Status s{};h8_get_status(cpu,&s);
        accesses.push_back({kind,address,h8_current_cycle(cpu),s.cycles});
    }
};
static uint8_t read8(void *p,uint32_t a) {
    auto &b=*static_cast<Bus*>(p);if(!b.atomic)b.observe('r',a);return b.memory.at(a);
}
static void write8(void *p,uint32_t a,uint8_t v) {
    auto &b=*static_cast<Bus*>(p);if(!b.atomic)b.observe('w',a);b.memory.at(a)=v;
}
static uint16_t read16(void *p,uint32_t a) {
    auto &b=*static_cast<Bus*>(p);b.observe('r',a);return (b.memory.at(a)<<8)|b.memory.at(a+1);
}
static void write16(void *p,uint32_t a,uint16_t v) {
    auto &b=*static_cast<Bus*>(p);b.observe('w',a);b.memory.at(a)=v>>8;b.memory.at(a+1)=v;
}
static void serial(void *p,unsigned ch,uint8_t) {static_cast<Bus*>(p)->observe('s',ch);}
static uint16_t adc(void *p,unsigned ch) {static_cast<Bus*>(p)->observe('a',ch);return 0x200;}
static void word(Bus &b,uint32_t a,uint16_t v) {b.memory[a]=v>>8;b.memory[a+1]=v;}
static void program(Bus &b,std::initializer_list<uint16_t> words) {
    word(b,0,0);word(b,2,0x100);uint32_t a=0x100;
    for(auto v:words){word(b,a,v);a+=2;}
}
static H8CPU *create(Bus &b) {
    H8Config cfg{};cfg.opaque=&b;cfg.read8=read8;cfg.write8=write8;
    cfg.read16=b.atomic?read16:nullptr;cfg.write16=b.atomic?write16:nullptr;
    cfg.serial_tx=serial;cfg.adc_read=adc;b.cpu=h8_create(&cfg);assert(b.cpu);return b.cpu;
}
static H8Status status(H8CPU *c) {H8Status s{};h8_get_status(c,&s);return s;}

static void access_timeline() {
    // Each word bus transaction charges two cycles before invoking the handler.
    // Reset is six cycles; MOV.L immediate is six; prefix is its own two-cycle
    // state; the absolute long store is ten; SLEEP fetch is two.
    const std::vector<Access> expected={
        {'r',0,2,0},{'r',2,4,0},{'r',0x100,6,0},
        {'r',0x102,8,6},{'r',0x104,10,6},{'r',0x106,12,6},
        {'r',0x108,14,12},
        {'r',0x10a,16,14},{'r',0x10c,18,14},{'r',0x10e,20,14},
        {'w',0x80040,22,14},{'w',0x80042,24,14},{'r',0x110,26,24}
    };
    Bus whole,split,bytes;bytes.atomic=false;
    for(Bus *b:{&whole,&split,&bytes}) {
        program(*b,{0x7a00,0x1122,0x3344,0x0100,0x6ba0,0x0008,0x0040,0x0180});
        create(*b);assert(h8_current_cycle(b->cpu)==0);
    }
    h8_run_cycles(whole.cpu,25);h8_run_cycles(bytes.cpu,25);
    // Requesting one cycle executes reset in full, leaving five cycles debt.
    h8_run_cycles(split.cpu,1);assert(h8_current_cycle(split.cpu)==6);
    size_t accesses=split.accesses.size();h8_run_cycles(split.cpu,5);
    assert(split.accesses.size()==accesses&&h8_current_cycle(split.cpu)==6);
    // Stop exactly after the prefix state, then retire its extended instruction.
    h8_run_cycles(split.cpu,7);assert(h8_current_cycle(split.cpu)==14);
    accesses=split.accesses.size();h8_run_cycles(split.cpu,1);
    assert(split.accesses.size()==accesses&&h8_current_cycle(split.cpu)==14);
    h8_run_cycles(split.cpu,1);assert(h8_current_cycle(split.cpu)==24);
    accesses=split.accesses.size();h8_run_cycles(split.cpu,9);
    assert(split.accesses.size()==accesses&&h8_current_cycle(split.cpu)==24);
    h8_run_cycles(split.cpu,1);
    assert(whole.accesses==expected&&split.accesses==expected);
    std::vector<Access> byte_expected;
    for(auto a:expected){byte_expected.push_back(a);a.address++;byte_expected.push_back(a);}
    assert(bytes.accesses==byte_expected); // Both fallback bytes share one word charge.
    for(Bus *b:{&whole,&split,&bytes}) {
        auto s=status(b->cpu);assert(s.cycles==26&&s.sleeping&&!s.halted&&s.instructions==3);
        assert(h8_current_cycle(b->cpu)==s.cycles);
        assert(b->memory[0x80040]==0x11&&b->memory[0x80043]==0x44);
    }
    // Debug accesses do not reuse the residual negative instruction counter.
    bytes.accesses.clear();h8_read_byte(bytes.cpu,0x200);h8_write_byte(bytes.cpu,0x200,1);
    assert((bytes.accesses==std::vector<Access>{{'r',0x200,26,26},{'w',0x200,26,26}}));
    h8_reset(bytes.cpu);assert(h8_current_cycle(bytes.cpu)==0&&status(bytes.cpu).cycles==0);
    bytes.accesses.clear();h8_read_byte(bytes.cpu,0x200);
    assert((bytes.accesses==std::vector<Access>{{'r',0x200,0,0}}));
    bytes.accesses.clear();h8_run_cycles(bytes.cpu,1);
    assert(h8_current_cycle(bytes.cpu)==6&&bytes.accesses.front().current==2);
    for(Bus *b:{&whole,&split,&bytes})h8_destroy(b->cpu);
}

static void peripheral_timeline() {
    Bus b;
    // Start scanning ADC from an actual MOV.B instruction, then sleep. ADC's
    // first sampling callback is inside execution; later samples occur in tick.
    program(b,{0xf831,0x6a88,0xffe8,0x0180});create(b);
    h8_write_byte(b.cpu,0xffffb1,0);h8_write_byte(b.cpu,0xffffb2,0x30);
    h8_write_byte(b.cpu,0xffffb3,0x42);h8_read_byte(b.cpu,0xffffb4);
    h8_write_byte(b.cpu,0xffffb4,4); // SCI deadline is 320 committed cycles.
    h8_run_cycles(b.cpu,400);
    std::vector<Access> events;
    for(auto a:b.accesses)if(a.kind=='s'||a.kind=='a')events.push_back(a);
    assert((events==std::vector<Access>{{'a',0,14,8},{'s',0,336,336},{'a',1,336,336}}));
    assert(h8_current_cycle(b.cpu)==400&&status(b.cpu).cycles==400);
    // Debug ADC start also samples at committed time, with no stale state cost.
    h8_write_byte(b.cpu,0xffffe8,0);h8_write_byte(b.cpu,0xffffe8,0x20);
    assert((b.accesses.back()==Access{'a',0,400,400}));
    h8_reset(b.cpu);assert(h8_current_cycle(b.cpu)==0);
    h8_write_byte(b.cpu,0xffffe8,0x20);
    assert((b.accesses.back()==Access{'a',0,0,0}));h8_destroy(b.cpu);
}
static void internal_cost_timeline() {
    Bus b;
    // External stack makes RTS bus activity observable. After its three word
    // reads (six cycles), RTS charges two internal cycles before target fetch.
    program(b,{0x7a07,0x0008,0x0080,0x5504,0x0180,0x0000,0x5470});create(b);
    h8_run_cycles(b.cpu,31);
    std::vector<Access> rts;
    for(auto a:b.accesses)if(a.committed==20)rts.push_back(a);
    assert((rts==std::vector<Access>{{'r',0x10e,22,20},{'r',0x8007c,24,20},
                                   {'r',0x8007e,26,20},{'r',0x108,30,20}}));
    auto s=status(b.cpu);assert(s.cycles==32&&s.er[7]==0x80080&&s.sleeping&&!s.halted);
    assert(h8_current_cycle(b.cpu)==32);h8_destroy(b.cpu);
}
int main() {
    assert(h8_current_cycle(nullptr)==0);access_timeline();peripheral_timeline();internal_cost_timeline();
    std::puts("H8 access timestamp, prefix/debt, reset/debug and SCI/ADC tests passed");
}
