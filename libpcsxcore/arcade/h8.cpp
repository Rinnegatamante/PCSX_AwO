// SPDX-License-Identifier: BSD-3-Clause
// copyright-holders:Olivier Galibert
// Standalone H8/300H ISA adapter and H8/3002 internal peripheral implementation.
#include "h8.h"
#include "h8_adc.h"
#include "h8_cycle_clock.h"
#include "h8_sci.h"
#include "h8_timer16.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
using s8=int8_t; using s16=int16_t; using s32=int32_t; using s64=int64_t;

struct H8CPU {
    enum { F_I=0x80,F_UI=0x40,F_H=0x20,F_U=0x10,F_N=8,F_Z=4,F_V=2,F_C=1,
           EXR_I=7,EXR_NC=0x78, STATE_RESET=0x10000,STATE_IRQ=0x10001 };
    H8Config config;
    u32 m_PC=0,m_NPC=0,m_PPC=0,m_TMP1=0,m_TMP2=0;
    u16 m_PIR=0,m_IR[5]={},m_R[16]={};
    u8 m_CCR=0,m_EXR=0;
    bool m_has_hc=true,m_has_mac=false,m_mode_advanced=true;
    int m_inst_state=STATE_RESET,m_inst_substate=0,m_icount=0,m_bcount=0,m_irq_vector=0,m_taken_irq_vector=0;
    int64_t budget=0;
    bool executing_state=false,in_run=false,deadline_run=false;
    bool m_sleeping=false,halted=false,nmi_level=false;
    u64 cycles=0,cycle_epoch=0,instructions=0,pending=0,cycle_ahead=0;
    u32 illegal_pc=0,unsupported=0;
    u16 illegal_opcode=0;
    u8 ram[0x200]={},io[0x100]={},irq_lines=0;
    u64 run_deadline=0;
    arcade::H8Adc3337 adc;
    arcade::H8Sci sci[2];
    arcade::H8Timer16 timers;
    arcade::H8CycleClock clock;
    void bind_peripherals();
    void refresh_bcount();
    void internal_update_at(u64 when);
    u64 next_internal_event() const;
    explicit H8CPU(const H8Config &c):config(c) {}
    u8 bus_read(u32 a);
    void bus_write(u32 a,u8 v);
    void tick(unsigned elapsed);
    void update_irq_filter();
    void interrupt_taken();
    void irq_setup() { m_CCR |= (io[0xf2]&8) ? F_I : F_I|F_UI; }
    int trapa_setup() { irq_setup(); return 8; }
    bool exr_in_stack() const { return false; }
    void debugger_exception_hook(int) {}
    void prefetch_done() {
        update_irq_filter();
        if(m_irq_vector) {
            m_inst_state=STATE_IRQ; m_taken_irq_vector=m_irq_vector; m_sleeping=false;
        } else m_inst_state=m_IR[0]=m_PIR;
    }
    void prefetch_done_noirq() { m_inst_state=m_IR[0]=m_PIR; }
    void prefetch_done_notrace() { prefetch_done(); }
    void prefetch_switch(u32 pc,u16 ir) {m_NPC=pc&0xffffff;m_PC=pc+2;m_PIR=ir;}
    void illegal() {halted=true;illegal_pc=m_PPC;illegal_opcode=m_IR[0];m_icount-=2;}
    u8 read8(u32 a) {m_icount-=2;return bus_read(a);}
    void write8(u32 a,u8 v) {m_icount-=2;bus_write(a,v);}
    u16 read16(u32 a) {
        m_icount-=2;a&=0xfffffe;
        if(a<0xfffd10&&config.read16)return config.read16(config.opaque,a);
        if(a>=0xffff60&&a<=0xffff9e)return timers.readWord(a&255,h8_current_cycle(this)-cycle_epoch);
        return (u16(bus_read(a))<<8)|bus_read(a+1);
    }
    u16 read16i(u32 a) {return read16(a);}
    void write16(u32 a,u16 v) {
        m_icount-=2;a&=0xfffffe;
        if(a<0xfffd10&&config.write16){config.write16(config.opaque,a,v);return;}
        if(a>=0xffff60&&a<=0xffff9e){timers.writeWord(a&255,v,0xffff,h8_current_cycle(this)-cycle_epoch);return;}
        bus_write(a,v>>8);bus_write(a+1,v);
    }
    void internal(int n) {m_icount-=n+1;}
    void r8_w(int r,u8 v) {
        if(r&8)m_R[r&7]=(m_R[r&7]&0xff00)|v;
        else m_R[r&7]=(m_R[r&7]&0xff)|(u16(v)<<8);
    }
    u8 r8_r(int r) {return (r&8)?m_R[r&7]:m_R[r&7]>>8;}
    void r16_w(int r,u16 v) {m_R[r&15]=v;}
    u16 r16_r(int r) {return m_R[r&15];}
    void r32_w(int r,u32 v) {m_R[r&7]=v;m_R[(r&7)|8]=v>>16;}
    u32 r32_r(int r) const {return m_R[r&7]|(u32(m_R[(r&7)|8])<<16);}
    void do_exec_full();
    void do_exec_partial();
    bool access_to_be_redone() const { return false; }
#include "h8_declarations.inc"
#include "h8_arithmetic_declarations.inc"
};

