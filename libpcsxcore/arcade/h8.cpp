// SPDX-License-Identifier: BSD-3-Clause
// copyright-holders:Olivier Galibert
// Standalone H8/300H ISA adapter and H8/3002 internal peripheral implementation.
#include "h8.h"
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
    int m_inst_state=STATE_RESET,m_icount=0,m_irq_vector=0,m_taken_irq_vector=0;
    int64_t budget=0;
    bool m_sleeping=false,halted=false,nmi_level=false,adc_level=true;
    u64 cycles=0,instructions=0,pending=0;
    u32 illegal_pc=0,unsupported=0;
    u16 illegal_opcode=0;
    u8 ram[0x200]={},io[0x100]={},irq_lines=0,ssr_read[2]={};
    u64 timer_phase[5]={},serial_deadline[2]={},adc_deadline=0;
    unsigned adc_channel=0;
    u16 adc_sample=0;
    void adc_start() {
        adc_channel=(io[0xe8]&0x10)?(io[0xe8]&4):(io[0xe8]&7);
        adc_sample=config.adc_read?config.adc_read(config.opaque,adc_channel)&0x3ff:0;
        adc_deadline=cycles+((io[0xe8]&8)?134:266);
    }
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
        return (u16(bus_read(a))<<8)|bus_read(a+1);
    }
    u16 read16i(u32 a) {return read16(a);}
    void write16(u32 a,u16 v) {
        m_icount-=2;a&=0xfffffe;
        if(a<0xfffd10&&config.write16){config.write16(config.opaque,a,v);return;}
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
#include "h8_declarations.inc"
#include "h8_arithmetic_declarations.inc"
};

static const unsigned timer_base[5]={0x64,0x6e,0x78,0x82,0x92};
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
    for(unsigned i=7;i<64;i++) if(candidates&(u64(1)<<i)) {
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
    if(r==0x61||r==0x62||r==0x63||r==0x90||r==0x91)return 0;
    for(unsigned i=0;i<5;i++)if(r==timer_base[i]+1)return 0;
    if(r==0xb4||r==0xbc)ssr_read[(r-0xb4)/8]=io[r];
    return io[r];
}
void H8CPU::bus_write(u32 a,u8 v) {
    a&=0xffffff;
    if(a>=0xfffd10&&a<=0xffff0f){ram[a-0xfffd10]=v;return;}
    if(a<0xffff20){if(config.write8)config.write8(config.opaque,a,v);return;}
    unsigned r=a&255;
    if(r>=0x20&&r<=0x3f)unsupported|=1;
    if(r>=0xa8&&r<=0xab)unsupported|=2;
    u8 old=io[r];
    // Status bits are write-zero-to-clear. Hardware-set reserved bits are retained.
    if(r==0xf6)io[r]&=v;
    else if(r==0xb4||r==0xbc) {
        unsigned ch=(r-0xb4)/8,b=0xb0+ch*8;
        bool start=(io[b+2]&0x20)&&(old&ssr_read[ch]&0x80)&&!(v&0x80);
        u8 next=start?old&~0x84:old;
        io[r]=(next&(~ssr_read[ch]|v|0x86)&~1)|(v&1);
        ssr_read[ch]&=io[r];
        if(start&&!serial_deadline[ch]) {
            unsigned divider=(io[b]&0x80)?4:32;
            unsigned bits=(io[b]&0x80)?8:((io[b]&0x40)?9:10)+((io[b]&0x20)?1:0)+((io[b]&8)?1:0);
            serial_deadline[ch]=cycles+u64(divider)*(io[b+1]+1)*(1u<<(2*(io[b]&3)))*bits;
        }
    } else if(r==0xe8)io[r]=(v&0x7f)|(old&v&0x80);
    else {
        bool status=false;
        for(unsigned i=0;i<5;i++)if(r==timer_base[i]+3)status=true;
        io[r]=status ? ((old&v&7)|0xf8) : v;
    }
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
    if(r==0xe8&&(v&0x20)&&!(old&0x20))adc_start();
    // Enabling TIE while TDRE is already set generates TXI.
    if(r==0xb2||r==0xba) {
        unsigned ch=(r-0xb2)/8;
        if((v&~old&0x80)&&(io[r+2]&0x80))pending|=u64(1)<<(54+ch*4);
        if((v&~old&4)&&(io[r+2]&4))pending|=u64(1)<<(55+ch*4);
        if((v&~old&0x40)&&(io[r+2]&0x40))pending|=u64(1)<<(53+ch*4);
        if((v&~old&0x40)&&(io[r+2]&0x38))pending|=u64(1)<<(52+ch*4);
    }
}

