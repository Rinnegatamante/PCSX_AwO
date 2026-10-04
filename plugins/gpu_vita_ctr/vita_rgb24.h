#ifndef PSX_VITA_RGB24_H
#define PSX_VITA_RGB24_H

#include <stdint.h>

static inline void psx_vita_rgb24_row(const uint16_t *vram, uint32_t display_x,
                                    uint32_t display_y, uint32_t row,
                                    uint32_t width, uint8_t *rgba) {
    const uint32_t byte_x = (display_x & 1023u) * 2u;
    const uint32_t physical_y = (display_y + row) & 511u;
    const uint32_t available = (2048u - byte_x) / 3u;
    const uint32_t pixels = width < available ? width : available;
    const uint8_t *source = (const uint8_t *)(vram + physical_y * 1024u) + byte_x;
    for (uint32_t x = 0; x < pixels; ++x) {
        rgba[x * 4u] = source[x * 3u];
        rgba[x * 4u + 1u] = source[x * 3u + 1u];
        rgba[x * 4u + 2u] = source[x * 3u + 2u];
        rgba[x * 4u + 3u] = 255;
    }
    for (uint32_t x = pixels; x < width; ++x) {
        rgba[x * 4u] = 0;
        rgba[x * 4u + 1u] = 0;
        rgba[x * 4u + 2u] = 0;
        rgba[x * 4u + 3u] = 255;
    }
}

#endif
