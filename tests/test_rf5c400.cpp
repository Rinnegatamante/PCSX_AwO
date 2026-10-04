/* Fixed PCM and clock/register protocol vectors for pinned MAME RF5C400.
 * Synthetic memory only; does not validate Twinkle gameplay or audio quality. */
#include "rf5c400.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
struct Fixture {
    RF5C400 *d=nullptr;
    std::array<uint16_t,128> memory{};
    std::vector<std::array<int32_t,4>> frames;
    std::vector<uint64_t> times;
    uint32_t last_read=0,last_write=0;
    uint16_t written=0;
    bool reentry=false;
    static uint16_t read(void *p,uint32_t a) {
        auto &f=*static_cast<Fixture*>(p); f.last_read=a;
        assert(!(a&1)&&a<=RF5C400_ADDRESS_MASK);
        if(f.reentry) f.check_reentry();
        return f.memory[(a/2)%f.memory.size()];
    }
    static void write(void *p,uint32_t a,uint16_t v) {
        auto &f=*static_cast<Fixture*>(p);f.last_write=a;f.written=v;
        if(f.reentry) f.check_reentry();
        f.memory[(a/2)%f.memory.size()]=v;
    }
    static void sample(void *p,const int32_t *o,uint64_t t) {
        auto &f=*static_cast<Fixture*>(p);f.frames.push_back({o[0],o[1],o[2],o[3]});f.times.push_back(t);
        RF5C400Status s{};rf5c400_get_status(f.d,&s);assert(s.cycles==t);
        if(f.reentry) f.check_reentry();
    }
    explicit Fixture(uint32_t clock=0, uint64_t origin=0) {
        RF5C400Config c{};c.clock_hz=clock;c.cycle_origin=origin;c.read_word=read;c.write_word=write;
        c.memory_opaque=this;c.sample=sample;c.sample_opaque=this;
        assert(rf5c400_create(&c,&d)==0);
    }
    ~Fixture(){rf5c400_destroy(d);}
    void w(unsigned r,uint16_t v,uint16_t m=0xffff){assert(rf5c400_write(d,r,v,m)==0);}
    uint16_t r(unsigned off){uint16_t v=0xa5a5;assert(rf5c400_read(d,off,0xffff,&v)==0);return v;}
    RF5C400Channel ch(unsigned n=0){RF5C400Channel s{};assert(rf5c400_get_channel(d,n,&s)==0);return s;}
    RF5C400Status status(){RF5C400Status s{};rf5c400_get_status(d,&s);return s;}
    void setup(unsigned c=0,uint16_t type=0,unsigned start=0,unsigned end=127,unsigned loop=128) {
        unsigned b=0x400+c*32;
        w(b,0);w(b+1,uint16_t(start));w(b+2,0x5000); // 0x1000 << 2, times four = 65536
        w(b+3,uint16_t(end));w(b+4,0);w(b+5,uint16_t(loop));
        w(b+6,0);w(b+7,0);w(b+8,type);w(b+9,0);w(b+12,0x80);w(b+14,0);
        w(1,uint16_t(0x60|c));
    }
    std::array<int32_t,4> one(){std::array<int32_t,4> o{};assert(rf5c400_render(d,1,o.data(),1)==0);return o;}
    void check_reentry(){
        uint16_t v=0xa55a;
        assert(rf5c400_read(d,0,0xffff,&v)==RF5C400_REENTRANT&&v==0xa55a);
        assert(rf5c400_write(d,0,0xffff,0xffff)==RF5C400_REENTRANT);
        assert(rf5c400_reset(d)==RF5C400_REENTRANT);
        assert(rf5c400_advance_to_cycle(d,0)==RF5C400_REENTRANT);
        assert(rf5c400_run_cycles(d,UINT64_MAX)==RF5C400_REENTRANT);
        assert(rf5c400_render(d,1,nullptr,0)==RF5C400_REENTRANT);
        rf5c400_destroy(d); // must leave the active device alive
    }
};
static void pcm_vectors(){
    Fixture f;f.memory.fill(0x4000);f.setup();
    auto a=f.one();assert((a==std::array<int32_t,4>{8160,8160,8160,8160}));
    f.w(0x406,0x4700);f.w(0x407,0x0047);
    a=f.one();assert((a==std::array<int32_t,4>{8160,0,0,8160}));
    f.w(0x406,0x4848);f.w(0x407,0xffff);a=f.one();assert((a==std::array<int32_t,4>{0,0,0,0}));
    f.w(0x406,0);f.w(0x407,0);f.memory.fill(0xc000);
    a=f.one();assert(a[0]==-8161);
    f.memory.fill(0x1234);f.w(0x408,0x4000);a=f.one();assert(a[0]==6630);
    f.w(0x408,0x8000);a=f.one();assert(a[0]==2295);
    f.w(0x408,0xc000);a=f.one();assert(a[0]==0); // reserved PCM type
    f.w(1,0);a=f.one();assert(a[0]==0&&f.ch().env_phase==0);
}
static void envelopes_and_loop(){
    Fixture f;f.memory.fill(0x4000);f.setup(0,0,10,12,3);
    f.one();assert((f.ch().pos>>16)==11);f.one();assert((f.ch().pos>>16)==12);
    f.one();assert((f.ch().pos>>16)==10); // inclusive end, three-word loop
    f.setup(0,0,100,102,20);f.one();f.one();f.one();assert((f.ch().pos>>16)==100); // oversized loop clamps
    f.w(0x409,0xff00);f.w(1,0x60);auto a=f.one();assert(a[0]==0&&f.ch().env_phase==1);
    f.w(0x409,0);f.w(1,0x60);f.one();assert(f.ch().env_phase==2&&f.ch().env_level==1);
    f.w(0x40e,0x80);f.w(1,0x40);f.one();assert(f.ch().env_phase==3&&f.ch().env_level==1);
    f.w(0x40e,0);f.w(1,0x40);assert(rf5c400_render(f.d,7000,nullptr,0)==0);assert(f.ch().env_phase==0);
    f.setup(1,0,8,8,0);RF5C400Channel before=f.ch(1);f.one();assert(f.ch(1).pos==before.pos&&f.ch(1).env_level==0);
    f.setup(31);f.w(0x7e2,0xffff);assert(f.ch(31).step==uint64_t((0x1fff<<7)*4));
}
static void memory_and_registers(){
    Fixture f;f.reentry=true;
    f.w(0,0x1234,0x00ff);assert(f.r(0)==0x1234); // MAME ignores partial mask
    f.w(0x11,0xabcd);f.w(0x12,0xffff);f.w(0x13,0x5511);f.w(0x14,2);assert(f.written==0);
    f.w(0x14,3);assert(f.last_write==0x1ff579a&&f.written==0x5511);assert(f.r(0x13)==0x5511);
    f.w(0x09,0x6789);assert((f.status().external_address&0xffff)==0x6789); // source fallthrough
    f.w(0x08,0xffff);assert(f.status().requested_channel==31);
    assert(f.r(0x04)==0&&f.r(0x40f)==0);
    f.setup();f.w(0x08,0);assert(rf5c400_render(f.d,64,nullptr,0)==0);assert(f.r(0x09)==1);
    const auto before=f.ch();assert(rf5c400_reset(f.d)==0);assert(f.ch().pos==before.pos&&f.r(0)==0x1234);
    uint16_t v=0xbeef;assert(rf5c400_read(f.d,0x800,0xffff,&v)==RF5C400_ARGUMENT&&v==0xbeef);
}
static void clocks_and_mixing(){
    Fixture a,b;a.memory.fill(0x4000);b.memory=a.memory;a.setup();b.setup();a.reentry=true;b.reentry=true;
    assert(rf5c400_run_cycles(a.d,10000)==0);
    const uint64_t spans[]={1,383,77,991,7,4096,4445};
    for(uint64_t s:spans)assert(rf5c400_run_cycles(b.d,s)==0);
    assert(a.frames==b.frames&&a.times==b.times);assert(a.frames.size()==26);
    for(size_t i=0;i<a.times.size();i++)assert(a.times[i]==(i+1)*384);
    assert(a.status().sample_phase==b.status().sample_phase&&a.status().cycles==10000);
    assert(rf5c400_advance_to_cycle(a.d,1)==RF5C400_TIME_REWIND);
    assert(rf5c400_run_cycles(a.d,UINT64_MAX)==RF5C400_OVERFLOW&&a.status().cycles==10000);
    Fixture horizon(0,UINT64_MAX-768);
    assert(rf5c400_run_cycles(horizon.d,768)==0&&horizon.frames.size()==2);
    assert(horizon.times[0]==UINT64_MAX-384&&horizon.times[1]==UINT64_MAX);
    assert(rf5c400_run_cycles(horizon.d,1)==RF5C400_OVERFLOW&&horizon.frames.size()==2);
    Fixture other(1000003);assert(other.status().sample_rate==2604);
    assert(rf5c400_run_cycles(other.d,1000003)==0);assert(other.frames.size()==2604&&other.times.back()==1000003);
    Fixture mix;mix.memory.fill(0x7fff);for(unsigned c=0;c<32;c++)mix.setup(c);
    const auto o=mix.one();assert(o[0]==522208&&o[1]==522208); // retain unclipped voice sum
    uint8_t rom[]={0,0x40};RF5C400Config cfg{};cfg.sample_rom=rom;cfg.sample_rom_size=sizeof(rom);
    RF5C400 *d=nullptr;assert(rf5c400_create(&cfg,&d)==0);
    uint16_t value;assert(rf5c400_write(d,0x11,1,0xffff)==0);assert(rf5c400_read(d,0x13,0xffff,&value)==0&&value==0xffff);
    rf5c400_destroy(d);cfg.clock_hz=383;assert(rf5c400_create(&cfg,&d)==RF5C400_ARGUMENT&&d==nullptr);
}
int main(){pcm_vectors();envelopes_and_loop();memory_and_registers();clocks_and_mixing();puts("RF5C400 PCM/envelope/register/clock vectors PASS");}
