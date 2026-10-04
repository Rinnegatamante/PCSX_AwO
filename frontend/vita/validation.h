#ifndef PCSX_VITA_VALIDATION_H
#define PCSX_VITA_VALIDATION_H
#include <stdint.h>

typedef struct {
    uint16_t triangle;
    uint16_t background;
    uint32_t sentinel;
} pcsx_vita_smoke_result;

/* Private standalone frontend probes; not part of the libretro ABI. */
int pcsx_vita_uses_dynarec(void);
void pcsx_vita_read_smoke(pcsx_vita_smoke_result *result);
#endif