static uint16_t adc_input(void *opaque,unsigned channel) {
    H8CPU *c=static_cast<H8CPU*>(opaque);
    return c->config.adc_read?c->config.adc_read(c->config.opaque,channel):0;
}
static void adc_irq(void *opaque) {
    H8CPU *c=static_cast<H8CPU*>(opaque);c->pending|=u64(1)<<60;
    c->update_irq_filter();
}
static void peripheral_irq(void *opaque,unsigned vector) {
    H8CPU *c=static_cast<H8CPU*>(opaque);c->pending|=u64(1)<<vector;
    c->update_irq_filter();
}
static void peripheral_reschedule(void *opaque) {
    H8CPU *c=static_cast<H8CPU*>(opaque);
    c->internal_update_at(h8_current_cycle(c));
}
static uint64_t peripheral_now(void *opaque) {
    H8CPU *c=static_cast<H8CPU*>(opaque);
    return h8_current_cycle(c)-c->cycle_epoch;
}
static uint64_t peripheral_rx_now(void *opaque) {
    H8CPU *c=static_cast<H8CPU*>(opaque);
    return c->clock.nowAsCycles(c->executing_state?u64(-int64_t(c->m_icount)):0);
}
template<unsigned Channel> static void serial_tx_pin(void *opaque,int level) {
    H8CPU *c=static_cast<H8CPU*>(opaque);
    c->config.serial_tx_pin(c->config.opaque,Channel,level);
}
template<unsigned Channel> static void serial_clk_pin(void *opaque,int level) {
    H8CPU *c=static_cast<H8CPU*>(opaque);
    c->config.serial_clk_pin(c->config.opaque,Channel,level);
}
static void serial_frame(void *opaque,unsigned channel,u8 byte) {
    H8CPU *c=static_cast<H8CPU*>(opaque);
    if(c->config.serial_tx)c->config.serial_tx(c->config.opaque,channel,byte);
}
void H8CPU::bind_peripherals() {
    adc.setCallbacks(this,adc_input,adc_irq,peripheral_reschedule);
    timers.setCallbacks(this,peripheral_irq,peripheral_reschedule);
    sci[0].setCallbacks(this,peripheral_now,
        config.serial_tx_pin?serial_tx_pin<0>:nullptr,
        config.serial_clk_pin?serial_clk_pin<0>:nullptr,
        peripheral_irq,52,0,serial_frame,peripheral_reschedule,peripheral_rx_now);
    sci[1].setCallbacks(this,peripheral_now,
        config.serial_tx_pin?serial_tx_pin<1>:nullptr,
        config.serial_clk_pin?serial_clk_pin<1>:nullptr,
        peripheral_irq,56,1,serial_frame,peripheral_reschedule,peripheral_rx_now);
    for(unsigned ch=0;ch<2;ch++)
        sci[ch].setExternalClockPeriodAS(config.serial_external_period_as[ch],16934400);
}
u64 H8CPU::next_internal_event() const {
    // Peripheral clocks restart at reset, while board deadlines stay absolute.
    u64 next=adc.next_event();
    next=std::min(next?next:UINT64_MAX,timers.nextDeadline());
    for(unsigned ch=0;ch<2;ch++)
        if(sci[ch].next_event())next=std::min(next,sci[ch].next_event());
    return next==UINT64_MAX?next:cycle_epoch+next;
}
void H8CPU::refresh_bcount() {
    if(!deadline_run)return;
    u64 bound=std::min(run_deadline,next_internal_event());
    u64 span=bound>cycles?bound-cycles:0;
    m_bcount=-int(std::min<u64>(span,0x7fffffff));
}
void H8CPU::internal_update_at(u64 when) {
    // Keep the source device order. SCI reschedules can re-enter here using
    // the charged CPU clock, like MAME's no-argument internal_update().
    when-=cycle_epoch;
    adc.update(when);
    sci[0].update(when);
    sci[1].update(when);
    timers.update(when);
    refresh_bcount();
}
static const int vector_slot[64]={
 -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,0,1,2,2,
 3,3,3,3,4,4,4,4, 5,5,5,5,6,6,6,6,
 7,7,7,7,8,8,8,8, 9,9,9,9,10,10,10,10,
 11,11,11,11,12,12,12,12, 13,13,13,13,14,14,14,14};

