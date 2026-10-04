#include "mn10200.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

struct Bus {
    std::vector<uint8_t> mem=std::vector<uint8_t>(1u<<24);
    mn10200_cpu *cpu=nullptr;
    uint64_t reads=0,writes=0,first_read_cycle=0;
    uint32_t last_address=0;
    bool reenter=false;
    int reenter_result=0;
    uint8_t port_value=0xa5;
};
static uint8_t rd(void *o,uint32_t a){auto &b=*static_cast<Bus*>(o);++b.reads;b.last_address=a;if(!b.first_read_cycle&&b.cpu)b.first_read_cycle=mn10200_get_callback_cycle(b.cpu);if(b.reenter&&b.cpu){b.reenter=false;b.reenter_result=mn10200_reset(b.cpu);}return a<b.mem.size()?b.mem[a]:0xff;}
static void wr(void *o,uint32_t a,uint8_t v){auto &b=*static_cast<Bus*>(o);++b.writes;b.last_address=a;if(a<b.mem.size())b.mem[a]=v;}
static uint8_t pr(void *o,unsigned,uint8_t){return static_cast<Bus*>(o)->port_value;}
static void pw(void *,unsigned,uint8_t,uint8_t){}
static void put(Bus&b,uint32_t a,std::initializer_list<uint8_t> data){for(uint8_t v:data)b.mem[a++]=v;}
static mn10200_cpu *make(Bus&b){mn10200_config c{};c.opaque=&b;c.read8=rd;c.write8=wr;c.port_read=pr;c.port_write=pw;auto *cpu=mn10200_create(&c);b.cpu=cpu;return cpu;}

static void test_execution_and_external_bus(){
    Bus b; put(b,0x80000,{0xf8,0x34,0x12, 0xc0,0x00,0x20, 0xf6,0xf6});
    auto *c=make(b);assert(c);auto used=mn10200_run(c,2);assert(used==2);assert(mn10200_get_d(c,0)==0x1234);assert(b.mem[0x2000]==0x34&&b.mem[0x2001]==0x12);assert(b.writes>=2);assert(mn10200_get_pc(c)==0x80006);assert(b.first_read_cycle==1);mn10200_destroy(c);
}
static void test_arithmetic_and_slice_equivalence(){
    Bus a,b; std::initializer_list<uint8_t> program={0xf8,0xff,0x7f, 0xf9,0x01,0x00, 0x94, 0xf6, 0xf6};put(a,0x80000,program);put(b,0x80000,program);auto *ca=make(a),*cb=make(b);assert(ca&&cb);assert(mn10200_run(ca,4)>=4);for(int i=0;i<4;i++)assert(mn10200_run(cb,1)>=1);assert(mn10200_get_d(ca,0)==mn10200_get_d(cb,0));assert(mn10200_get_psw(ca)==mn10200_get_psw(cb));assert(mn10200_get_pc(ca)==mn10200_get_pc(cb));assert(mn10200_get_d(ca,0)==0x8000);assert(mn10200_get_psw(ca)&0x8);mn10200_destroy(ca);mn10200_destroy(cb);
}
static void test_timer_and_irq_inputs(){
    Bus b;put(b,0x80000,{0xf6,0xf6,0xf6,0xf6,0xf6,0xf6});auto*c=make(b);assert(c);
    assert(mn10200_write_internal(c,0x21a,1)==0);assert(mn10200_write_internal(c,0x22a,0xc0)==0);
    assert(mn10200_write_internal(c,0x210,1)==0);assert(mn10200_write_internal(c,0x220,0xc2)==0);
    assert(mn10200_write_internal(c,0x211,0)==0);assert(mn10200_write_internal(c,0x221,0xc1)==0);
    assert(mn10200_run(c,4)>=4);assert((mn10200_read_internal(c,0x42)&0x20)!=0);
    assert(mn10200_write_internal(c,0x56,0)==0);assert(mn10200_set_irq(c,0,1)==0);assert((mn10200_read_internal(c,0x50)&0x10)!=0);assert(mn10200_set_irq(c,0,0)==0);assert(mn10200_write_internal(c,0x50,0xef)==0);assert((mn10200_read_internal(c,0x50)&0x10)==0);
    mn10200_destroy(c);
}

static void test_illegal_opcode_nmi(){
    Bus b;put(b,0x80000,{0xf2,0xa0});put(b,0x80008,{0xf6,0xf6});auto*c=make(b);assert(c);assert(mn10200_run(c,4)>=4);assert(mn10200_get_pc(c)>=0x80009);assert(mn10200_get_a(c,3)==0xfffffa);assert((mn10200_read_internal(c,0x40)&2)!=0);mn10200_destroy(c);
}

static void test_internal_ports_reentry_reset_and_overflow(){
    Bus b;put(b,0x80000,{0xf6,0xf6,0xf6,0xf6});auto*c=make(b);assert(c);
    assert(mn10200_read_internal(c,0x3d0)==0xa5);assert(mn10200_get_fault(c)==0);
    b.reenter=true;assert(mn10200_run(c,1)>=1);assert(b.reenter_result==-1);assert(mn10200_get_fault(c)==4);
    uint32_t pc=mn10200_get_pc(c);uint64_t cycles=mn10200_get_cycles(c);mn10200_clear_fault(c);assert(mn10200_run(c,UINT64_MAX)==-1);assert(mn10200_get_fault(c)==5);assert(mn10200_get_pc(c)==pc&&mn10200_get_cycles(c)==cycles);
    mn10200_clear_fault(c);assert(mn10200_set_irq(c,0,0)==0);uint64_t before_reset=mn10200_get_cycles(c);assert(mn10200_reset(c)==0);assert(mn10200_get_pc(c)==0x80000);assert(mn10200_get_cycles(c)==before_reset);assert(mn10200_read_internal(c,0x57)&0x10);mn10200_destroy(c);
}

static void test_irq_vector_and_stack(){
    Bus b;put(b,0x80000,{0xf8,0x00,0x0f,0xf3,0xd0,0xf6});put(b,0x80008,{0xf6,0xf6});
    auto*c=make(b);assert(c);assert(mn10200_write_internal(c,0x51,1)==0);assert(mn10200_set_irq(c,0,1)==0);assert((mn10200_read_internal(c,0x50)&0x10)!=0);
    assert(mn10200_run(c,5)>=5);assert(mn10200_get_pc(c)>=0x80009&&mn10200_get_pc(c)<0x80020);assert(mn10200_get_a(c,3)==0xfffffa);assert(mn10200_read_internal(c,0x00e)==0x10);assert(b.mem[0xfffffa]==0x00&&b.mem[0xfffffb]==0x0f);assert(b.mem[0xfffffc]==0x05&&b.mem[0xfffffd]==0x00&&b.mem[0xfffffe]==0x08);mn10200_destroy(c);
}

int main(){test_execution_and_external_bus();test_arithmetic_and_slice_equivalence();test_timer_and_irq_inputs();test_irq_vector_and_stack();test_illegal_opcode_nmi();test_internal_ports_reentry_reset_and_overflow();std::puts("MN10200 standalone tests passed");return 0;}
