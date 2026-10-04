/* Panasonic MN10200 / MN1020012A standalone core.
 * The opcode, interrupt and peripheral register algorithms are derived from
 * MAME src/devices/cpu/mn10200/mn10200.cpp; see mn10200_LICENSE.txt.
 */
#include "mn10200.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <limits>
#include <new>

static void logerror(const char *, ...) {}
#define log_event(...) ((void)0)
using offs_t = uint32_t;

enum mn10200_flag {
 FLAG_ZF=0x0001, FLAG_NF=0x0002, FLAG_CF=0x0004, FLAG_VF=0x0008,
 FLAG_ZX=0x0010, FLAG_NX=0x0020, FLAG_CX=0x0040, FLAG_VX=0x0080,
 FLAG_IM0=0x0100, FLAG_IM1=0x0200, FLAG_IM2=0x0400, FLAG_IE=0x0800,
 FLAG_S0=0x1000, FLAG_S1=0x2000, FLAG_D14=0x4000, FLAG_D15=0x8000
};
enum { MN10200_PORT0=0, MN10200_PORT1, MN10200_PORT2, MN10200_PORT3, MN10200_PORT4 };
static constexpr unsigned MN10200_MAX_EXT_IRQ=4;

struct mn10200_cpu {
    class MN10200Core *core;
};

class MN10200Core {
public:
    explicit MN10200Core(const mn10200_config &cfg) : m_config(cfg) { reset(); }
    void reset();
    int64_t run(uint64_t cycles);
    int set_irq(unsigned irq, int state);
    void set_irq_line(int irqnum, int state);
    uint8_t io_control_r(offs_t offset);
    void io_control_w(offs_t offset, uint8_t data);
    void execute_run();
    void illegal(uint8_t prefix, uint8_t op);
    uint32_t do_add(uint32_t a,uint32_t b,uint32_t c=0);
    uint32_t do_sub(uint32_t a,uint32_t b,uint32_t c=0);
    void test_nz16(uint16_t v); void do_jsr(uint32_t to,uint32_t ret); void do_branch(int condition=1);
    void take_irq(int level,int group); void check_irq(); void check_ext_irq();
    void refresh_timer(int tmr); void refresh_all_timers(); int timer_tick_simple(int tmr);
    void process_timers();
    void change_pc(uint32_t pc) { m_pc=pc&0xffffff; }
    uint8_t read_bus8(uint32_t address);
    void write_bus8(uint32_t address,uint8_t data);
    uint8_t read_arg8(uint32_t a){return read_bus8(a);} 
    uint16_t read_arg16(uint32_t a){return (uint16_t)(read_bus8(a)|(uint16_t(read_bus8(a+1))<<8));}
    uint32_t read_arg24(uint32_t a){return read_bus8(a)|(uint32_t(read_bus8(a+1))<<8)|(uint32_t(read_bus8(a+2))<<16);}
    uint8_t read_mem8(uint32_t a){return read_bus8(a);}
    uint16_t read_mem16(uint32_t a){a &= ~1U; return (uint16_t)(read_bus8(a)|(uint16_t(read_bus8(a+1))<<8));}
    uint32_t read_mem24(uint32_t a){a &= ~1U; return read_mem16(a)|(uint32_t(read_bus8(a+2))<<16);}
    void write_mem8(uint32_t a,uint8_t d){write_bus8(a,d);}
    void write_mem16(uint32_t a,uint16_t d){a &= ~1U;write_bus8(a,d);write_bus8(a+1,d>>8);}
    void write_mem24(uint32_t a,uint32_t d){a &= ~1U;write_mem16(a,d);write_bus8(a+2,d>>16);}
    uint8_t read_port(unsigned p,uint8_t mask){if(!m_config.port_read)return 0xff; return invoke_port_read(p,mask);}
    void write_port(unsigned p,uint8_t v,uint8_t mask){if(m_config.port_write)invoke_port_write(p,v,mask);}
    uint64_t callback_cycle() const { if(m_timer_processing)return m_timer_event_cycle; return m_in_run ? m_run_start + uint64_t(int64_t(m_run_budget) - m_cycles) : m_total_cycles; }
    uint32_t m_pc=0,m_d[4]{},m_a[4]{}; uint16_t m_psw=0,m_mdr=0;
public:
    struct timer_t { uint8_t mode=0,base=0,cur=0; uint64_t deadline=UINT64_MAX; } m_simple_timer[10];
    struct prescaler_t { uint8_t mode=0,base=0,cur=0; } m_prescaler[2];
    struct dma_t {uint32_t adr=0,count=0;uint16_t iadr=0;uint8_t ctrll=0,ctrlh=0,irq=0;} m_dma[8];
    struct serial_t {uint8_t ctrll=0,ctrlh=0,buf=0,recv=0;} m_serial[2];
    struct port_t {uint8_t out=0,dir=0;} m_port[4];
    mn10200_config m_config{};
    uint8_t m_icrl[31]{},m_icrh[31]{},m_nmicr=0,m_iagr=0,m_extmdl=0,m_extmdh=0,m_p4=0xf;
    bool m_possible_irq=false,m_in_run=false,m_in_callback=false,m_faulted=false,m_timer_processing=false;
    uint64_t m_timer_event_cycle=0;
    uint8_t m_pplul=0,m_ppluh=0,m_p3md=0;
    int m_cycles=0;
    uint64_t m_total_cycles=0,m_run_start=0,m_run_budget=0;
    uint32_t m_fault=0;
        uint8_t invoke_port_read(unsigned p,uint8_t mask);
    void invoke_port_write(unsigned p,uint8_t v,uint8_t mask);
    bool enter_callback();
    void leave_callback();
    uint8_t callback_read(uint32_t address);
    void callback_write(uint32_t address,uint8_t value);
    void set_fault(uint32_t code){m_fault=code;m_faulted=true;}
};

