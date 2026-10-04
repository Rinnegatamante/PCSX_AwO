/* Standalone NMOS Z80 adapted from MAME. BSD-3-Clause; see z80_LICENSE.txt. */
#ifndef PCSX_ARCADE_Z80_H
#define PCSX_ARCADE_Z80_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Z80 Z80;
enum {
    Z80_PC,
    Z80_SP,
    Z80_A,
    Z80_F,
    Z80_B,
    Z80_C,
    Z80_D,
    Z80_E,
    Z80_H,
    Z80_L,
    Z80_AF,
    Z80_BC,
    Z80_DE,
    Z80_HL,
    Z80_IX,
    Z80_IY,
    Z80_AF2,
    Z80_BC2,
    Z80_DE2,
    Z80_HL2,
    Z80_R,
    Z80_I,
    Z80_IM,
    Z80_IFF1,
    Z80_IFF2,
    Z80_HALT,
    Z80_WZ
};
enum { Z80_PIN_HALT, Z80_PIN_BUSACK, Z80_PIN_IRQACK };
enum { Z80_DIAG_IM0_UNSUPPORTED = 1, Z80_DIAG_BAD_PHASE = 2 };
typedef struct Z80Config {
    void *opaque;
    uint8_t (*memory_read)(void *, uint16_t address);
    void (*memory_write)(void *, uint16_t address, uint8_t value);
    uint8_t (*opcode_read)(void *, uint16_t address); /* Optional decrypted opcode/M1 bus. */
    uint8_t (*io_read)(void *, uint16_t port);
    void (*io_write)(void *, uint16_t port, uint8_t value);
    /* Returns a byte for IM2/RST; upstream IM0 also accepts encoded 0xcdAAAA/0xc3AAAA. */
    uint32_t (*irq_ack)(void *);
    void (*reti)(void *); /* Optional board daisy-chain notification. */
    void (*pin)(void *, unsigned pin, int asserted);
    void (*refresh)(void *, uint16_t address);
    void (*nomreq)(void *, uint16_t address);
    void (*diagnostic)(void *, unsigned kind, uint16_t pc, uint32_t detail);
} Z80Config;
typedef struct Z80Status {
    uint16_t pc, previous_pc, sp, af, bc, de, hl, ix, iy, af2, bc2, de2, hl2, wz;
    uint8_t i, r, im, iff1, iff2, halt, irq, nmi, wait, busreq, busack;
    uint32_t execution_phase;
    uint64_t cycles, instructions, interrupts, nmis, undocumented_opcodes, unsupported_operations;
    int64_t cycle_debt;
} Z80Status;
Z80 *z80_create(const Z80Config *config);
void z80_destroy(Z80 *cpu);
void z80_reset(Z80 *cpu);
/* Nominal T-state budgets. Retains bus-cycle overshoot debt; can stop mid-instruction. */
uint64_t z80_run_cycles(Z80 *cpu, uint64_t cycles);
/* Debugger step, independent of scheduler debt. Caps at 65536 T-states; WAIT/BUSREQ return zero. */
uint64_t z80_step_instruction(Z80 *cpu);
void z80_irq(Z80 *cpu, int asserted);
void z80_nmi(Z80 *cpu, int asserted); /* Rising-edge latch; first clock must have elapsed. */
void z80_wait(Z80 *cpu, int asserted);
void z80_busreq(Z80 *cpu, int asserted);
void z80_get_status(Z80 *cpu, Z80Status *status);
uint16_t z80_get_register(Z80 *cpu, unsigned reg);
void z80_set_register(Z80 *cpu, unsigned reg, uint16_t value);
#ifdef __cplusplus
}
#endif
#endif
