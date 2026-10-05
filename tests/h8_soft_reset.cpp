// SPDX-License-Identifier: BSD-3-Clause
// Reset retention follows pinned MAME h8.cpp::device_reset, h83002.cpp::map
// and h8_port.cpp::device_reset; timelines use independent charged bus costs.
#include "../libpcsxcore/arcade/h8.h"
#include <cassert>
#include <cstdio>
#include <vector>

struct Access {uint32_t address;uint64_t absolute,diagnostic;};
struct Pins {unsigned port;uint8_t data,ddr;};
struct Bus {
    H8CPU *cpu=nullptr;
    std::vector<uint8_t> memory=std::vector<uint8_t>(0x2000,0);
    std::vector<Access> reads,adc,serial;
    std::vector<Pins> pins;
    bool try_reset=false;
    unsigned reset_rejections=0;
    Access timestamp(uint32_t a) {
        H8Status s{};h8_get_status(cpu,&s);return {a,h8_current_cycle(cpu),s.cycles};
    }
    void reject_running_reset() {
        if(try_reset) {assert(h8_soft_reset(cpu,0)==-2);reset_rejections++;}
    }
};
static uint8_t read8(void *p,uint32_t a) {
    auto &b=*static_cast<Bus*>(p);b.reads.push_back(b.timestamp(a));b.reject_running_reset();return b.memory.at(a);
}
static void write8(void *p,uint32_t a,uint8_t v) {static_cast<Bus*>(p)->memory.at(a)=v;}
static uint16_t read16(void *p,uint32_t a) {
    auto &b=*static_cast<Bus*>(p);b.reads.push_back(b.timestamp(a));b.reject_running_reset();
    return (b.memory.at(a)<<8)|b.memory.at(a+1);
}
static uint8_t port_read(void*,unsigned) {return 0;}
static void port_write(void *p,unsigned port,uint8_t data,uint8_t ddr) {
    static_cast<Bus*>(p)->pins.push_back({port,data,ddr});
}
static uint16_t adc_read(void *p,unsigned ch) {
    auto &b=*static_cast<Bus*>(p);b.adc.push_back(b.timestamp(ch));b.reject_running_reset();return 0x155;
}
static void serial_tx(void *p,unsigned ch,uint8_t) {
    auto &b=*static_cast<Bus*>(p);b.serial.push_back(b.timestamp(ch));b.reject_running_reset();
}
static void word(Bus &b,uint32_t a,uint16_t v) {b.memory.at(a)=v>>8;b.memory.at(a+1)=v;}
static void code(Bus &b,std::initializer_list<uint16_t> words) {
    word(b,0,0);word(b,2,0x100);unsigned a=0x100;
    for(auto v:words){word(b,a,v);a+=2;}
}
static H8Status status(H8CPU *c) {H8Status s{};h8_get_status(c,&s);return s;}
static void create(Bus &b) {
    H8Config cfg{};cfg.opaque=&b;cfg.read8=read8;cfg.write8=write8;cfg.read16=read16;
    cfg.port_read=port_read;cfg.port_write=port_write;cfg.adc_read=adc_read;cfg.serial_tx=serial_tx;
    b.cpu=h8_create(&cfg);assert(b.cpu);
}
static void start_serial(Bus &b) {
    h8_write_byte(b.cpu,0xffffb1,0);h8_write_byte(b.cpu,0xffffb2,0x30);
    h8_write_byte(b.cpu,0xffffb3,0x42);h8_read_byte(b.cpu,0xffffb4);h8_write_byte(b.cpu,0xffffb4,4);
}
static void check_reset_reads(const Bus &b,uint64_t epoch) {
    assert(b.reads.size()>=3);
    for(unsigned i=0;i<3;i++) {
        assert(b.reads[i].address==(i==2?0x100:i*2));
        assert(b.reads[i].absolute==epoch+2*(i+1));
        assert(b.reads[i].diagnostic==0);
    }
}

