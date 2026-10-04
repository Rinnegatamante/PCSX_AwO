/* Independent fixed vectors and protocol traces for the pinned MAME ZSG-2 port.
 * Synthetic packed ROM fixture only; no authentic Taito sound media. */
#include "zsg2.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int32_t signed_width(int64_t value, unsigned bits)
{
    const int64_t span = int64_t(1) << bits;
    value = (value % span + span) % span;
    return int32_t(value >= span / 2 ? value - span : value);
}
static int32_t floor_shift(int64_t value, unsigned bits)
{ return int32_t(std::floor(double(value) / double(uint64_t(1) << bits))); }
static int16_t clip(int64_t value)
{ return int16_t(std::max<int64_t>(-32768, std::min<int64_t>(32767, value))); }
static uint32_t encode(const std::array<int,4> &s, unsigned shift)
{
    unsigned a = s[0] & 127, b = s[1] & 127, c = s[2] & 127, d = s[3] & 127;
    return (a << 8) | (b << 16) | (c << 24) | ((d & 64) << 9) | ((d & 32) << 18) |
        ((d & 16) << 27) | (d & 15) | (shift << 4);
}
static void put_word(std::vector<uint8_t> &rom, size_t index, uint32_t value)
{ for (unsigned i = 0; i < 4; i++) rom[index*4+i] = uint8_t(value >> (i*8)); }
static std::array<int16_t,4> decode_ref(uint32_t value)
{
    std::array<int16_t,4> out{};
    unsigned a[] = {(value >> 8) & 127, (value >> 16) & 127, (value >> 24) & 127,
        ((value >> 9) & 64) | ((value >> 18) & 32) | ((value >> 27) & 16) | (value & 15)};
    for (unsigned i = 0; i < 4; i++) out[i] = int16_t(floor_shift((a[i] >= 64 ? int(a[i])-128 : int(a[i])) * 512, (value >> 4) & 15));
    return out;
}
static void reject_reentry(ZSG2 *d)
{
    assert(d); int16_t samples[4]; uint16_t value;
    assert(zsg2_write(d, 0, 0xffff, 0xffff) == ZSG2_REENTRANT);
    assert(zsg2_read(d, 0, 0xffff, &value) == ZSG2_REENTRANT);
    assert(zsg2_run_cycles(d, 500000) == ZSG2_REENTRANT);
    assert(zsg2_advance_to_cycle(d, UINT64_MAX) == ZSG2_REENTRANT);
    assert(zsg2_advance_to_cycle(d, 0) == ZSG2_REENTRANT);
    assert(zsg2_decode_block(d, 0, samples) == ZSG2_REENTRANT);
    assert(zsg2_stream_update(d) == ZSG2_REENTRANT);
    assert(zsg2_render_update(d, 0, nullptr, 0) == ZSG2_REENTRANT);
    assert(zsg2_reset(d) == ZSG2_REENTRANT);
}
struct Capture {
    std::vector<std::array<int16_t,4>> frames;
    std::vector<uint64_t> times;
    uint64_t bound = UINT64_MAX;
    ZSG2 *device = nullptr;
    unsigned diagnostic_count = 0;
    static void sample(void *opaque, const int16_t *out, uint64_t cycle) {
        auto &c = *static_cast<Capture *>(opaque); assert(cycle <= c.bound);
        c.frames.push_back({out[0],out[1],out[2],out[3]}); c.times.push_back(cycle);
        if (c.device) reject_reentry(c.device);
    }
    static void diag(void *opaque, uint32_t code, uint32_t, uint16_t) {
        assert(code); auto &c=*static_cast<Capture *>(opaque); c.diagnostic_count++; if(c.device) reject_reentry(c.device);
    }
};
struct Device {
    ZSG2 *d = nullptr; std::vector<uint8_t> rom;
    explicit Device(size_t words = 64, Capture *cap = nullptr, uint32_t clock = ZSG2_DEFAULT_CLOCK) : rom(words * 4) {
        ZSG2Config cfg{}; cfg.clock_hz = clock; cfg.sample_rom = rom.data(); cfg.sample_rom_size = rom.size();
        if (cap) { cfg.sample = Capture::sample; cfg.sample_opaque = cap; cfg.diagnostic = Capture::diag; cfg.diagnostic_opaque = cap; }
        assert(zsg2_create(&cfg, &d) == ZSG2_OK);
    }
    ~Device() { zsg2_destroy(d); }
    void write(uint32_t offset, uint16_t value) { assert(zsg2_write(d, offset, value, 0xffff) == ZSG2_OK); }
    uint16_t read(uint32_t offset) { uint16_t value; assert(zsg2_read(d, offset, 0xffff, &value) == ZSG2_OK); return value; }
    ZSG2Channel channel(unsigned n=0) { ZSG2Channel c{}; assert(zsg2_get_channel(d,n,&c)==ZSG2_OK); return c; }
    ZSG2Status status() { ZSG2Status s{}; zsg2_get_status(d,&s); return s; }
    void batch(size_t n) { assert(zsg2_render_update(d,n,nullptr,0)==ZSG2_OK); }
};
static void configure(Device &d, unsigned channel, uint16_t pitch=0x3fff, unsigned loop=0, unsigned end=16)
{
    unsigned base=channel*16;
    d.write(base+0,0);d.write(base+1,0);d.write(base+3,0x0400);d.write(base+4,pitch);
    d.write(base+5,uint16_t(0x1f00|(loop&255)));d.write(base+6,uint16_t(end));d.write(base+7,uint16_t(0x1f00|(loop>>8)));
    d.write(base+8,0xffff);d.write(base+10,0xface);d.write(base+12,0xffff);d.write(base+14,0xffff);
    d.write(base+13,0x1f00);d.write(base+15,0x1f00);d.write(0x300+channel/16,uint16_t(1u<<(channel&15)));
}
static void decode_vectors()
{
    Device d(16);
    put_word(d.rom,0,0x7f7f7f0f);
    int16_t out[4]; assert(zsg2_decode_block(d.d,0,out)==ZSG2_OK);
    assert(out[0]==-512&&out[1]==-512&&out[2]==-512&&out[3]==7680);
    put_word(d.rom,0,0x80808000);zsg2_decode_block(d.d,0,out);
    assert(out[0]==0&&out[1]==0&&out[2]==0&&out[3]==-8192);
    unsigned vectors=0;
    for (unsigned shift=0;shift<16;shift++) for(int n=-64;n<64;n++) {
        std::array<int,4> values{n,63-n/2,-64+n/2,n^31};
        uint32_t block=encode(values,shift);put_word(d.rom,0,block);zsg2_decode_block(d.d,0,out);
        auto expected=decode_ref(block);assert(std::equal(expected.begin(),expected.end(),out));vectors++;
    }
    /* Source zero/OOB alias is decoded[rom_words], here block four's cache. */
    uint32_t alias_word=encode({5,-3,11,-7},1);put_word(d.rom,4,alias_word);
    zsg2_decode_block(d.d,4,out);auto expected=decode_ref(alias_word);
    put_word(d.rom,0,0);zsg2_decode_block(d.d,0,out);assert(std::equal(expected.begin(),expected.end(),out));
    zsg2_decode_block(d.d,16,out);assert(std::equal(expected.begin(),expected.end(),out));
    zsg2_decode_block(d.d,UINT32_MAX,out);assert(std::equal(expected.begin(),expected.end(),out));
    zsg2_reset(d.d);zsg2_decode_block(d.d,0,out);assert(std::equal(expected.begin(),expected.end(),out));
    std::printf("%u independent compressed decode vectors passed\n",vectors);
}
static void bus_reset_vectors()
{
    Capture cap;Device d(64,&cap);cap.device=d.d;ZSG2 *bad=nullptr;ZSG2Config cfg{};
    assert(zsg2_create(&cfg,&bad)==ZSG2_ARGUMENT&&bad==nullptr);
    cfg.clock_hz=767;cfg.sample_rom=d.rom.data();cfg.sample_rom_size=d.rom.size();assert(zsg2_create(&cfg,&bad)==ZSG2_ARGUMENT);
    assert(d.channel().step==1);for(auto v:d.channel().registers)assert(v==0);
    d.write(0,0x3401);d.write(1,0xab12);auto c=d.channel();assert(c.start==0x1234&&c.page==0xab0000);
    d.write(5,0x8b56);d.write(7,0x1f78);assert(d.channel().loop==0x7856&&d.channel().output_gain[3]==0x8b);
    d.write(3,0xffff);assert(d.read(3)==0x7fff);d.write(0x300,1);assert(d.read(3)==0xffff);
    assert(d.channel().volume==0&&d.channel().volume_delta==0x400&&d.channel().current_position==0x1233);
    d.write(3,0x0456);assert(d.read(3)==0x8456);d.write(0x304,1);assert(d.read(3)==0x0456&&d.read(11)==0);
    d.write(0x301,1);d.write(0x302,0x8000);assert(d.channel(16).status&ZSG2_ACTIVE);assert(d.channel(47).status&ZSG2_ACTIVE);
    d.write(0x305,1);d.write(0x306,0x8000);assert(!(d.channel(16).status&ZSG2_ACTIVE)&&!(d.channel(47).status&ZSG2_ACTIVE));
    put_word(d.rom,3,0xcafe1234);d.write(0x31c,12);d.write(0x31d,0);assert(d.read(0x31e)==0x1234&&d.read(0x31f)==0xcafe);
    assert(d.read(0x31e)==0x1234&&d.status().read_address==3);
    d.write(0x31c,15);assert(d.status().read_address==3&&(d.status().diagnostics&ZSG2_DIAG_UNALIGNED_ROM_ADDRESS));
    d.write(0x31d,0xffff);assert(d.read(0x31e)==0&&d.read(0x31f)==0);
    d.write(0x308,0x5678);assert(d.read(0x308)==0x5678);d.write(0x314,0xaaaa);assert(d.read(0x314)==0);
    d.write(0x3ff,0xaaaa);assert(d.read(0x3ff)==0);
    d.write(0,0x3400);uint16_t value=0xffff;
    assert(zsg2_write(d.d,0,0x9900,0xff)==ZSG2_MASK_UNSUPPORTED&&d.channel().start==0x1234);
    assert(zsg2_read(d.d,0,0xff,&value)==ZSG2_MASK_UNSUPPORTED&&value==0);
    assert(cap.diagnostic_count==6);
}
/* Independent scalar equations. No production implementation helpers or state
 * mutations are reused: arithmetic shifts are mathematical floor division,
 * and signed wrapping uses modulo rather than C shifts/conversions. */
