#include "zn_mcu.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    uint8_t analog[8];
    uint16_t trackball[4];
    uint8_t dsw;
    unsigned analog_reads[8];
    unsigned trackball_reads[4];
    unsigned dsw_reads;
    int dsr;
    int txd;
    unsigned dsr_low_count;
    unsigned dsr_high_count;
    unsigned txd_count;
} Fixture;

#define MAKE_ANALOG(N) \
    static uint8_t analog##N(void *opaque) { \
        Fixture *f = (Fixture *)opaque; f->analog_reads[N]++; return f->analog[N]; \
    }
#define MAKE_TRACKBALL(N) \
    static uint16_t trackball##N(void *opaque) { \
        Fixture *f = (Fixture *)opaque; f->trackball_reads[N]++; return f->trackball[N]; \
    }
MAKE_ANALOG(0) MAKE_ANALOG(1) MAKE_ANALOG(2) MAKE_ANALOG(3)
MAKE_ANALOG(4) MAKE_ANALOG(5) MAKE_ANALOG(6) MAKE_ANALOG(7)
MAKE_TRACKBALL(0) MAKE_TRACKBALL(1) MAKE_TRACKBALL(2) MAKE_TRACKBALL(3)

static uint8_t dsw(void *opaque)
{
    Fixture *f = (Fixture *)opaque;
    f->dsw_reads++;
    return f->dsw;
}

static void dsr(void *opaque, int state)
{
    Fixture *f = (Fixture *)opaque;
    f->dsr = state;
    if (state) f->dsr_high_count++;
    else f->dsr_low_count++;
}

static void txd(void *opaque, int state)
{
    Fixture *f = (Fixture *)opaque;
    f->txd = state;
    f->txd_count++;
}

static ZNMCUConfig make_config(Fixture *fixture)
{
    ZNMCUConfig config;
    memset(&config, 0, sizeof(config));
    config.analog[0] = analog0; config.analog[1] = analog1;
    config.analog[2] = analog2; config.analog[3] = analog3;
    config.analog[4] = analog4; config.analog[5] = analog5;
    config.analog[6] = analog6; config.analog[7] = analog7;
    config.trackball[0] = trackball0; config.trackball[1] = trackball1;
    config.trackball[2] = trackball2; config.trackball[3] = trackball3;
    config.dsw = dsw;
    config.dsr = dsr;
    config.txd = txd;
    config.opaque = fixture;
    return config;
}

static void clock_byte(ZNMCU *mcu, Fixture *fixture, uint8_t expected)
{
    for (unsigned bit = 0; bit < 8; bit++) {
        assert(zn_mcu_sck(mcu, 0) == ZN_MCU_OK);
        if (fixture->txd != ((expected >> bit) & 1))
            fprintf(stderr, "TXD mismatch byte=%02x bit=%u got=%d\n", expected, bit, fixture->txd);
        assert(fixture->txd == ((expected >> bit) & 1));
        assert(zn_mcu_sck(mcu, 1) == ZN_MCU_OK);
    }
}

static void prepare_packet(ZNMCU *mcu, Fixture *fixture, uint64_t *now)
{
    assert(zn_mcu_select(mcu, 0) == ZN_MCU_OK);
    *now += 50;
    assert(zn_mcu_advance_to_us(mcu, *now) == ZN_MCU_OK);
    assert(fixture->dsr == 0);
    *now += 5;
    assert(zn_mcu_advance_to_us(mcu, *now) == ZN_MCU_OK);
    assert(fixture->dsr == 1);
}

static void clock_next_packet_byte(ZNMCU *mcu, Fixture *fixture,
                                   uint64_t *now, uint8_t expected)
{
    *now += 50;
    assert(zn_mcu_advance_to_us(mcu, *now) == ZN_MCU_OK);
    assert(fixture->dsr == 0);
    *now += 5;
    assert(zn_mcu_advance_to_us(mcu, *now) == ZN_MCU_OK);
    assert(fixture->dsr == 1);
    clock_byte(mcu, fixture, expected);
}