void H8CPU::update_irq_filter() {
    // Level-triggered requests stay latched until ISR clear or acknowledge.
    io[0xf6] |= irq_lines & u8(~io[0xf4]);
    u64 candidates=pending;
    for(unsigned i=0;i<6;i++) if((io[0xf5]&io[0xf6])&(1<<i)) candidates|=u64(1)<<(12+i);
    int filter=0;
    if(io[0xf2]&8) filter=(m_CCR&F_I)?2:0;
    else if((m_CCR&(F_I|F_UI))==(F_I|F_UI)) filter=2;
    else if(m_CCR&F_I)filter=1;
    int chosen=0,priority=-1;
    unsigned icr=io[0xf8]|(unsigned(io[0xf9])<<8);
    // Visit pending vectors in the original ascending order, preserving ties.
    candidates &= ~u64(0x7f);
    while(candidates) {
        unsigned i=__builtin_ctzll(candidates); // candidates is nonzero
        candidates &= candidates-1;
        int p=i==7?2:(vector_slot[i]<0?0:(icr>>(vector_slot[i]^7))&1);
        if(p>=filter&&p>priority) {chosen=i;priority=p;}
    }
    m_irq_vector=chosen;
}
void H8CPU::interrupt_taken() {
    pending &= ~(u64(1)<<m_taken_irq_vector);
    if(m_taken_irq_vector>=12&&m_taken_irq_vector<18) {
        unsigned bit=1<<(m_taken_irq_vector-12);
        if((io[0xf4]&bit)||!(irq_lines&bit))io[0xf6]&=~bit;
    }
    update_irq_filter();
}

