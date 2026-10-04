/* Standalone API for the MAME-derived TMS57002 DASP core. */
#ifndef PCSX_ARCADE_TMS57002_H
#define PCSX_ARCADE_TMS57002_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tms57002 tms57002_t;

typedef uint8_t (*tms57002_data_read_cb)(void *opaque, uint32_t address);
typedef void (*tms57002_data_write_cb)(void *opaque, uint32_t address,
                                       uint8_t value);
typedef void (*tms57002_line_cb)(void *opaque, int state);

typedef struct tms57002_config {
    void *opaque;
    tms57002_data_read_cb data_read;
    tms57002_data_write_cb data_write;
    tms57002_line_cb dready_changed;
    tms57002_line_cb pc0_changed;
    tms57002_line_cb empty_changed;
    uint64_t cycle_origin; /* Absolute board clock at creation; reset preserves it. */
} tms57002_config_t;

tms57002_t *tms57002_create(const tms57002_config_t *config);
void tms57002_destroy(tms57002_t *dsp);
void tms57002_reset(tms57002_t *dsp);

/* Runs whole DSP clock cycles. Returns cycles consumed, -1 on absolute-cycle
 * overflow, or -2 if called reentrantly from one of the synchronous callbacks.
 * Rejected requests have no state or callback side effects. */
int64_t tms57002_run(tms57002_t *dsp, uint64_t cycles);
uint64_t tms57002_total_cycles(const tms57002_t *dsp);
/* During a synchronous external-memory/status callback, this is the exact
 * cycle position at that access; otherwise it equals total_cycles(). */
uint64_t tms57002_current_cycle(const tms57002_t *dsp);
int tms57002_callback_fault(const tms57002_t *dsp);
int tms57002_unsupported_instruction(const tms57002_t *dsp);

/* Host-side 8-bit data port and control/status pins follow MAME semantics. */
uint8_t tms57002_data_r(tms57002_t *dsp);
void tms57002_data_w(tms57002_t *dsp, uint8_t value);
void tms57002_pload_w(tms57002_t *dsp, int state);
void tms57002_cload_w(tms57002_t *dsp, int state);
int tms57002_dready_r(const tms57002_t *dsp);
int tms57002_pc0_r(const tms57002_t *dsp);
int tms57002_empty_r(const tms57002_t *dsp);
void tms57002_sync_w(tms57002_t *dsp, int state);

/* One LRCLK sample: four normalized inputs and four normalized outputs.
 * This mirrors MAME's synchronous sound_stream_update path and does not run
 * DSP instruction cycles itself. */
int tms57002_process_sample(tms57002_t *dsp, const float input[4],
                            float output[4]);

/* Host diagnostic/program access. Writes invalidate the decoded instruction
 * cache and use the same 256-entry internal program RAM as host PLOAD. */
int tms57002_program_write(tms57002_t *dsp, uint8_t address, uint32_t opcode);
uint32_t tms57002_program_read(const tms57002_t *dsp, uint8_t address);
uint8_t tms57002_pc(const tms57002_t *dsp);
uint32_t tms57002_st0(const tms57002_t *dsp);
uint32_t tms57002_st1(const tms57002_t *dsp);
uint32_t tms57002_aacc(const tms57002_t *dsp);
int64_t tms57002_macc(const tms57002_t *dsp);
uint32_t tms57002_dmem_read(const tms57002_t *dsp, unsigned bank,
                            unsigned address);

#ifdef __cplusplus
}
#endif
#endif
