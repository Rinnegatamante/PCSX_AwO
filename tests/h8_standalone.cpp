// SPDX-License-Identifier: BSD-3-Clause
#include "../libpcsxcore/arcade/h8.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
struct Bus {
    std::vector<uint8_t> memory;
    unsigned writes=0,shared_writes=0,tx_count=0;
    uint8_t last_tx=0;
    uint16_t adc_values[8]={};
    unsigned atomic_pcm_reads=0,atomic_pcm_writes=0;
    Bus():memory(0x1000000,0xff) {}
};
static uint8_t read_bus(void *p,uint32_t a){return static_cast<Bus*>(p)->memory[a];}
static void write_bus(void *p,uint32_t a,uint8_t v){auto &b=*static_cast<Bus*>(p);b.memory[a]=v;b.writes++;if(a>=0x80000&&a<0x90000)b.shared_writes++;}
static uint8_t read_port(void*,unsigned p){return p==6?0xff:p==8?0xef:0xff;}
static void serial_tx(void*p,unsigned,uint8_t v){auto &b=*static_cast<Bus*>(p);b.tx_count++;b.last_tx=v;}
static uint16_t read_adc(void*p,unsigned ch){return static_cast<Bus*>(p)->adc_values[ch];}
static uint16_t read_word(void*p,uint32_t a){auto &b=*static_cast<Bus*>(p);if(a>=0x280000&&a<0x288000)b.atomic_pcm_reads++;return (b.memory[a]<<8)|b.memory[a+1];}
static void write_word(void*p,uint32_t a,uint16_t v){auto &b=*static_cast<Bus*>(p);if(a>=0x280000&&a<0x288000)b.atomic_pcm_writes++;write_bus(p,a,v>>8);write_bus(p,a+1,v);}
static void word(Bus &b,unsigned a,unsigned v){b.memory[a]=v>>8;b.memory[a+1]=v;}
static void longword(Bus &b,unsigned a,uint32_t v){word(b,a,v>>16);word(b,a+2,v);}
static void code(Bus &b,unsigned a,std::initializer_list<uint16_t> w){for(auto v:w){word(b,a,v);a+=2;}}
static H8CPU *create(Bus &b,bool atomic=false){H8Config cfg{};cfg.opaque=&b;cfg.read8=read_bus;cfg.write8=write_bus;cfg.port_read=read_port;cfg.serial_tx=serial_tx;cfg.read16=atomic?read_word:nullptr;cfg.write16=atomic?write_word:nullptr;cfg.adc_read=read_adc;return h8_create(&cfg);}
static H8Status status(H8CPU *c){H8Status s;h8_get_status(c,&s);return s;}
static void synthetic_tests() {
    {
        Bus b;longword(b,0,0x100);
        code(b,0x100,{0x7900,0x1234,0x6ba0,0x0028,0x0404,0x6b21,0x0028,0x0404,0x0180});
        H8CPU*c=create(b,true);h8_run_cycles(c,100);auto s=status(c);
        assert(!s.halted&&s.er[1]==0x1234&&b.atomic_pcm_reads==1&&b.atomic_pcm_writes==1);
        h8_destroy(c);
    }
    {
        Bus b;longword(b,0,0x100);
        code(b,0x100,{0x7a00,0xfedc,0xba98,0x7a50,0xffff,0xffff,0x7a10,0x0000,0x0001,
                      0x0100,0x6ba0,0x0008,0x0040,0x0100,0x6b21,0x0008,0x0040,0x0180});
        H8CPU*c=create(b);h8_run_cycles(c,100);
        auto s=status(c);assert(!s.halted&&s.sleeping);assert(s.er[0]==0x01234568&&s.er[1]==s.er[0]);
        assert(b.memory[0x80040]==1&&b.memory[0x80043]==0x68);h8_destroy(c);
    }
    {
        Bus b;longword(b,0,0x100);
        // MOV.B #0x7f,R0L; ADD.B #1,R0L; MOV.W #0x1234,R1; MOV.L #stack,ER7;
        // JSR @0x200; MOV.L ER7,ER2; SLEEP. Subroutine increments R1 and returns.
        code(b,0x100,{0xf87f,0x8801,0x7901,0x1234,0x7a07,0x00ff,0xfe80,
                      0x5e00,0x0200,0x0ff2,0x0180});
        code(b,0x200,{0x0b51,0x5470});
        H8CPU *c=create(b);h8_run_cycles(c,100);
        auto s=status(c);assert(!s.halted&&s.sleeping);assert((s.er[0]&255)==0x80);
        assert(s.er[1]==0x1235);assert(s.er[2]==0xfffe80&&s.er[7]==0xfffe80);
        assert(h8_read_byte(c,0xfffe7f)==0x12); // return address 0x112
        h8_destroy(c);
    }
    {
        Bus b;longword(b,0,0x100);longword(b,13*4,0x300);
        code(b,0x100,{0x7a07,0x00ff,0xfe80,0x0700,0x0180,0x0180});
        code(b,0x300,{0xfa5a,0x5670}); // IRQ1 moves #0x5a,R2L and RTE.
        H8CPU *c=create(b);h8_write_byte(c,0xfffff5,2);h8_run_cycles(c,40);
        assert(status(c).sleeping);h8_set_irq(c,1,1);h8_set_irq(c,1,0);h8_run_cycles(c,80);
        auto s=status(c);assert(!s.halted&&s.sleeping);assert(s.er[2]==0x5a&&s.er[7]==0xfffe80);
        assert((s.ccr&0x80)==0);h8_destroy(c);
    }
    {
        Bus b;longword(b,0,0x100);code(b,0x100,{0x0180});H8CPU *c=create(b);
        // Timer0: divide-by-eight, clear on GRA match, compare IRQ enabled.
        h8_write_byte(c,0xffff64,0x23);h8_write_byte(c,0xffff66,1);
        h8_write_byte(c,0xffff6a,0);h8_write_byte(c,0xffff6b,9);h8_write_byte(c,0xffff60,1);
        h8_run_cycles(c,80);assert(h8_read_byte(c,0xffff67)&1);
        assert(h8_read_byte(c,0xffff68)==0&&h8_read_byte(c,0xffff69)<9);
        // Source SCI transfers TDR immediately (TDRE), then completes the wire frame.
        h8_write_byte(c,0xffffb1,0);h8_write_byte(c,0xffffb2,0x30);
        h8_write_byte(c,0xffffb3,0x42);h8_read_byte(c,0xffffb4);h8_write_byte(c,0xffffb4,4);
        assert((h8_read_byte(c,0xffffb4)&0x84)==0x80);h8_run_cycles(c,400);
        assert(b.tx_count==1&&b.last_tx==0x42);assert((h8_read_byte(c,0xffffb4)&0x84)==0x84);
        h8_serial_receive(c,0,0x33);assert(h8_read_byte(c,0xffffb5)==0x33);assert(h8_read_byte(c,0xffffb4)&0x40);
        h8_write_byte(c,0xffffb4,0x84);assert(!(h8_read_byte(c,0xffffb4)&0x40));
        h8_write_byte(c,0xffffe8,0x20);h8_run_cycles(c,300);assert((h8_read_byte(c,0xffffe8)&0xa0)==0x80);
        // External ADC trigger requires the falling pin edge, not just enable.
        b.adc_values[3]=0x155;
        h8_write_byte(c,0xffffe8,3);h8_write_byte(c,0xffffe9,0x80);
        h8_adc_trigger(c,1);h8_run_cycles(c,300);assert(!(h8_read_byte(c,0xffffe8)&0x80));
        h8_adc_trigger(c,0);assert(h8_read_byte(c,0xffffe8)&0x20);
        h8_run_cycles(c,300);assert((h8_read_byte(c,0xffffe8)&0xa0)==0x80);
        assert(h8_read_byte(c,0xffffe6)==0x55&&h8_read_byte(c,0xffffe7)==0x40);
        b.adc_values[0]=0x3ff;b.adc_values[1]=0x200;
        h8_write_byte(c,0xffffe8,0x31);h8_run_cycles(c,600);
        assert(h8_read_byte(c,0xffffe8)&0x80);
        assert(h8_read_byte(c,0xffffe0)==0xff&&h8_read_byte(c,0xffffe1)==0xc0);
        assert(h8_read_byte(c,0xffffe2)==0x80&&h8_read_byte(c,0xffffe3)==0);
        h8_destroy(c);
    }
    {
        Bus b;longword(b,0,0x100);code(b,0x100,{0x0101});H8CPU*c=create(b);h8_run_cycles(c,100);
        auto s=status(c);assert(s.halted&&s.illegal_pc==0x100&&s.illegal_opcode==0x0101);h8_destroy(c);
    }
    {
        // Different scheduler slice lengths must produce the same final instruction state.
        Bus a,b;longword(a,0,0x100);code(a,0x100,{0xf800,0x8801,0x40fc});b.memory=a.memory;
        H8CPU *ca=create(a),*cb=create(b);h8_run_cycles(ca,1000);
        for(int i=0;i<1000;i++)h8_run_cycles(cb,1);
        auto sa=status(ca),sb=status(cb);assert(sa.pc==sb.pc&&sa.er[0]==sb.er[0]&&sa.cycles==sb.cycles);
        h8_destroy(ca);h8_destroy(cb);
    }
    std::puts("H8 synthetic instruction, IRQ, timer, SCI, ADC and scheduling tests passed");
}
static int run_rom(const char *path,unsigned frames) {
    Bus b;std::ifstream f(path,std::ios::binary);if(!f){std::fprintf(stderr,"Cannot read %s\n",path);return 1;}
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
    if(rom.size()!=0x80000){std::fprintf(stderr,"Expected logical BE 512KiB sub ROM\n");return 1;}
    std::copy(rom.begin(),rom.end(),b.memory.begin());
    std::fill(b.memory.begin()+0x80000,b.memory.begin()+0x90000,0);
    std::fill(b.memory.begin()+0x280000,b.memory.begin()+0x288000,0);
    H8CPU*c=create(b);
    for(unsigned frame=0;frame<frames&&!status(c).halted;frame++) {
        h8_set_irq(c,1,1);h8_adc_trigger(c,1);h8_run_cycles(c,16384);h8_set_irq(c,1,0);h8_adc_trigger(c,0);h8_run_cycles(c,16934400/60-16384);
        if(frame<3||frame==5||frame%60==59) {auto s=status(c);std::printf("frame=%u pc=%06x ccr=%02x cycles=%llu instructions=%llu writes=%u shared=%u tx=%u sp=%08x\n",
            frame+1,s.pc,s.ccr,(unsigned long long)s.cycles,(unsigned long long)s.instructions,b.writes,b.shared_writes,b.tx_count,s.er[7]);}
        if(frame==5||frame==59||frame==119) {
            auto s=status(c);
            std::printf("H8_MILESTONE requested=%.6f pc=%06x CCR=%02x\n",(frame+1)/60.0,s.pc,s.ccr);
            for(unsigned i=0;i<8;i++)std::printf("ER%u=%08x%s",i,s.er[i],i==7?"\n":" ");
            for(auto base:{0xffffb0u,0xfffff2u,0xffff60u,0x080400u,0x087fe0u}) {
                unsigned n=base==0xffff60?64:base==0xfffff2?8:base==0xffffb0?16:32;
                std::printf("H8_BYTES base=%06x bytes=",base);
                for(unsigned i=0;i<n;i++)std::printf("%02x%s",h8_read_byte(c,base+i),i+1==n?"\n":" ");
            }
        }
    }
    auto s=status(c);std::printf("final pc=%06x halted=%d illegal=%06x:%04x unsupported=%x shared0400=%02x%02x shared7ffe=%02x%02x\n",
        s.pc,s.halted,s.illegal_pc,s.illegal_opcode,s.unsupported_peripherals,b.memory[0x80400],b.memory[0x80401],b.memory[0x87ffe],b.memory[0x87fff]);
    std::printf("SCI0=%02x,%02x,%02x,%02x,%02x SCI1=%02x,%02x,%02x,%02x,%02x timerTSTR=%02x\n",h8_read_byte(c,0xffffb0),h8_read_byte(c,0xffffb1),h8_read_byte(c,0xffffb2),h8_read_byte(c,0xffffb3),h8_read_byte(c,0xffffb4),h8_read_byte(c,0xffffb8),h8_read_byte(c,0xffffb9),h8_read_byte(c,0xffffba),h8_read_byte(c,0xffffbb),h8_read_byte(c,0xffffbc),h8_read_byte(c,0xffff60));
    h8_destroy(c);return s.halted?1:0;
}
int main(int argc,char **argv){synthetic_tests();if(argc>1)return run_rom(argv[1],argc>2?std::strtoul(argv[2],nullptr,0):120);return 0;}