static void retention_and_deadlines() {
    Bus b;word(b,0,0);word(b,2,0x100);unsigned pc=0x100;
    for(unsigned r=0;r<8;r++) {
        uint32_t v=0x11223344+r*0x01020304;
        word(b,pc,0x7a00|r);word(b,pc+2,v>>16);word(b,pc+4,v);pc+=6;
    }
    word(b,pc,0x0735);word(b,pc+2,0x0180);create(b);h8_run_cycles(b.cpu,200);
    auto before=status(b.cpu);assert(before.sleeping&&!before.halted&&before.ccr==0x35);
    for(unsigned i=0;i<512;i++)h8_write_byte(b.cpu,0xfffd10+i,(i*37+11)&255);
    const unsigned ddr[]={0xc5,0xc9,0xcd,0xd0,0xd1,0xd4};
    const unsigned dr[]={0xc7,0xcb,0xcf,0xd2,0xd3,0xd6};
    uint8_t old_data[6];Pins old_pins[6];
    for(unsigned i=0;i<6;i++) {
        h8_write_byte(b.cpu,0xffff00+ddr[i],0xff);
        h8_write_byte(b.cpu,0xffff00+dr[i],0x35+i*13);
        old_data[i]=h8_read_byte(b.cpu,0xffff00+dr[i]);
        old_pins[i]=b.pins.back();
    }
    h8_write_byte(b.cpu,0xffffda,0x5e);
    // Neighboring holes are not preserved as if they were GPIO registers.
    const unsigned holes[]={0xc4,0xc6,0xc8,0xca,0xd5,0xd7,0xd8,0xd9,0xdb,0xdc,0xdd,0xdf};
    for(auto r:holes)h8_write_byte(b.cpu,0xffff00+r,0x77);
    start_serial(b);h8_write_byte(b.cpu,0xffffe8,0x31);
    assert(b.adc.size()==1&&b.serial.empty());
    h8_write_byte(b.cpu,0xfffff2,0);h8_write_byte(b.cpu,0xfffff5,2);h8_set_irq(b.cpu,1,1);
    h8_set_irq(b.cpu,-1,1);
    // The caller supplies an absolute budget epoch; values above 32 bits must
    // reach both bus and peripheral callbacks without truncation.
    const uint64_t epoch=(UINT64_C(1)<<40)+123;
    const size_t pin_events=b.pins.size();
    assert(h8_soft_reset(b.cpu,epoch)==0);
    auto s=status(b.cpu);assert(s.cycles==0&&s.instructions==0&&s.ccr==before.ccr);
    assert(!s.sleeping&&!s.halted&&s.illegal_pc==0&&s.unsupported_peripherals==0);
    assert(h8_current_cycle(b.cpu)==epoch&&b.pins.size()==pin_events);
    for(unsigned r=0;r<8;r++)assert(s.er[r]==before.er[r]);
    for(unsigned i=0;i<512;i++)assert(h8_read_byte(b.cpu,0xfffd10+i)==uint8_t(i*37+11));
    for(unsigned i=0;i<6;i++) {
        assert(h8_read_byte(b.cpu,0xffff00+dr[i])==old_data[i]);
        h8_write_byte(b.cpu,0xffff00+dr[i],0x35+i*13);
        auto p=b.pins.back();assert(p.port==old_pins[i].port&&p.data==old_pins[i].data&&p.ddr==old_pins[i].ddr);
    }
    assert(h8_read_byte(b.cpu,0xffffda)==0x5e);
    for(auto r:holes)assert(h8_read_byte(b.cpu,0xffff00+r)==0);
    assert(h8_read_byte(b.cpu,0xfffff2)==9&&h8_read_byte(b.cpu,0xfffff5)==0&&h8_read_byte(b.cpu,0xfffff6)==0);
    assert(h8_read_byte(b.cpu,0xffffb2)==0&&h8_read_byte(b.cpu,0xffffb4)==0x84);
    assert(h8_read_byte(b.cpu,0xffffe8)==0);
    code(b,{0x0000,0x0180});b.reads.clear();b.try_reset=true;
    h8_run_cycles(b.cpu,1);check_reset_reads(b,epoch);
    s=status(b.cpu);assert(s.cycles==6&&s.ccr==0xb5&&s.instructions==0);
    assert(h8_current_cycle(b.cpu)==epoch+6);
    for(unsigned r=0;r<8;r++)assert(s.er[r]==before.er[r]);
    h8_run_cycles(b.cpu,999);
    assert(status(b.cpu).sleeping&&status(b.cpu).cycles==1000);
    assert(b.serial.empty()&&b.adc.size()==1); // Old deadlines and scan are gone.
    start_serial(b); // New deadlines use the absolute domain, not cycles=1000.
    b.try_reset=false;h8_write_byte(b.cpu,0xffffe8,0x31);b.try_reset=true;
    auto first=b.adc.back();assert(first.absolute==epoch+1000&&first.diagnostic==1000);
    h8_run_cycles(b.cpu,400);
    assert(b.serial.size()==1&&b.adc.size()==3);
    assert(b.serial[0].absolute==epoch+1320&&b.serial[0].diagnostic==1320);
    assert(b.adc.back().absolute==epoch+1320&&b.adc.back().diagnostic==1320);
    assert(b.reset_rejections>=5); // Bus callbacks and tick SCI/ADC reject reset.
    b.try_reset=false;
    // Check a non-default DDR survives another reset, including fixed pin masks.
    for(unsigned i=0;i<6;i++) {
        h8_write_byte(b.cpu,0xffff00+ddr[i],0xa5+i);
        old_pins[i]=b.pins.back();
    }
    assert(h8_soft_reset(b.cpu,epoch+1400)==0);
    for(unsigned i=0;i<6;i++) {
        h8_write_byte(b.cpu,0xffff00+dr[i],0x35+i*13);auto p=b.pins.back();
        assert(p.port==old_pins[i].port&&p.data==old_pins[i].data&&p.ddr==old_pins[i].ddr);
    }
    // The original cold API still clears storage, registers, diagnostics/epoch.
    h8_reset(b.cpu);s=status(b.cpu);assert(h8_current_cycle(b.cpu)==0&&s.cycles==0&&s.ccr==0);
    for(unsigned r=0;r<8;r++)assert(s.er[r]==0);
    for(unsigned i=0;i<512;i++)assert(h8_read_byte(b.cpu,0xfffd10+i)==0);
    assert(h8_read_byte(b.cpu,0xffffda)==0);h8_destroy(b.cpu);
}