#include "mn10200_engine.inc"

bool MN10200Core::enter_callback(){if(m_in_callback){set_fault(2);return false;}m_in_callback=true;return true;}
void MN10200Core::leave_callback(){m_in_callback=false;}
uint8_t MN10200Core::callback_read(uint32_t a){if(!m_config.read8)return 0xff;if(!enter_callback())return 0xff;uint8_t v=m_config.read8(m_config.opaque,a&0xffffff);leave_callback();return v;}
void MN10200Core::callback_write(uint32_t a,uint8_t v){if(!m_config.write8)return;if(!enter_callback())return;m_config.write8(m_config.opaque,a&0xffffff,v);leave_callback();}
uint8_t MN10200Core::invoke_port_read(unsigned p,uint8_t mask){if(!enter_callback())return 0xff;uint8_t v=m_config.port_read(m_config.opaque,p,mask);leave_callback();return v;}
void MN10200Core::invoke_port_write(unsigned p,uint8_t v,uint8_t mask){if(!enter_callback())return;m_config.port_write(m_config.opaque,p,v,mask);leave_callback();}
uint8_t MN10200Core::read_bus8(uint32_t a){a&=0xffffff;if(a>=0xfc00&&a<=0xffff)return io_control_r(a-0xfc00);return callback_read(a);}
void MN10200Core::write_bus8(uint32_t a,uint8_t d){a&=0xffffff;if(a>=0xfc00&&a<=0xffff)io_control_w(a-0xfc00,d);else callback_write(a,d);}
void MN10200Core::refresh_timer(int t){int p=m_simple_timer[t].mode&1;if((m_simple_timer[t].mode&0x82)==0x82&&(m_prescaler[p].mode&0x80)){uint64_t period=uint64_t(m_prescaler[p].base+1)*(m_simple_timer[t].cur+1);uint64_t now=callback_cycle();if(period>UINT64_MAX-now){set_fault(3);m_simple_timer[t].deadline=UINT64_MAX;}else m_simple_timer[t].deadline=now+period;}else m_simple_timer[t].deadline=UINT64_MAX;}
void MN10200Core::refresh_all_timers(){for(int i=0;i<10;i++)refresh_timer(i);}
int MN10200Core::timer_tick_simple(int t){int next=t+1;if(next<10&&(m_simple_timer[next].mode&0x83)==0x81){if(--m_simple_timer[next].cur==0xff){if(timer_tick_simple(next)!=2){m_simple_timer[next].cur=m_simple_timer[next].base;return 1;}}return 2;}m_icrl[1+(t>>2)]|=1<<(4+(t&3));m_possible_irq=true;return 0;}
void MN10200Core::process_timers(){for(;;){int pick=-1;uint64_t due=UINT64_MAX;for(int i=0;i<10;i++)if(m_simple_timer[i].deadline<=m_total_cycles&&m_simple_timer[i].deadline<due){pick=i;due=m_simple_timer[i].deadline;}if(pick<0)break;auto &t=m_simple_timer[pick];m_timer_processing=true;m_timer_event_cycle=due;if(timer_tick_simple(pick)==2)t.cur=0xff;else t.cur=t.base;refresh_timer(pick);m_timer_processing=false;} }
void MN10200Core::reset(){if(m_in_callback){set_fault(4);return;}m_fault=0;m_faulted=false;m_pc=0x80000;m_psw=0;memset(m_d,0,sizeof(m_d));memset(m_a,0,sizeof(m_a));m_mdr=0;m_nmicr=0;m_iagr=0;m_possible_irq=false;m_in_run=false;m_cycles=0;memset(m_icrl,0,sizeof(m_icrl));memset(m_icrh,0,sizeof(m_icrh));m_extmdl=m_extmdh=0;m_pplul=m_ppluh=m_p3md=0;for(auto &x:m_simple_timer)x={};for(auto &x:m_prescaler)x={};for(auto &x:m_dma)x={};for(auto &x:m_serial)x={};for(auto &x:m_port)x={};write_mem16(0xfc00,0x8000);write_mem16(0xfc02,0x0737);for(int j=0;j<2;j++)for(unsigned a=0xfc04;a<0x10000;a++)write_mem16(a,0);m_possible_irq=false;}

