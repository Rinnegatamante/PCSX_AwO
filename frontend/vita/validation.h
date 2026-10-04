#ifndef PCSX_VITA_VALIDATION_H
#define PCSX_VITA_VALIDATION_H
#include <stdint.h>

typedef struct {
    uint16_t triangle;
    uint16_t background;
    uint32_t sentinel;
} pcsx_vita_smoke_result;

typedef struct {
    uint32_t magic;
    uint32_t ram_mb;
    uint32_t bank0;
    uint32_t bank1;
    uint32_t upper_ram;
    uint32_t alias;
    uint32_t smc_before;
    uint32_t smc_after;
    uint32_t dma5;
    uint32_t shared_mmio;
    uint32_t shared_mmio_half;
    uint32_t shared_mmio_byte;
} pcsx_vita_system12_drc_result;

/* Private standalone frontend probes; not part of the libretro ABI. */
int pcsx_vita_uses_dynarec(void);
void pcsx_vita_read_smoke(pcsx_vita_smoke_result *result);
int pcsx_vita_read_system12_drc(pcsx_vita_system12_drc_result *result);
int pcsx_vita_write_system12_report(const char *path);
#endif