static void budget_and_repeated_resets() {
    Bus b;code(b,{0x0000,0x0180});create(b);
    assert(h8_soft_reset(nullptr,0)==-1);
    h8_run_cycles(b.cpu,1);assert(h8_current_cycle(b.cpu)==6); // Five-cycle debt.
    assert(h8_soft_reset(b.cpu,1)==0); // Reset discards it, as MAME does.
    assert(status(b.cpu).cycles==0&&h8_current_cycle(b.cpu)==1);
    b.reads.clear();h8_run_cycles(b.cpu,1);check_reset_reads(b,1);
    assert(status(b.cpu).cycles==6&&h8_current_cycle(b.cpu)==7);
    size_t n=b.reads.size();h8_run_cycles(b.cpu,5);
    assert(b.reads.size()==n&&h8_current_cycle(b.cpu)==7); // New debt is retained.
    for(uint64_t epoch:{UINT64_C(0),UINT64_C(2),UINT64_C(100)}) {
        assert(h8_soft_reset(b.cpu,epoch)==0);
        assert(h8_current_cycle(b.cpu)==epoch&&status(b.cpu).cycles==0);
        b.reads.clear();h8_run_cycles(b.cpu,1);check_reset_reads(b,epoch);
        assert(h8_current_cycle(b.cpu)==epoch+6&&status(b.cpu).cycles==6);
    }
    // A pending extended decode state must not survive the reset boundary.
    h8_reset(b.cpu);code(b,{0x0100,0x6b20,0x0000,0x0200});
    h8_run_cycles(b.cpu,7);assert(h8_current_cycle(b.cpu)==8);
    code(b,{0x0180});assert(h8_soft_reset(b.cpu,7)==0);
    b.reads.clear();h8_run_cycles(b.cpu,8);check_reset_reads(b,7);
    assert(status(b.cpu).sleeping&&!status(b.cpu).halted&&status(b.cpu).cycles==8);
    for(auto access:b.reads)assert(access.address!=0x200);
    // Reset also restarts an illegal-opcode halt and clears its diagnostics.
    h8_reset(b.cpu);code(b,{0x0101});h8_run_cycles(b.cpu,8);
    h8_read_byte(b.cpu,0xffff20);h8_read_byte(b.cpu,0xffffa8);
    auto failed=status(b.cpu);
    assert(failed.halted&&failed.illegal_pc==0x100&&failed.illegal_opcode==0x0101);
    assert(failed.unsupported_peripherals==3);
    code(b,{0x0180});assert(h8_soft_reset(b.cpu,8)==0);
    auto restored=status(b.cpu);
    assert(!restored.halted&&restored.illegal_pc==0&&restored.illegal_opcode==0&&restored.unsupported_peripherals==0);
    h8_run_cycles(b.cpu,8);assert(status(b.cpu).sleeping&&status(b.cpu).cycles==8);
    h8_reset(b.cpu);assert(h8_current_cycle(b.cpu)==0);
    b.reads.clear();h8_run_cycles(b.cpu,1);check_reset_reads(b,0);h8_destroy(b.cpu);
}
int main() {
    retention_and_deadlines();budget_and_repeated_resets();
    std::puts("H8 soft reset: source retention, epochs/debt, callbacks/deadlines and cold reset PASS");
}
