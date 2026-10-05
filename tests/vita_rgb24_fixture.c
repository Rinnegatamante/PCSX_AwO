#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../plugins/gpu_vita_ctr/vita_rgb24.h"

static uint16_t vram[1024 * 1024];

int main(void)
{
    uint8_t rgba[4];
    vram[0] = 0x2211;
    vram[1] = 0x0033;
    vram[512 * 1024] = 0x5544;
    vram[512 * 1024 + 1] = 0x0066;
    vram[1023 * 1024] = 0x8877;
    vram[1023 * 1024 + 1] = 0x0099;
    psx_vita_rgb24_row(vram, 0, 512, 0, 1, 1024, rgba);
    assert(rgba[0] == 0x44 && rgba[1] == 0x55 && rgba[2] == 0x66 && rgba[3] == 255);
    psx_vita_rgb24_row(vram, 0, 1023, 0, 1, 1024, rgba);
    assert(rgba[0] == 0x77 && rgba[1] == 0x88 && rgba[2] == 0x99 && rgba[3] == 255);
    psx_vita_rgb24_row(vram, 0, 1023, 1, 1, 1024, rgba);
    assert(rgba[0] == 0x11 && rgba[1] == 0x22 && rgba[2] == 0x33 && rgba[3] == 255);
    psx_vita_rgb24_row(vram, 0, 512, 0, 1, 512, rgba);
    assert(rgba[0] == 0x11 && rgba[1] == 0x22 && rgba[2] == 0x33 && rgba[3] == 255);
    puts("Vita RGB24 upper row and 512/1024 vertical wrap: PASS");
    return 0;
}
