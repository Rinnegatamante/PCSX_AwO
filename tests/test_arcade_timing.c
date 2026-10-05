#include "psxcommon.h"

#include <math.h>
#include <stdio.h>

static int active_taito, active_gnet, active_gv, active_zn;
static int active_system10, active_system11, active_system12;
static u32 gnet_clock, zn_clock;

int systemtaito_active(void) { return active_taito; }
int systemzn_active(void) { return active_zn; }
unsigned int systemzn_main_clock_hz(void) { return zn_clock; }
int system10_active(void) { return active_system10; }
int system11_active(void) { return active_system11; }
int system12_active(void) { return active_system12; }
int systemgnet_active(void) { return active_gnet; }
uint32_t systemgnet_main_clock_hz(void) { return gnet_clock; }
int systemgv_active(void) { return active_gv; }

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "timing test failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int close_to(double value, double expected)
{
    return fabs(value - expected) < 0.0000001;
}

int main(void)
{
    Config.cycle_multiplier = 175;

    CHECK(close_to(psxGpuSourceRefreshHz(0), 59.8260978565));
    CHECK(close_to(psxGpuSourceRefreshHz(1u << 19), 59.9400523286));
    CHECK(close_to(psxGpuSourceRefreshHz(1u << 20), 50.0));
    CHECK(close_to(psxGpuSourceRefreshHz((1u << 20) | (1u << 19)), 50.0));

#if defined(PCSX_ARCADE_SOURCE_TIMING)
    active_gnet = 1;
    gnet_clock = 67737600u;
    psxTimingRefresh();
    CHECK(psxArcadeSourceTimingActive());
    CHECK(psxArcadeSourceInputClockHz() == 67737600u);
    CHECK(PSXCLK == 33868800u && Config.cycle_multiplier == 200);

    active_gnet = 0;
    active_zn = 1;
    zn_clock = 100000000u;
    psxTimingRefresh();
    CHECK(psxArcadeSourceInputClockHz() == 100000000u);
    CHECK(PSXCLK == 50000000u && Config.cycle_multiplier == 200);

    active_zn = 0;
    active_taito = 1;
    psxTimingRefresh();
    CHECK(psxArcadeSourceInputClockHz() == 67737600u);

    active_taito = 0;
    active_gv = 1;
    psxTimingRefresh();
    CHECK(psxArcadeSourceInputClockHz() == 67737600u);

    active_gv = 0;
    active_system11 = 1;
    psxTimingRefresh();
    CHECK(psxArcadeSourceInputClockHz() == 67737600u);

    active_system11 = 0;
    active_system10 = 1;
    psxTimingRefresh();
    CHECK(psxArcadeSourceInputClockHz() == 101491200u);
    CHECK(PSXCLK == 50745600u);

    active_system10 = 0;
#if defined(PCSX_SYSTEM12_SOURCE_TIMING)
    active_system12 = 1;
    psxTimingRefresh();
    CHECK(psxArcadeSourceInputClockHz() == 100000000u);
    CHECK(PSXCLK == 50000000u && Config.cycle_multiplier == 200);
    active_system12 = 0;
#else
    active_system12 = 1;
    psxTimingRefresh();
    CHECK(!psxArcadeSourceTimingActive());
    CHECK(PSXCLK == 33868800u && Config.cycle_multiplier == 175);
    active_system12 = 0;
#endif
#elif defined(PCSX_SYSTEM12_SOURCE_TIMING)
    active_system12 = 1;
    psxTimingRefresh();
    CHECK(psxArcadeSourceTimingActive());
    CHECK(psxArcadeSourceInputClockHz() == 100000000u);
    CHECK(PSXCLK == 50000000u && Config.cycle_multiplier == 200);
    active_system12 = 0;
#else
    active_gnet = 1;
    gnet_clock = 100000000u;
    active_system12 = 1;
    psxTimingRefresh();
    CHECK(!psxArcadeSourceTimingActive());
    CHECK(psxArcadeSourceInputClockHz() == 0);
    CHECK(PSXCLK == 33868800u && Config.cycle_multiplier == 175);
    active_gnet = active_system12 = 0;
#endif

    psxTimingRefresh();
    CHECK(!psxArcadeSourceTimingActive());
    CHECK(psxArcadeSourceInputClockHz() == 0);
    CHECK(PSXCLK == 33868800u && Config.cycle_multiplier == 175);
    puts("arcade timing tests passed");
    return 0;
}