struct Reference {
    std::vector<uint32_t> rom;
    uint32_t cur=UINT32_MAX,ptr=0x10000,step=0x4000,start=0,end=16,loop=0;
    uint16_t volume=65535,target=65535,cutoff=65535,cutoff_target=65535;
    int16_t vdelta=0,cdelta=0;
    int32_t emph=0,filter=0;
    std::array<int16_t,5> samples{};
    std::array<uint8_t,4> gains{31,31,31,31};
    bool active=true;
    std::array<int16_t,4> frame(unsigned parity) {
        std::array<int16_t,4> out{};if(!active)return out;
        ptr+=step;
        if(ptr>=65536) {
            cur++;
            if(cur>=end) {cur=loop;if(cur+1>=end){volume=0;active=false;return out;}}
            if(cur==start)emph=0;
            ptr%=65536;samples[0]=samples[4];auto raw=decode_ref(rom[cur]);
            for(unsigned i=0;i<4;i++){emph+=raw[i]-floor_shift(int64_t(emph)+32,6);samples[i+1]=clip(floor_shift(emph,1));}
        }
        unsigned pos=ptr/16384;int32_t sample=samples[pos];
        int32_t diff=signed_width(int64_t(samples[pos+1])-sample,16);
        sample+=floor_shift(int64_t((ptr*4)%65536)*diff,16);
        filter=signed_width(int64_t(filter)+(int64_t(sample)-floor_shift(filter,16))*cutoff,32);
        sample=floor_shift(filter,16);if(!cutoff)filter=floor_shift(filter,1);
        sample=floor_shift(int64_t(sample)*volume,16);
        for(unsigned i=0;i<4;i++) {
            int32_t routed=(gains[i]&128)?-sample:sample;
            unsigned gain=(gains[i]&31)==31?65535:(gains[i]&31)==1?2072:0;
            out[i]=clip(floor_shift(int64_t(routed)*gain,16));
        }
        if(parity&1) {
            auto r=[](uint16_t now,uint16_t target,int16_t delta){int32_t val=int32_t(now)+delta;if((delta<0&&val<target)||(delta>=0&&val>target))val=target;return uint16_t(val);};
            volume=r(volume,target,vdelta);cutoff=r(cutoff,cutoff_target,cdelta);
        }
        return out;
    }
};
static void interpolation_filter_loop_vectors()
{
    for(uint16_t pitch:{uint16_t(0),uint16_t(0x3fff),uint16_t(0x9fff),uint16_t(0xffff)}) {
        Device d;Reference ref;ref.step=uint32_t(pitch)+1;
        for(unsigned i=0;i<16;i++){uint32_t word=encode({int(i)-8,31-int(i),int(i)*3-24,-9},i%4);put_word(d.rom,i,word);ref.rom.push_back(word);}
        configure(d,0,pitch);d.write(11,0xffff);d.write(15,0x1f00);
        std::vector<int16_t> out(512*4);assert(zsg2_render_update(d.d,512,out.data(),512)==ZSG2_OK);
        for(unsigned i=0;i<512;i++) {auto expected=ref.frame(0);for(unsigned j=0;j<4;j++)assert(out[i*4+j]==expected[j]);}
        auto c=d.channel();assert(c.current_position==ref.cur&&c.step_pointer==ref.ptr&&c.output_filter==ref.filter&&c.emphasis_filter==ref.emph);
        /* cutoff zero discharges after reading its retained filter state. */
        d.write(9,0);ref.cutoff=0;
        for(unsigned i=0;i<32;i++){int16_t actual[4];zsg2_render_update(d.d,1,actual,1);auto expected=ref.frame((1+i)&1);for(unsigned j=0;j<4;j++)assert(actual[j]==expected[j]);}
    }
    Device d;for(unsigned i=0;i<16;i++)put_word(d.rom,i,encode({10,10,10,10},0));
    configure(d,0,0xffff,2,3);d.write(11,65535);d.batch(4);assert(!(d.channel().status&ZSG2_ACTIVE)&&d.channel().volume==0);
    configure(d,0,0xffff,0,4);d.write(11,65535);d.batch(20);assert(d.channel().status&ZSG2_ACTIVE);
}
static int16_t ramp_ref(uint8_t value)
{
    int fraction=int(value&15);if(fraction>=8)fraction-=16;
    fraction=signed_width(uint32_t(fraction)^8u,32);
    return int16_t(floor_shift(signed_width(int64_t(fraction)*(int64_t(1)<<(value>>4)),16),4));
}
static void envelope_routing_vectors()
{
    Device d;for(unsigned i=0;i<16;i++)put_word(d.rom,i,encode({40,40,40,40},0));
    for(unsigned value=0;value<256;value++){d.write(13,uint16_t(value));d.write(15,uint16_t(value));auto c=d.channel();assert(c.cutoff_delta==ramp_ref(value)&&c.volume_delta==ramp_ref(value));}
    configure(d,0);d.write(14,4096);
    d.batch(4);assert(d.channel().volume==0);d.batch(1);assert(d.channel().volume==1024);
    d.batch(5);assert(d.channel().volume==1024);d.batch(2);assert(d.channel().volume==3072);
    d.batch(2);assert(d.channel().volume==3072);d.batch(2);assert(d.channel().volume==4096);
    /* Downward ramp clamps at target and the real current-volume register is readable. */
    d.write(15,0x1fb8);d.write(14,2048);d.batch(1);d.batch(1);assert(d.read(11)==2048);
    /* Four routes remain independent: no synthetic DSP/reverb/stereo mix. */
    configure(d,0);d.write(11,65535);d.write(5,0x9f00);d.write(7,0x0100);d.write(13,0x0000);d.write(15,0x1f00);
    Reference ref;for(unsigned i=0;i<16;i++)ref.rom.push_back(encode({40,40,40,40},0));ref.gains={31,0,1,159};
    std::vector<int16_t> out(40*4);zsg2_render_update(d.d,40,out.data(),40);
    for(unsigned i=0;i<40;i++){auto e=ref.frame(d.status().sample_count-1);for(unsigned j=0;j<4;j++)assert(out[i*4+j]==e[j]);assert(out[i*4+1]==0);}
    Device mix;for(unsigned i=0;i<16;i++)put_word(mix.rom,i,encode({63,63,63,63},0));
    for(unsigned ch=0;ch<48;ch++){configure(mix,ch);mix.write(ch*16+11,65535);}
    int16_t frames[40*4];mix.batch(0);zsg2_render_update(mix.d,40,frames,40);
    assert(std::find(std::begin(frames),std::end(frames),int16_t(32767))!=std::end(frames));
    for(unsigned ch=0;ch<48;ch++)mix.write(ch*16+5,0x9f00);
    zsg2_render_update(mix.d,40,frames,40);assert(std::find(std::begin(frames),std::end(frames),int16_t(-32768))!=std::end(frames));
}
static void clock_schedule_vectors()
{
    Capture a,b;Device da(64,&a),db(64,&b);a.device=da.d;b.device=db.d;
    for(unsigned i=0;i<16;i++){uint32_t word=encode({23,-17,9,33},i%5);put_word(da.rom,i,word);put_word(db.rom,i,word);}
    configure(da,0);configure(db,0);
    const uint64_t events[]={123456,500001,777777,1234567,25000000};uint64_t last=0;
    for(unsigned event=0;event<5;event++) {
        uint64_t target=events[event];a.bound=target;assert(zsg2_advance_to_cycle(da.d,target)==ZSG2_OK);
        for(uint64_t current=last;current<target;){uint64_t part=std::min<uint64_t>(target-current,(current%977)+1);b.bound=current+part;assert(zsg2_run_cycles(db.d,part)==ZSG2_OK);current+=part;}
        assert(a.frames==b.frames&&a.times==b.times);
        ZSG2Status sa=da.status(),sb=db.status();assert(sa.cycles==sb.cycles&&sa.sample_phase==sb.sample_phase&&sa.periodic_phase==sb.periodic_phase&&sa.sample_count==sb.sample_count);
        auto ca=da.channel(),cb=db.channel();assert(std::memcmp(&ca,&cb,sizeof(ca))==0);
        da.write(14,uint16_t(0x7000+event));db.write(14,uint16_t(0x7000+event));last=target;
    }
    assert(a.frames.size()==32552&&da.status().sample_phase==0&&a.times.back()==25000000);
    assert(zsg2_advance_to_cycle(da.d,0)==ZSG2_TIME_REWIND);
    assert(zsg2_run_cycles(da.d,UINT64_MAX)==ZSG2_TIME_OVERFLOW);
    /* Partial bus masks do not become source batch barriers. */
    Device dc;assert(zsg2_run_cycles(dc.d,1000)==ZSG2_OK);auto old=dc.status();assert(old.pending_batch);
    uint16_t ignored;assert(zsg2_read(dc.d,0,0xff,&ignored)==ZSG2_MASK_UNSUPPORTED);assert(dc.status().sample_count==old.sample_count&&dc.status().pending_batch);
    assert(dc.read(0)==0&&dc.status().sample_count==old.sample_count+1);
    /* A zero-frame explicit callback increments source batch counter literally. */
    old=dc.status();dc.batch(0);assert(dc.status().sample_count==old.sample_count+1);
    int16_t short_buffer[4]; assert(zsg2_render_update(dc.d,2,short_buffer,1)==ZSG2_CAPACITY);
    assert(dc.status().sample_count==old.sample_count+1);
}
struct ExternalRom {
    std::array<uint32_t,16> words{};unsigned calls=0,bank=0; ZSG2 *device=nullptr;
    static uint32_t read(void *opaque,uint32_t word){auto &r=*static_cast<ExternalRom *>(opaque);assert(word<r.words.size());r.calls++;if(r.device)reject_reentry(r.device);return r.words[word]^r.bank;}
};
static void external_and_selective_reset_vectors()
{
    ExternalRom rom;rom.words[0]=encode({2,4,6,8},0);ZSG2Config cfg{};cfg.clock_hz=ZSG2_DEFAULT_CLOCK;cfg.sample_rom_size=64;cfg.external_read=ExternalRom::read;cfg.rom_opaque=&rom;
    ZSG2 *d=nullptr;assert(zsg2_create(&cfg,&d)==ZSG2_OK);rom.device=d;int16_t out[4];
    zsg2_decode_block(d,0,out);assert(rom.calls==1);zsg2_decode_block(d,0,out);assert(rom.calls==2);
    zsg2_decode_block(d,16,out);assert(rom.calls==2);rom.bank=0x11111111;zsg2_decode_block(d,0,out);auto expected=decode_ref(rom.words[0]^rom.bank);assert(std::equal(expected.begin(),expected.end(),out));
    zsg2_write(d,0x308,0x1234,0xffff);zsg2_write(d,0x31c,12,0xffff);
    zsg2_write(d,6,10,0xffff);zsg2_write(d,8,65535,0xffff);zsg2_write(d,4,0x3fff,0xffff);zsg2_write(d,0x300,1,0xffff);zsg2_write(d,11,65535,0xffff);
    zsg2_run_cycles(d,12345);ZSG2Channel before{};zsg2_get_channel(d,0,&before);ZSG2Status old{};zsg2_get_status(d,&old);
    zsg2_reset(d);ZSG2Channel after{};zsg2_get_channel(d,0,&after);ZSG2Status now{};zsg2_get_status(d,&now);
    assert(now.cycles==old.cycles&&now.sample_phase==old.sample_phase&&now.periodic_phase==old.periodic_phase&&now.frames==old.frames&&now.sample_count==0&&now.read_address==0);
    assert(after.step==1&&after.status==0&&after.volume==0&&after.cutoff==0&&after.initial_volume==0);
    assert(after.current_position==before.current_position&&after.step_pointer==before.step_pointer&&after.emphasis_filter==before.emphasis_filter&&after.output_filter==before.output_filter);
    assert(std::memcmp(after.samples,before.samples,sizeof(after.samples))==0);for(auto v:after.registers)assert(!v);
    uint16_t value;zsg2_read(d,0x308,0xffff,&value);assert(value==0x1234);
    zsg2_destroy(d);
}
int main()
{
    decode_vectors();bus_reset_vectors();interpolation_filter_loop_vectors();envelope_routing_vectors();clock_schedule_vectors();external_and_selective_reset_vectors();
    puts("ZSG-2 complete source device vectors and scheduling tests passed");
}