static unsigned port_number(unsigned reg) {
    switch(reg) {case 0xc7:return 4;case 0xcb:return 6;case 0xce:return 7;
    case 0xcf:return 8;case 0xd2:return 9;case 0xd3:return 10;case 0xd6:return 11;default:return 0;}
}
static unsigned port_ddr(unsigned reg) {
    switch(reg) {case 0xc7:return 0xc5;case 0xcb:return 0xc9;case 0xcf:return 0xcd;
    case 0xd2:return 0xd0;case 0xd3:return 0xd1;case 0xd6:return 0xd4;default:return 0;}
}
u8 H8CPU::bus_read(u32 a) {
    a&=0xffffff;
    if(a>=0xfffd10&&a<=0xffff0f)return ram[a-0xfffd10];
    if(a<0xffff20)return config.read8?config.read8(config.opaque,a):0xff;
    unsigned r=a&255;
    if(r>=0xe0&&r<=0xe9)return adc.read(r-0xe0);
    if(r>=0x60&&r<=0x9f)return timers.readByte(r,h8_current_cycle(this)-cycle_epoch);
    if((r>=0xb0&&r<=0xb5)||(r>=0xb8&&r<=0xbd))
        return sci[(r-0xb0)/8].read((r-0xb0)%8);
    unsigned p=port_number(r);
    if(p) {
        u8 input=config.port_read?config.port_read(config.opaque,p):0xff;
        unsigned d=port_ddr(r);
        u8 ddr=d?io[d]:0;
        u8 fixed=p==6?0x80:p==8?0xe0:p==9?0xc0:0;
        return fixed|(input&~ddr)|(io[r]&ddr);
    }
    switch(r) {case 0xc5:case 0xc9:case 0xcd:case 0xd0:case 0xd1:case 0xd4:return 0xff;}
    if(r>=0x20&&r<=0x3f)unsupported|=1;
    if(r>=0xa8&&r<=0xab)unsupported|=2;
    if(r==0xad)return io[r]|0x80;
    return io[r];
}
void H8CPU::bus_write(u32 a,u8 v) {
    a&=0xffffff;
    if(a>=0xfffd10&&a<=0xffff0f){ram[a-0xfffd10]=v;return;}
    if(a<0xffff20){if(config.write8)config.write8(config.opaque,a,v);return;}
    unsigned r=a&255;
    if(r>=0xe0&&r<=0xe9) {
        adc.write(r-0xe0,v,h8_current_cycle(this)-cycle_epoch);return;
    }
    if(r>=0x60&&r<=0x9f) {
        timers.writeByte(r,v,h8_current_cycle(this)-cycle_epoch);return;
    }
    if((r>=0xb0&&r<=0xb5)||(r>=0xb8&&r<=0xbd)) {
        sci[(r-0xb0)/8].write((r-0xb0)%8,v);return;
    }
    if(r>=0x20&&r<=0x3f)unsupported|=1;
    if(r>=0xa8&&r<=0xab)unsupported|=2;
    // Status bits are write-zero-to-clear. Hardware-set reserved bits are retained.
    if(r==0xf6)io[r]&=v;
    else io[r]=v;
    unsigned p=port_number(r),d=port_ddr(r);
    if(!p) {
        for(unsigned candidate:{0xc7u,0xcbu,0xcfu,0xd2u,0xd3u,0xd6u})
            if(port_ddr(candidate)==r){p=port_number(candidate);d=r;r=candidate;break;}
    }
    if(p&&config.port_write) {
        u8 ddr=d?io[d]:0,fixed=p==6?0x80:p==8?0xe0:p==9?0xc0:0;
        config.port_write(config.opaque,p,(io[r]|~ddr)&~fixed,ddr&~fixed);
    }
    r=a&255;
    if(r==0xf2||r==0xf4||r==0xf5||r==0xf6||r==0xf8||r==0xf9)update_irq_filter();
}

void H8CPU::tick(unsigned elapsed) {
    cycles+=elapsed;
    clock.advance(elapsed);
    while(next_internal_event()<=cycles)
        internal_update_at(next_internal_event());
    update_irq_filter();
}

#include "h8_arithmetic.inc"
#include "h8_instructions.inc"