static void test_digital_packet_and_timer_edges(void)
{
    Fixture f;
    ZNMCUConfig config;
    ZNMCU *mcu = NULL;
    uint64_t now = 0;
    memset(&f, 0, sizeof(f));
    f.dsw = 0xa5;
    config = make_config(&f);
    assert(zn_mcu_create(&config, &mcu) == ZN_MCU_OK && mcu);
    assert(f.dsr == 1 && f.txd == 1); /* device_start output levels */
    assert(zn_mcu_select(mcu, 0) == ZN_MCU_OK);
    assert(zn_mcu_advance_to_us(mcu, 49) == ZN_MCU_OK);
    assert(f.dsr == 1 && f.dsw_reads == 0);
    assert(zn_mcu_advance_to_us(mcu, 50) == ZN_MCU_OK);
    assert(f.dsr == 0 && f.dsw_reads == 1);
    assert(zn_mcu_advance_to_us(mcu, 54) == ZN_MCU_OK && f.dsr == 0);
    assert(zn_mcu_advance_to_us(mcu, 55) == ZN_MCU_OK && f.dsr == 1);
    now = 55;
    clock_byte(mcu, &f, 0x15); /* (one data byte << 4) | DSW low nibble */
    clock_next_packet_byte(mcu, &f, &now, 0x00); /* zero-filled unpopulated send byte */
    assert(f.dsw_reads == 1); /* inter-byte pulses do not resample the header */
    assert(zn_mcu_advance_to_us(mcu, now + 100) == ZN_MCU_OK);
    assert(f.dsr_low_count == 2);
    assert(zn_mcu_advance_to_us(mcu, now) == ZN_MCU_TIME_REWIND);
    assert(zn_mcu_select(mcu, 1) == ZN_MCU_OK);
    assert(f.dsr == 1 && f.txd == 1);
    zn_mcu_destroy(mcu);

    /* SELECT high cancels a scheduled 50 us inter-byte pulse. */
    memset(&f, 0, sizeof(f));
    f.dsw = 0x0f;
    config = make_config(&f);
    assert(zn_mcu_create(&config, &mcu) == ZN_MCU_OK);
    now = 0;
    prepare_packet(mcu, &f, &now);
    clock_byte(mcu, &f, 0x1f);
    assert(zn_mcu_select(mcu, 1) == ZN_MCU_OK);
    assert(zn_mcu_advance_to_us(mcu, now + 100) == ZN_MCU_OK);
    assert(f.dsr_low_count == 1);
    zn_mcu_destroy(mcu);
}

static void test_analog_packet_and_priority(void)
{
    Fixture f;
    ZNMCUConfig config;
    ZNMCU *mcu = NULL;
    uint64_t now = 0;
    memset(&f, 0, sizeof(f));
    for (unsigned i = 0; i < 8; i++)
        f.analog[i] = (uint8_t)(0x10 + i * 0x11);
    f.dsw = 0xf5;
    config = make_config(&f);
    assert(zn_mcu_create(&config, &mcu) == ZN_MCU_OK);
    zn_mcu_analog_read(mcu, 1);
    zn_mcu_trackball_read(mcu, 1); /* analog mode takes priority in MAME */
    prepare_packet(mcu, &f, &now);
    assert(f.dsw_reads == 1);
    for (unsigned i = 0; i < 8; i++)
        assert(f.analog_reads[i] == 1);
    for (unsigned i = 0; i < 4; i++)
        assert(f.trackball_reads[i] == 0);
    clock_byte(mcu, &f, 0x85);
    for (unsigned i = 0; i < 8; i++)
        clock_next_packet_byte(mcu, &f, &now, f.analog[i]);
    assert(f.dsw_reads == 1);
    assert(zn_mcu_select(mcu, 1) == ZN_MCU_OK);
    zn_mcu_destroy(mcu);
}