void H8CPU::tick(unsigned elapsed) {
    cycles+=elapsed;
    for(unsigned ch=0;ch<5;ch++)if(io[0x60]&(1<<ch)) {
        unsigned b=timer_base[ch],tcr=io[b],clock=tcr&7;
        if(clock>3)continue; // External clocks require board wiring.
        unsigned shift=clock;
        if(clock>1&&(tcr&0x10))shift--;
        u64 phase=timer_phase[ch]+elapsed,ticks=phase>>shift;
        timer_phase[ch]=phase&((1u<<shift)-1);
        u32 count=(u32(io[b+4])<<8)|io[b+5];
        u32 ca=(u32(io[b+6])<<8)|io[b+7],cb=(u32(io[b+8])<<8)|io[b+9];
        while(ticks--) {
            unsigned events=(count==ca?1:0)|(count==cb?2:0)|(count==65535?4:0);
            count=(count+1)&65535;
            io[b+3]|=events;
            for(unsigned event=0;event<3;event++)if(events&io[b+2]&(1<<event))pending|=u64(1)<<(24+ch*4+event);
            unsigned clear=(tcr>>5)&3;
            if((clear==1&&(events&1))||(clear==2&&(events&2)))count=0;
        }
        io[b+4]=count>>8;io[b+5]=count;
    }
    for(unsigned ch=0;ch<2;ch++)if(serial_deadline[ch]&&cycles>=serial_deadline[ch]) {
        unsigned b=0xb0+ch*8;
        serial_deadline[ch]=0;io[b+4]|=0x84;
        if(config.serial_tx)config.serial_tx(config.opaque,ch,io[b+3]);
        if(io[b+2]&0x80)pending|=u64(1)<<(54+ch*4);
        if(io[b+2]&4)pending|=u64(1)<<(55+ch*4);
    }
    if(adc_deadline&&cycles>=adc_deadline) {
        unsigned r=0xe0+((adc_channel&3)*2);
        io[r]=adc_sample>>2;io[r+1]=adc_sample<<6;
        bool scan=(io[0xe8]&0x10)!=0;
        bool last=!scan||adc_channel==(io[0xe8]&7);
        if(last) {
            io[0xe8]|=0x80;
            if(io[0xe8]&0x40)pending|=u64(1)<<60;
        }
        if(scan) {
            adc_channel=last?(io[0xe8]&4):((adc_channel+1)&7);
            adc_sample=config.adc_read?config.adc_read(config.opaque,adc_channel)&0x3ff:0;
            adc_deadline=cycles+((io[0xe8]&8)?128:256);
        } else {adc_deadline=0;io[0xe8]&=~0x20;}
    }
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
    c->io[0xf2]=0x09;
    c->io[0x60]=0xe0;c->io[0x61]=0xe0;c->io[0x62]=0x80;c->io[0x63]=0xe0;
    c->io[0x90]=0xff;c->io[0x91]=0xff;
    c->io[0xc9]=0x80;c->io[0xcd]=0xf0;
    for(unsigned i=0;i<5;i++) {
        unsigned b=timer_base[i];c->io[b+2]=0xf8;c->io[b+3]=0xf8;
        c->io[b+6]=c->io[b+7]=c->io[b+8]=c->io[b+9]=0xff;
    }
    for(unsigned i=0;i<2;i++) {
        unsigned b=0xb0+i*8;c->io[b+1]=0xff;c->io[b+3]=0xff;c->io[b+4]=0x84;
    }
}
void h8_run_cycles(H8CPU *c,unsigned requested) {
    if(!c)return;
    c->budget+=requested;
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
        if(c->m_inst_state<0x10000){c->m_PPC=c->m_NPC;c->instructions++;}
        c->do_exec_full();
        unsigned n=unsigned(-c->m_icount);
        if(!n) {c->illegal();break;}
        c->tick(n);c->budget-=n;
    }
    if(c->halted)c->budget=0;
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
void h8_adc_trigger(H8CPU *c,int level) {
    if(!c)return;
    bool next=level!=0;
    if(c->adc_level&&!next&&(c->io[0xe9]&0x80)&&!(c->io[0xe8]&0x20)) {
        c->io[0xe8]|=0x20;c->adc_start();
    }
    c->adc_level=next;
}
void h8_serial_receive(H8CPU *c,unsigned ch,uint8_t v) {
    if(!c||ch>1)return;
    unsigned b=0xb0+ch*8;
    if(!(c->io[b+2]&0x10))return;
    if(c->io[b+4]&0x40){c->io[b+4]|=0x20;if(c->io[b+2]&0x40)c->pending|=u64(1)<<(52+ch*4);}
    else {c->io[b+5]=v;c->io[b+4]|=0x40;if(c->io[b+2]&0x40)c->pending|=u64(1)<<(53+ch*4);}
    c->update_irq_filter();
}
void h8_get_status(const H8CPU *c,H8Status *s) {
    if(!c||!s)return;
    std::memset(s,0,sizeof(*s));s->pc=c->m_NPC;
    for(unsigned i=0;i<8;i++)s->er[i]=c->r32_r(i);
    s->ccr=c->m_CCR;s->cycles=c->cycles;s->instructions=c->instructions;
    s->sleeping=c->m_sleeping;s->halted=c->halted;
    s->illegal_pc=c->illegal_pc;s->illegal_opcode=c->illegal_opcode;
    s->unsupported_peripherals=c->unsupported;
}
uint8_t h8_read_byte(H8CPU *c,uint32_t a) {return c?c->bus_read(a):0xff;}
void h8_write_byte(H8CPU *c,uint32_t a,uint8_t v) {if(c)c->bus_write(a,v);}
}