extern "C" {
H8CPU *h8_create(const H8Config *config) {
    if(!config)return nullptr;
    H8CPU *cpu=new(std::nothrow) H8CPU(*config);
    if(cpu)h8_reset(cpu);
    return cpu;
}
void h8_destroy(H8CPU *cpu) {delete cpu;}
void h8_reset(H8CPU *c) {
    if(!c)return;
    H8Config config=c->config;
    *c=H8CPU(config);
    c->bind_peripherals();
    c->io[0xf2]=0x09;
    c->io[0xc9]=0x80;c->io[0xcd]=0xf0;
    for(unsigned ch=0;ch<2;ch++)c->sci[ch].reset();
}
int h8_soft_reset(H8CPU *c,uint64_t absolute_cycle) {
    if(!c)return -1;
    if(c->in_run)return -2;
    // MAME resets instruction/debt/IRQ/peripheral state while keeping ER/CCR,
    // internal RAM and h8_port DDR/DR/PCR. H83002 maps no ODR register.
    u16 registers[16];u8 memory[0x200],gpio[13],ccr=c->m_CCR;
    static const unsigned gpio_regs[]={
        0xc5,0xc7,0xc9,0xcb,0xcd,0xcf,0xd0,0xd1,0xd2,0xd3,0xd4,0xd6,0xda
    };
    std::memcpy(registers,c->m_R,sizeof(registers));
    std::memcpy(memory,c->ram,sizeof(memory));
    for(unsigned i=0;i<13;i++)gpio[i]=c->io[gpio_regs[i]];
    // Device reset retains SCI's status-read latch and physical RX level.
    arcade::H8Sci serial[2]={c->sci[0],c->sci[1]};
    arcade::H8CycleClock clock=c->clock;
    clock.resetAtCycle(c->cycles,absolute_cycle);
    for(unsigned ch=0;ch<2;ch++) {
        serial[ch].setCallbacks(nullptr,nullptr,nullptr,nullptr,nullptr,0);
        serial[ch].reset();
    }
    H8Config config=c->config;
    *c=H8CPU(config);
    for(unsigned ch=0;ch<2;ch++)c->sci[ch]=serial[ch];
    c->bind_peripherals();
    c->io[0xf2]=0x09;
    std::memcpy(c->m_R,registers,sizeof(registers));
    std::memcpy(c->ram,memory,sizeof(memory));
    for(unsigned i=0;i<13;i++)c->io[gpio_regs[i]]=gpio[i];
    c->m_CCR=ccr;
    c->cycles=c->cycle_epoch=absolute_cycle;
    c->clock=clock;
    c->timers.reset();
    for(unsigned ch=0;ch<2;ch++)c->sci[ch].reset();
    return 0;
}
void h8_run_cycles(H8CPU *c,unsigned requested) {
    if(!c)return;
    c->in_run=true;
    c->budget+=requested;
    if(c->cycle_ahead&&c->budget>0) {
        u64 n=std::min<u64>(c->cycle_ahead,u64(c->budget));
        c->tick(unsigned(n));c->cycle_ahead-=n;c->budget-=int64_t(n);
    }
    while(c->budget>0&&!c->halted) {
        c->update_irq_filter();
        if(c->m_sleeping) {
            if(c->m_irq_vector) {
                c->m_sleeping=false;c->m_inst_state=H8CPU::STATE_IRQ;c->m_taken_irq_vector=c->m_irq_vector;
            } else {
                unsigned n=unsigned(std::min<int64_t>(c->budget,64));
                c->tick(n);c->budget-=n;continue;
            }
        }
        c->m_icount=0;
        if(!c->m_inst_substate&&c->m_inst_state<0x10000){c->m_PPC=c->m_NPC;c->instructions++;}
        c->executing_state=true;
        if(c->m_inst_substate)c->do_exec_partial();
        else c->do_exec_full();
        c->executing_state=false;
        unsigned n=unsigned(-c->m_icount);
        if(!n) {if(c->halted)break;continue;}
        c->tick(n);c->budget-=n;
    }
    if(c->halted)c->budget=0;
    c->in_run=false;
}
int h8_run_until(H8CPU *c,uint64_t target) {
    if(!c)return -1;
    if(c->in_run)return -2;
    if(target<c->cycles)return -1;
    c->in_run=true;c->deadline_run=true;c->run_deadline=target;
    while(c->cycles<target&&!c->halted) {
        // An access can consume one bus operation past the requested deadline.
        // Commit that source-generated debt only as host time advances beyond it.
        if(c->cycle_ahead) {
            u64 n=std::min<u64>(c->cycle_ahead,target-c->cycles);
            c->tick(unsigned(n));c->cycle_ahead-=n;
            if(c->cycles==target)break;
        }
        u64 span=std::min<u64>(target-c->cycles,0x7fffffff);
        c->m_icount=0;c->refresh_bcount();
        c->update_irq_filter();
        if(c->m_sleeping) {
            if(c->m_irq_vector) {
                c->m_sleeping=false;c->m_inst_state=H8CPU::STATE_IRQ;
                c->m_taken_irq_vector=c->m_irq_vector;
            } else {
                unsigned n=unsigned(std::min<u64>(span,64));
                if(c->next_internal_event()>c->cycles)
                    n=unsigned(std::min<u64>(n,c->next_internal_event()-c->cycles));
                c->tick(n);continue;
            }
        }
        if(c->m_inst_substate==0&&c->m_inst_state<0x10000) {
            c->m_PPC=c->m_NPC;c->instructions++;
        }
        c->executing_state=true;
        if(c->m_inst_substate)c->do_exec_partial();
        else c->do_exec_full();
        c->executing_state=false;
        u64 used=c->m_icount<0?u64(-int64_t(c->m_icount)):0;
        if(!used) {
            if(c->halted)break;
            // A partial state may resume immediately after its last timed bus
            // operation and only finish register/prefetch bookkeeping here.
            continue;
        }
        u64 committed=std::min<u64>(used,span);
        c->tick(unsigned(committed));
        if(used>committed) {
            c->cycle_ahead+=used-committed;
            break;
        }
    }
    c->deadline_run=false;c->in_run=false;
    return 0;
}
void h8_set_irq(H8CPU *c,int line,int level) {
    if(!c)return;
    if(line==-1) {
        if(level&&!c->nmi_level)c->pending|=u64(1)<<7;
        c->nmi_level=level!=0;
    } else if(line>=0&&line<6) {
        unsigned bit=1u<<line;
        bool old=(c->irq_lines&bit)!=0;
        if(level)c->irq_lines|=bit;else c->irq_lines&=~bit;
        if(level&&(!(c->io[0xf4]&bit)||!old))c->io[0xf6]|=bit;
    }
    c->update_irq_filter();
}
uint64_t h8_current_cycle(const H8CPU *c) {
    if(!c)return 0;
    // Bus charges precede callbacks; tick already commits those charges before
    // invoking SCI/ADC callbacks, so only add them during instruction execution.
    return c->cycles+(c->executing_state?u64(-int64_t(c->m_icount)):0);
}
void h8_adc_trigger(H8CPU *c,int level) {
    if(!c)return;
    c->adc.trigger(level!=0,h8_current_cycle(c)-c->cycle_epoch);
}
void h8_serial_receive(H8CPU *c,unsigned ch,uint8_t v) {
    if(!c||ch>1)return;
    c->sci[ch].receiveByte(v);
}
void h8_serial_rx_pin(H8CPU *c,unsigned ch,int level) {
    if(c&&ch<2)c->sci[ch].rx_w(level);
}
void h8_serial_clock_pin(H8CPU *c,unsigned ch,int level) {
    if(c&&ch<2)c->sci[ch].clk_w(level);
}
void h8_get_status(const H8CPU *c,H8Status *s) {
    if(!c||!s)return;
    std::memset(s,0,sizeof(*s));s->pc=c->m_NPC;
    for(unsigned i=0;i<8;i++)s->er[i]=c->r32_r(i);
    s->ccr=c->m_CCR;s->cycles=c->cycles-c->cycle_epoch;s->instructions=c->instructions;
    s->sleeping=c->m_sleeping;s->halted=c->halted;
    s->illegal_pc=c->illegal_pc;s->illegal_opcode=c->illegal_opcode;
    s->unsupported_peripherals=c->unsupported;
}
uint8_t h8_read_byte(H8CPU *c,uint32_t a) {return c?c->bus_read(a):0xff;}
void h8_write_byte(H8CPU *c,uint32_t a,uint8_t v) {if(c)c->bus_write(a,v);}
}
