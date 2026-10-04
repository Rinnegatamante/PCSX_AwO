/* Independent bus transactions and fixed MAME KEYCUS vectors. */
#include "../libpcsxcore/arcade/namcos11_board.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t *main_rom, *bank_rom, *data_rom;
static uint8_t bios[0x4000], wave[64];
static unsigned audio_frames, audio_peak, output_leds, output_recoil;
static uint16_t inputs(void *p, unsigned input) { (void)p; return 0x10 + input; }
static uint16_t guns(void *p, unsigned channel) { (void)p; return 100 + channel * 200; }
static uint16_t rng(void *p) { (void)p; return 0xbeef; }
static void outputs(void *p, unsigned leds, unsigned recoil)
{ (void)p; output_leds = leds; output_recoil = recoil; }
static void audio(void *p, const int16_t *samples, size_t frames)
{
    size_t i; (void)p;
    audio_frames += (unsigned)frames;
    for (i = 0; i < frames * 4; i++) {
        unsigned peak = samples[i] < 0 ? -(int)samples[i] : samples[i];
        if (peak > audio_peak) audio_peak = peak;
    }
}
static Namcos11Config config(unsigned chip, unsigned banking)
{
    Namcos11Config c;
    memset(&c, 0, sizeof(c));
    c.model = NAMCOS11_COH110; c.banking = banking; c.keycus = chip;
    c.main_rom = main_rom; c.main_size = 0x400000;
    c.bank_rom = bank_rom; c.bank_size = 0x4000000;
    c.c76_data = data_rom; c.c76_size = 0x80000;
    c.c76_bios = bios; c.c76_bios_size = sizeof(bios);
    c.wave_rom = wave; c.wave_size = sizeof(wave);
    c.input = inputs; c.gun = guns; c.gun_outputs = outputs;
    c.random16 = rng; c.audio = audio;
    return c;
}
static uint32_t read_bus(Namcos11Board *b, uint32_t a, unsigned width)
{ uint32_t v; assert(namcos11_read(b, a, width, &v) == 1); return v; }
static void write_bus(Namcos11Board *b, uint32_t a, unsigned width, uint32_t v)
{ assert(namcos11_write(b, a, width, v) == 1); }
static void kw(Namcos11Board *b, unsigned offset, uint16_t value)
{ write_bus(b, 0x1fa20000 + offset * 2, 2, value); }
static uint16_t kr(Namcos11Board *b, unsigned offset)
{ return (uint16_t)read_bus(b, 0x1fa20000 + offset * 2, 2); }
static void key_vectors(void)
{
    unsigned chips[] = {406,409,410,411,430,431,432,442,443}, i;
    for (i = 0; i < sizeof(chips) / sizeof(chips[0]); i++) {
        Namcos11Config c = config(chips[i], NAMCOS11_BANK_ROM8);
        Namcos11Board *b = namcos11_create(&c);
        Namcos11Status s;
        assert(b);
        switch (chips[i]) {
        case 406: kw(b,1,0x1234);kw(b,2,0x5678);kw(b,3,15);assert(kr(b,0)==0x3256);break;
        case 409:
            kw(b,3,0x001f);kw(b,7,0x7c00);
            kw(b,1,1);assert(kr(b,7)==0x7c00);
            kw(b,1,32);assert(kr(b,7)==0x001f);
            kw(b,1,16);assert(kr(b,7)==0x400f);break;
        case 410:
            kw(b,0,0xfffe);kw(b,2,0);assert(kr(b,1)==0);assert(kr(b,2)==4);assert(kr(b,3)==0x0100);
            kw(b,0,12345);assert(kr(b,1)==5);assert(kr(b,2)==0x0203);assert(kr(b,3)==0x0401);break;
        case 411:
            kw(b,2,0);kw(b,8,0);kw(b,10,0xff7f);assert(kr(b,0)==0x0101);assert(kr(b,2)==4);assert(kr(b,8)==0);
            kw(b,2,0x7256);kw(b,10,54321);assert(kr(b,0)==0x0201);assert(kr(b,2)==0x0403);assert(kr(b,8)==5);break;
        case 430:
            kw(b,0,0xbfff);kw(b,1,0);kw(b,4,0);assert(kr(b,1)==0);assert(kr(b,4)==4);assert(kr(b,5)==0x0300);
            kw(b,0,54321);kw(b,4,0xe296);assert(kr(b,1)==5);assert(kr(b,4)==0x0403);assert(kr(b,5)==0x0201);break;
        case 431:
            kw(b,0,0);kw(b,4,0);kw(b,12,0x7fff);assert(kr(b,0)==0x0301);assert(kr(b,4)==4);assert(kr(b,8)==0);break;
        case 432:
            kw(b,0,0);kw(b,2,0xefff);kw(b,4,0);assert(kr(b,2)==0x0302);assert(kr(b,4)==4);assert(kr(b,6)==0);break;
        case 442:
            kw(b,0,0x20);kw(b,1,0x21);assert(kr(b,0)==0);assert(kr(b,1)==0xc442);
            kw(b,1,0x3af2);assert(kr(b,1)==0);break;
        case 443:
            kw(b,0,0x20);kw(b,1,0xffff);assert(kr(b,0)==0x20);assert(kr(b,1)==0xc443);
            kw(b,1,0xffdf);assert(kr(b,1)==0);break;
        }
        assert(kr(b,15)==0xbeef); /* Explicit MAME random fallback. */
        namcos11_get_status(b,&s);assert(s.unknown_keycus_reads==1 && !s.fault);
        namcos11_reset(b);assert(kr(b,15)==0xbeef);
        namcos11_destroy(b);
    }
}
static void buses(void)
{
    Namcos11Config c = config(409, NAMCOS11_BANK_ROM8);
    Namcos11Board *b = namcos11_create(&c);
    M37710 *cpu;
    uint32_t value;
    uint8_t eeprom[AT28C16_IMAGE_BYTES];
    unsigned i;
    assert(b);cpu=namcos11_c76(b);
    assert(namcos11_gpu_model(b)==2);
    assert(read_bus(b,0x1fc00000,4)==0x44332211);
    assert(namcos11_read(b,0x1f801810,4,&value)==0); /* GPU belongs to host. */
    assert(namcos11_read(b,0x1fa04001,2,&value)==-1);
    assert(namcos11_read(b,0x1fa04000,3,&value)==-1);
    write_bus(b,0x1fa04000,4,0x1234abcd);
    assert(m37710_read16(cpu,0x4000)==0xabcd && m37710_read16(cpu,0x4002)==0x1234);
    m37710_write16(cpu,0xbffe,0x5678);assert(read_bus(b,0x1fa0bffe,2)==0x5678);
    write_bus(b,0x1fa04001,1,0xef);assert(m37710_read16(cpu,0x4000)==0xefcd);
    m37710_write8(cpu,0x4002,0x99);assert(read_bus(b,0x1fa04000,4)==0x1299efcd);
    for(i=0;i<4;i++)assert(m37710_read16(cpu,0x1000+i*2)==0x10+i);
    assert(m37710_read16(cpu,0x080000)==0x2211);
    assert(m37710_read16(cpu,0x200000)==0x2211);
    assert(m37710_read16(cpu,0x280000)==0x2211);
    assert(m37710_read16(cpu,0x510000)==0x8080);
    assert(read_bus(b,0x1fa30000,4)==0xffffffff);
    write_bus(b,0x1fa30000,4,0x00120034);assert(read_bus(b,0x1fa30000,4)==0xffb4ffb4);
    namcos11_reset(b);assert(read_bus(b,0x1fa30000,4)==0xffb4ffb4);
    namcos11_run_cycles(b,1700); /* 200 us deadline completes; second lane was busy. */
    assert(read_bus(b,0x1fa30000,4)==0xffffff34);
    write_bus(b,0x1fa30001,1,0x77);assert(read_bus(b,0x1fa30000,2)==0xff34);
    assert(namcos11_eeprom_export(b,eeprom,sizeof(eeprom))==0 && eeprom[0]==0x34 && eeprom[1]==0xff);
    assert(namcos11_eeprom_import(b,eeprom,2047)==-1);
    memset(eeprom,0x56,sizeof(eeprom));assert(namcos11_eeprom_import(b,eeprom,sizeof(eeprom))==0);
    namcos11_reset(b);assert(read_bus(b,0x1fa30000,4)==0xff56ff56);
    for(i=0;i<8;i++) {
        write_bus(b,0x1fa10020+i*2,2,0xc0|i%4);
        assert(read_bus(b,0x1f000000+i*0x100000,1)==12+i%4);
    }
    assert(namcos11_read(b,0x1fa0c000,2,&value)==-1); /* Declared aperture exceeds real share. */
    namcos11_destroy(b);
    c.model=NAMCOS11_COH100;c.banking=NAMCOS11_BANK_ROM8_64;c.lightgun=1;
    b=namcos11_create(&c);assert(b && namcos11_gpu_model(b)==1);
    write_bus(b,0x1f080002,2,0);write_bus(b,0x1fa10020,2,0xc7);
    assert(read_bus(b,0x1f000000,1)==15); /* (24+7) XOR16. */
    write_bus(b,0x1f080000,4,0);write_bus(b,0x1fa10022,2,0x40);
    assert(read_bus(b,0x1f100000,1)==24); /* (8+0) XOR16. */
    assert(read_bus(b,0x1f780000,4)==100);
    assert(read_bus(b,0x1f780004,4)==(301u<<16|300));
    assert(read_bus(b,0x1f78000c,4)==(701u<<16|700));
    write_bus(b,0x1f788000,2,0x0a);assert(output_leds==2 && output_recoil==2);
    namcos11_destroy(b);
}
static void sound_and_scheduler(void)
{
    Namcos11Config c = config(406,NAMCOS11_BANK_NONE);
    Namcos11Board *b=namcos11_create(&c);
    M37710 *cpu=namcos11_c76(b);
    Namcos11Status s;M37710Status cs;
    assert(b);audio_frames=audio_peak=0;
    m37710_write16(cpu,0x2000,0xffff);m37710_write16(cpu,0x2002,0);
    m37710_write16(cpu,0x2004,0xffff);m37710_write16(cpu,0x2006,0x4002);
    m37710_write16(cpu,0x2008,0);m37710_write16(cpu,0x200a,0);
    m37710_write16(cpu,0x200c,63);m37710_write16(cpu,0x200e,0);
    m37710_write8(cpu,0x2404,0);m37710_write8(cpu,0x2405,0);
    assert(m37710_read16(cpu,0x2006)==0x4002); /* Byte writes cannot execute C352 key. */
    m37710_write16(cpu,0x2404,0);assert(m37710_read16(cpu,0x2006)&0x8000);
    namcos11_run_cycles(b,0);
    namcos11_get_status(b,&s);
    assert(s.cycles==0 && s.irq0_ticks==1 && s.irq2_ticks==1);
    namcos11_run_cycles(b,141119);
    namcos11_get_status(b,&s);
    assert(s.irq0_ticks==1 && s.irq2_ticks==1);
    namcos11_run_cycles(b,1);
    namcos11_get_status(b,&s);m37710_get_status(cpu,&cs);
    assert(!s.fault && !cs.fault && !cs.illegal_opcodes && !cs.unsupported_accesses);
    assert(s.cycles==141120 && s.samples==1470 && audio_frames==1470 && audio_peak>0);
    assert(s.irq0_ticks==2 && s.irq2_ticks==2); /* t=0 and first period. */
    assert((cs.pending_irqs&(1u<<M37710_LINE_IRQ0)) && (cs.pending_irqs&(1u<<M37710_LINE_IRQ2)));
    assert(read_bus(b,0x1fa04000,2)==0xdead); /* Real C76 instructions wrote this. */
    m37710_set_register(cpu,M37710_S,0x27f);
    m37710_set_register(cpu,M37710_PS,0); /* IRQ enable, IPL zero. */
    m37710_write8(cpu,0x7d,2);m37710_write8(cpu,0x7f,3);
    namcos11_run_cycles(b,96);
    m37710_get_status(cpu,&cs);
    assert(cs.interrupts==2 && !(cs.pending_irqs&((1u<<M37710_LINE_IRQ0)|(1u<<M37710_LINE_IRQ2))));
    assert(read_bus(b,0x1fa04002,2)==0xbeef);
    namcos11_destroy(b);
}
int main(void)
{
    Namcos11Config c;unsigned i;
    main_rom=calloc(1,0x400000);bank_rom=malloc(0x4000000);data_rom=calloc(1,0x80000);
    assert(main_rom && bank_rom && data_rom);
    for(i=0;i<64;i++)memset(bank_rom+i*0x100000,i,0x100000);
    main_rom[0]=data_rom[0]=0x11;main_rom[1]=data_rom[1]=0x22;
    main_rom[2]=0x33;main_rom[3]=0x44;
    { const uint8_t program[]={0xa9,0xad,0xde,0x8d,0,0x40,0x80,0xfe};
      const uint8_t handler[]={0xa9,0xef,0xbe,0x8d,2,0x40,0x40};
      memcpy(bios,program,sizeof(program));memcpy(bios+0x10,handler,sizeof(handler)); }
    bios[0x3ff4]=bios[0x3ff0]=0x10;bios[0x3ff5]=bios[0x3ff1]=0xc0;
    bios[0x3ffe]=0;bios[0x3fff]=0xc0;
    memset(wave,0x40,sizeof(wave));
    c=config(999,NAMCOS11_BANK_ROM8);assert(!namcos11_create(&c));
    c=config(406,NAMCOS11_BANK_ROM8);c.c76_bios_size=0;assert(!namcos11_create(&c));
    c=config(406,NAMCOS11_BANK_ROM8);c.main_size--;assert(!namcos11_create(&c));
    key_vectors();buses();sound_and_scheduler();
    free(main_rom);free(bank_rom);free(data_rom);
    puts("System11: nine KEYCUS vectors, ROM8/64 banks, shared LE bus, C76/C352 atomic key, IRQ/audio scheduler: PASS");
    return 0;
}
