/* Optional fixed-capacity GP0 candidate trace for the System 12 water ROI.
 * Compile only with PCSX_GP0_ROI_TRACE; the normal Vita renderer is unchanged. */
#ifndef PCSX_GPU_VITA_CTR_GP0_ROI_TRACE_H
#define PCSX_GPU_VITA_CTR_GP0_ROI_TRACE_H

#include <stdint.h>

#define GP0_ROI_TRACE_CAPACITY 128
#define GP0_ROI_TRACE_WORDS 255
#define GP0_ROI_TRACE_FIRST_FRAME 3597
#define GP0_ROI_TRACE_LAST_FRAME 3599

enum gp0_roi_trace_kind {
	GP0_ROI_TRACE_PACKET = 1,
	GP0_ROI_TRACE_VRAM_UPDATE = 2,
	GP0_ROI_TRACE_LOAD_IMAGE = 3
};

typedef struct gp0_roi_trace_record {
	uint32_t frame;
	uint32_t sequence;
	uint32_t regs[7];
	uint32_t raw[GP0_ROI_TRACE_WORDS];
	uint32_t normalized[GP0_ROI_TRACE_WORDS];
	uint16_t x0, y0, x1, y1;
	uint16_t count;
	uint8_t kind;
	uint8_t stored_words;
	uint8_t point_mask;
	uint8_t accepted_mask;
	uint32_t color;
	uint32_t uv[4];
	uint16_t clut;
	uint16_t tpage;
} gp0_roi_trace_record;

static gp0_roi_trace_record gp0_roi_trace_ring[GP0_ROI_TRACE_CAPACITY];
static uint32_t gp0_roi_trace_count;
static uint32_t gp0_roi_trace_sequence;

#endif