int64_t MN10200Core::run(uint64_t cycles){
    if(m_in_callback||m_in_run){set_fault(4);return -1;}
    if(!cycles)return 0;
    if(cycles>uint64_t(INT64_MAX-32)||m_total_cycles>UINT64_MAX-32||cycles>UINT64_MAX-m_total_cycles-32){set_fault(5);return -1;}
    uint64_t initial=m_total_cycles, remaining=cycles;
    process_timers();
    while(remaining){
        uint64_t budget=std::min<uint64_t>(remaining, uint64_t(INT_MAX-32));
        uint64_t next=UINT64_MAX;
        for(auto &t:m_simple_timer) if(t.deadline>m_total_cycles&&t.deadline<next)next=t.deadline;
        if(next!=UINT64_MAX && next-m_total_cycles<budget)budget=next-m_total_cycles;
        if(!budget){process_timers();continue;}
        m_run_start=m_total_cycles;m_run_budget=budget;m_cycles=(int)budget;m_in_run=true;
        execute_run();m_in_run=false;
        int64_t consumed=int64_t(budget)-m_cycles;
        if(consumed<=0){set_fault(6);return -1;}
        if(uint64_t(consumed)>UINT64_MAX-m_total_cycles){set_fault(5);return -1;}
        m_total_cycles+=uint64_t(consumed);
        remaining=uint64_t(consumed)>=remaining?0:remaining-uint64_t(consumed);
        process_timers();
    }
    uint64_t delta=m_total_cycles-initial;
    return delta>uint64_t(INT64_MAX)?-1:int64_t(delta);
}
int MN10200Core::set_irq(unsigned irq,int state){if(irq>=4){set_fault(7);return -1;}set_irq_line(int(irq),state);return 0;}

extern "C" {
mn10200_cpu *mn10200_create(const mn10200_config *cfg){if(!cfg)return nullptr;mn10200_cpu *cpu=new(std::nothrow) mn10200_cpu{};if(!cpu)return nullptr;cpu->core=new(std::nothrow) MN10200Core(*cfg);if(!cpu->core){delete cpu;return nullptr;}return cpu;}
void mn10200_destroy(mn10200_cpu *cpu){if(!cpu)return;if(cpu->core && cpu->core->m_in_callback){cpu->core->set_fault(4);return;}delete cpu->core;delete cpu;}
int mn10200_reset(mn10200_cpu *cpu){if(!cpu||!cpu->core)return -1;if(cpu->core->m_in_callback||cpu->core->m_in_run){cpu->core->set_fault(4);return -1;}cpu->core->reset();return 0;}
int64_t mn10200_run(mn10200_cpu *cpu,uint64_t cycles){return cpu&&cpu->core?cpu->core->run(cycles):-1;}
int mn10200_set_irq(mn10200_cpu *cpu,unsigned n,int asserted){return cpu&&cpu->core?cpu->core->set_irq(n,asserted):-1;}
uint32_t mn10200_get_pc(const mn10200_cpu *c){return c&&c->core?c->core->m_pc:0;}
uint16_t mn10200_get_psw(const mn10200_cpu *c){return c&&c->core?c->core->m_psw:0;}
uint32_t mn10200_get_d(const mn10200_cpu *c,unsigned i){return c&&c->core&&i<4?(c->core->m_d[i]&0xffffff):0;}
uint32_t mn10200_get_a(const mn10200_cpu *c,unsigned i){return c&&c->core&&i<4?(c->core->m_a[i]&0xffffff):0;}
uint64_t mn10200_get_cycles(const mn10200_cpu *c){return c&&c->core?c->core->m_total_cycles:0;}
uint64_t mn10200_get_callback_cycle(const mn10200_cpu *c){return c&&c->core?c->core->callback_cycle():0;}
uint32_t mn10200_get_fault(const mn10200_cpu *c){return c&&c->core?c->core->m_fault:1;}
void mn10200_clear_fault(mn10200_cpu *c){if(c&&c->core&&!c->core->m_in_callback)c->core->m_fault=0;}
uint8_t mn10200_read_internal(mn10200_cpu *c,uint16_t o){return c&&c->core?c->core->io_control_r(o):0xff;}
int mn10200_write_internal(mn10200_cpu *c,uint16_t o,uint8_t v){if(!c||!c->core)return -1;if(c->core->m_in_callback||c->core->m_in_run){c->core->set_fault(4);return -1;}c->core->io_control_w(o,v);return c->core->m_fault? -1:0;}
}