static void test_trackball_delta_wrap_and_history(void)
{
    Fixture f;
    ZNMCUConfig config;
    ZNMCU *mcu = NULL;
    uint64_t now = 0;
    static const uint8_t first[6] = {0x01, 0x10, 0x0f, 0xf0, 0xff, 0xff};
    static const uint8_t second[6] = {0xff, 0xff, 0xf1, 0xf0, 0x0f, 0x03};
    memset(&f, 0, sizeof(f));
    f.dsw = 0x0a;
    f.trackball[0] = 0x0001;
    f.trackball[1] = 0x010f;
    f.trackball[2] = 0x0ff0;
    f.trackball[3] = 0x0fff;
    config = make_config(&f);
    assert(zn_mcu_create(&config, &mcu) == ZN_MCU_OK);
    zn_mcu_trackball_read(mcu, 1);
    prepare_packet(mcu, &f, &now);
    clock_byte(mcu, &f, 0x6a);
    for (unsigned i = 0; i < 6; i++)
        clock_next_packet_byte(mcu, &f, &now, first[i]);
    for (unsigned i = 0; i < 4; i++)
        assert(f.trackball_reads[i] == 1);
    {
        unsigned low_pulses = f.dsr_low_count;
        assert(zn_mcu_advance_to_us(mcu, now + 100) == ZN_MCU_OK);
        assert(f.dsr_low_count == low_pulses); /* no byte after the six data bytes */
        now += 100;
    }

    /* New hardware counts produce unsigned 16-bit wrapped movement deltas. */
    f.trackball[0] = 0x0000;
    f.trackball[1] = 0x0100;
    f.trackball[2] = 0x0fe0;
    f.trackball[3] = 0x0002;
    assert(zn_mcu_select(mcu, 1) == ZN_MCU_OK);
    assert(zn_mcu_select(mcu, 0) == ZN_MCU_OK);
    now += 50;
    assert(zn_mcu_advance_to_us(mcu, now) == ZN_MCU_OK && f.dsr == 0);
    now += 5;
    assert(zn_mcu_advance_to_us(mcu, now) == ZN_MCU_OK && f.dsr == 1);
    clock_byte(mcu, &f, 0x6a);
    for (unsigned i = 0; i < 6; i++)
        clock_next_packet_byte(mcu, &f, &now, second[i]);
    for (unsigned i = 0; i < 4; i++)
        assert(f.trackball_reads[i] == 2);
    assert(zn_mcu_select(mcu, 1) == ZN_MCU_OK);
    zn_mcu_destroy(mcu);
}

static void test_mame_default_callbacks(void)
{
    ZNMCU *mcu = NULL;
    Fixture f;
    ZNMCUConfig config;
    uint64_t now = 0;
    memset(&f, 0, sizeof(f));
    memset(&config, 0, sizeof(config));
    config.dsr = dsr;
    config.txd = txd;
    config.opaque = &f;
    assert(zn_mcu_create(&config, &mcu) == ZN_MCU_OK);
    zn_mcu_analog_read(mcu, 1);
    prepare_packet(mcu, &f, &now);
    clock_byte(mcu, &f, 0x8f); /* MAME defaults: DSW ff and analog ff */
    clock_next_packet_byte(mcu, &f, &now, 0xff);
    assert(zn_mcu_sck(NULL, 0) == ZN_MCU_ARGUMENT);
    zn_mcu_destroy(mcu);

    memset(&f, 0, sizeof(f));
    assert(zn_mcu_create(&config, &mcu) == ZN_MCU_OK);
    zn_mcu_trackball_read(mcu, 1);
    now = 0;
    prepare_packet(mcu, &f, &now);
    clock_byte(mcu, &f, 0x6f); /* default DSW ff */
    for (unsigned i = 0; i < 6; i++)
        clock_next_packet_byte(mcu, &f, &now, 0); /* default counters are zero */
    assert(zn_mcu_select(mcu, 1) == ZN_MCU_OK);
    zn_mcu_destroy(mcu);
}

static void test_clock_range_errors(void)
{
    Fixture f;
    ZNMCUConfig config;
    ZNMCU *mcu = NULL;
    memset(&f, 0, sizeof(f));
    config = make_config(&f);
    assert(zn_mcu_create(&config, &mcu) == ZN_MCU_OK);
    assert(zn_mcu_advance_to_us(mcu, UINT64_MAX - 50) == ZN_MCU_OK);
    assert(zn_mcu_select(mcu, 0) == ZN_MCU_OK);
    assert(zn_mcu_advance_to_us(mcu, UINT64_MAX) == ZN_MCU_TIME_OVERFLOW);
    assert(f.dsr == 0); /* expiry ran, but +5 us is beyond representable time */
    zn_mcu_destroy(mcu);
}

int main(void)
{
    test_digital_packet_and_timer_edges();
    test_analog_packet_and_priority();
    test_trackball_delta_wrap_and_history();
    test_mame_default_callbacks();
    test_clock_range_errors();
    puts("Sony ZN MCU digital/analog/trackball packets, serial pins, and virtual timers PASS");
    return 0;
}
