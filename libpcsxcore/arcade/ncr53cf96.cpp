/*
 * NCR 53CF96-2 SCSI controller core, derived from MAME's ncr53c90_device and
 * NSCSI bus endpoint line semantics from nscsi_bus.cpp/.h.
 * Source revision deb2f54fe7300e7a192690318beb860159bfd52a.
 * Copyright Olivier Galibert. SPDX-License-Identifier: BSD-3-Clause
 * Fast CF96-specific changes are explicitly TODO in upstream; it currently
 * inherits the CF94 implementation unchanged.
 */
#include "ncr53cf96.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace {
enum : int {
    MODE_D, MODE_T, MODE_I,
    IDLE,
    BUSRESET_WAIT_INT,
    DISC_SEL_ARBITRATION_INIT, DISC_SEL_ARBITRATION,
    DISC_SEL_ATN_WAIT_REQ, DISC_SEL_ATN_SEND_BYTE,
    DISC_SEL_WAIT_REQ, DISC_SEL_SEND_BYTE,
    DISC_REC_ARBITRATION,
    CMDSEQ_CMD_PHASE, CMDSEQ_RECV_BYTE,
    TARGET_SEND_BYTE, TARGET_CMD_RECV_BYTE, TARGET_MSG_RECV_BYTE,
    TARGET_MSG_RECV_PAD, TARGET_DISC_SEND_BYTE, TARGET_DISC_MSG_IN,
    TARGET_DISC_SEND_BYTE_2,
    INIT_MSG_WAIT_REQ, INIT_XFR, INIT_XFR_SEND_BYTE,
    INIT_XFR_SEND_PAD_WAIT_REQ, INIT_XFR_SEND_PAD,
    INIT_XFR_RECV_PAD_WAIT_REQ, INIT_XFR_RECV_PAD,
    INIT_XFR_RECV_BYTE_ACK, INIT_XFR_RECV_BYTE_NACK,
    INIT_XFR_FUNCTION_COMPLETE, INIT_XFR_BUS_COMPLETE,
    INIT_XFR_WAIT_REQ, INIT_CPT_RECV_BYTE_ACK,
    INIT_CPT_RECV_WAIT_REQ, INIT_CPT_RECV_BYTE_NACK
};
enum : int {
    ARB_WAIT_BUS_FREE = 1, ARB_COMPLETE, ARB_ASSERT_SEL, ARB_SET_DEST,
    ARB_RELEASE_BUSY, ARB_TIMEOUT_BUSY, ARB_TIMEOUT_ABORT, ARB_DESKEW_WAIT,
    SEND_WAIT_SETTLE, SEND_WAIT_REQ_0,
    RECV_WAIT_REQ_1, RECV_WAIT_SETTLE, RECV_WAIT_REQ_0
};
enum : int { STATE_MASK = 0xff, SUB_SHIFT = 8, SUB_MASK = 0xff00 };
enum : int { DMA_NONE, DMA_IN, DMA_OUT };
enum : uint8_t {
    S_GROSS_ERROR = 0x40, S_PARITY = 0x20, S_TC0 = 0x10, S_TCC = 0x08,
    I_SCSI_RESET = 0x80, I_ILLEGAL = 0x40, I_DISCONNECT = 0x20,
    I_BUS = 0x10, I_FUNCTION = 0x08, I_RESELECTED = 0x04,
    I_SELECT_ATN = 0x02, I_SELECTED = 0x01,
    CM_NOP = 0x00, CM_FLUSH_FIFO = 0x01, CM_RESET = 0x02,
    CM_RESET_BUS = 0x03, CD_RESELECT = 0x40, CD_SELECT = 0x41,
    CD_SELECT_ATN = 0x42, CD_SELECT_ATN_STOP = 0x43,
    CD_ENABLE_SEL = 0x44, CD_DISABLE_SEL = 0x45,
    CD_SELECT_ATN3 = 0x46, CT_ABORT_DMA = 0x04,
    CT_SEND_MSG = 0x20, CT_SEND_STATUS = 0x21, CT_SEND_DATA = 0x22,
    CT_DISCONNECT_SEQ = 0x23, CT_TERMINATE = 0x24, CT_COMPLETE = 0x25,
    CT_DISCONNECT = 0x27, CT_RECV_MSG = 0x28, CT_RECV_CMD = 0x29,
    CT_RECV_DATA = 0x2a, CT_RECV_CMD_SEQ = 0x2b,
    CI_XFER = 0x10, CI_COMPLETE = 0x11, CI_MSG_ACCEPT = 0x12,
    CI_PAD = 0x18, CI_SET_ATN = 0x1a, CI_RESET_ATN = 0x1b
};
constexpr uint32_t S_INP = NCR53CF96_S_INP;
constexpr uint32_t S_CTL = NCR53CF96_S_CTL;
constexpr uint32_t S_MSG = NCR53CF96_S_MSG;
constexpr uint32_t S_BSY = NCR53CF96_S_BSY;
constexpr uint32_t S_SEL = NCR53CF96_S_SEL;
constexpr uint32_t S_REQ = NCR53CF96_S_REQ;
constexpr uint32_t S_ACK = NCR53CF96_S_ACK;
constexpr uint32_t S_ATN = NCR53CF96_S_ATN;
constexpr uint32_t S_RST = NCR53CF96_S_RST;
constexpr uint32_t S_ALL = 0x1ff;
constexpr uint32_t S_PHASE_MASK = NCR53CF96_S_PHASE_MASK;
constexpr uint32_t S_PHASE_DATA_IN = NCR53CF96_PHASE_DATA_IN;
constexpr uint32_t S_PHASE_COMMAND = NCR53CF96_PHASE_COMMAND;
constexpr uint32_t S_PHASE_STATUS = NCR53CF96_PHASE_STATUS;
constexpr uint32_t S_PHASE_MSG_OUT = NCR53CF96_PHASE_MSG_OUT;
constexpr uint32_t S_PHASE_MSG_IN = NCR53CF96_PHASE_MSG_IN;
constexpr uint8_t CONF2_S2FE = 0x08;
}

struct Ncr53cf96 {
    uint32_t clock_hz = 16000000;
    ncr53cf96_bus_ops_t ops{};
    uint64_t now = 0, deadline = UINT64_MAX;
    uint32_t bus_ctrl = 0, bus_data = 0;
    uint32_t host_ctrl = 0, host_data = 0, external_ctrl = 0, external_data = 0;
    uint8_t command[2]{}, config = 0, status = 0, istatus = 0;
    uint8_t clock_conv = 2, sync_offset = 0, sync_period = 5, bus_id = 0;
    uint8_t select_timeout = 0, seq = 0;
    std::array<uint8_t, 16> fifo{};
    uint32_t tcount = 0, tcounter = 0, tcounter_mask = 0xffff;
    int mode = MODE_D, fifo_pos = 0, command_pos = 0;
    int state = IDLE, xfr_phase = 0, dma_dir = DMA_NONE, stepping = 0;
    bool irq = false, drq = false, dma_command = false, test_mode = false;
    uint8_t config2 = 0, config3 = 0, config4 = 0;
    uint8_t family_id = 4, revision_level = 2;
    uint8_t fifo_align = 0;
    uint8_t scsi_id = 7; /* nscsi_device_interface default ID for Konami boards */
    uint32_t faults = 0;

    void set_irq() {
        bool next = istatus != 0;
        if (next != irq) { irq = next; if (ops.irq) ops.irq(ops.opaque, irq); }
    }
    void set_drq(bool next) {
        if (next != drq) { drq = next; if (ops.drq) ops.drq(ops.opaque, drq); }
    }
    void drive_ctrl(uint32_t lines, uint32_t mask) {
        host_ctrl = (host_ctrl & ~mask) | (lines & mask);
        bus_ctrl = (host_ctrl | external_ctrl) & S_ALL;
        if (ops.drive_ctrl) ops.drive_ctrl(ops.opaque, lines, mask);
    }
    void drive_data(uint32_t lines) {
        host_data = lines & 0xff; bus_data = host_data | external_data;
        if (ops.drive_data) ops.drive_data(ops.opaque, lines);
    }
    void wait_ctrl(uint32_t lines, uint32_t mask) {
        if (ops.wait_ctrl) ops.wait_ctrl(ops.opaque, lines, mask);
    }
    void delay(int cycles) {
        int mul = clock_conv ? clock_conv : 8;
        uint64_t ticks=uint64_t(std::max(cycles, 0))*uint64_t(mul);
        if(ticks>=UINT64_MAX-now){faults|=NCR53CF96_FAULT_TIME_OVERFLOW;deadline=UINT64_MAX;}
        else deadline=now+ticks;
    }
    void delay_cycles(int cycles) {
        uint64_t ticks=uint64_t(std::max(cycles, 0));
        if(ticks>=UINT64_MAX-now){faults|=NCR53CF96_FAULT_TIME_OVERFLOW;deadline=UINT64_MAX;}
        else deadline=now+ticks;
    }
    void update_deadline() { if (deadline < now) deadline = now; }
    void reset_disconnect() {
        drive_ctrl(0, ~S_RST);
        command_pos = 0; command[0] = command[1] = 0; mode = MODE_D;
    }
    void reset() {
        fifo_pos = 0; fifo.fill(0);
        clock_conv = 2; sync_period = 5; sync_offset = 0; seq = 0;
        config &= 7; status = 0; istatus = 0; config2 = config3 = config4 = 0;
        tcounter_mask = 0xffff;
        faults = 0;
        irq = false; if (ops.irq) ops.irq(ops.opaque, 0);
        state = IDLE; wait_ctrl(S_SEL|S_BSY|S_RST, S_ALL);
        drq = false; if (ops.drq) ops.drq(ops.opaque, 0); test_mode = false;
        drive_ctrl(0, S_RST); tcount = tcounter = 0; tcounter_mask = 0xffff;
        reset_disconnect();
    }
    void decrement(int count = 1) {
        if (!dma_command) return;
        if (!(status & S_TC0)) tcounter = (tcounter - uint32_t(count)) & tcounter_mask;
        else tcounter = 0;
        if (!tcounter) status |= S_TC0;
        check_drq();
    }
    uint8_t pop() {
        uint8_t val = fifo[0];
        if (fifo_pos) { --fifo_pos; std::memmove(fifo.data(), fifo.data()+1, size_t(fifo_pos)); }
        check_drq(); return val;
    }
    void push(uint8_t val) {
        if (fifo_pos != 16) fifo[size_t(fifo_pos++)] = val;
        check_drq();
    }
    void check_drq() {
        bool next = drq;
        if (dma_dir == DMA_NONE) next = false;
        else if (dma_dir == DMA_IN) {
            if (!sync_offset) next = fifo_pos > 0;
            else next = !(status & S_TC0) && fifo_pos;
        } else if (dma_dir == DMA_OUT) next = !(status & S_TC0) && fifo_pos < 16;
        set_drq(next);
    }
    void dma_set(int direction) { dma_dir = direction; check_drq(); }
    void step(bool timeout);
    void start_command();
    void command_chain() {
        if (command_pos) {
            --command_pos;
            if (command_pos) { command[0] = command[1]; start_command(); }
        }
    }
    bool valid_command(uint8_t cmd) {
        int sub = cmd & 15;
        switch ((cmd >> 4) & 7) {
        case 0: return sub <= 3 || (mode == MODE_T && sub == 4);
        case 4: return mode == MODE_D && sub <= 6;
        case 2: return mode == MODE_T && sub <= 11 && sub != 6;
        case 1: return mode == MODE_I && (sub <= 2 || sub == 8 || sub == 10 || sub == 11);
        default: return false;
        }
    }
    void load_tcounter() {
        tcounter = tcount & tcounter_mask;
        status &= uint8_t(~S_TC0);
        if (!(config2 & CONF2_S2FE))
            tcount = (1u << 23) | (uint32_t(family_id) << 19) |
                (uint32_t(revision_level) << 16) | (tcount & 0xffff);
    }
    void function_bus_complete() { state=IDLE; istatus|=I_FUNCTION|I_BUS; dma_set(DMA_NONE); set_irq(); }
    void function_complete() { state=IDLE; istatus|=I_FUNCTION; dma_set(DMA_NONE); set_irq(); }
    void bus_complete() { state=IDLE; istatus|=I_BUS; dma_set(DMA_NONE); set_irq(); }
    void send_byte() {
        state = (state & STATE_MASK) | (SEND_WAIT_SETTLE << SUB_SHIFT);
        if ((state & STATE_MASK) != INIT_XFR_SEND_PAD) {
            if (!fifo_pos) { status |= S_GROSS_ERROR; function_complete(); return; }
            drive_data(pop());
        } else drive_data(0);
        drive_ctrl(S_ACK, S_ACK); wait_ctrl(S_REQ, S_REQ); delay_cycles(sync_period);
    }
    void recv_byte() {
        wait_ctrl(S_REQ, S_REQ); state=(state&STATE_MASK)|(RECV_WAIT_REQ_1<<SUB_SHIFT); step(false);
    }
    void arbitrate() {
        state=(state&STATE_MASK)|(ARB_COMPLETE<<SUB_SHIFT);
        drive_data(1u << scsi_id); drive_ctrl(S_BSY,S_BSY); delay(11);
    }
    void check_irq() { set_irq(); }
};

void Ncr53cf96::step(bool timeout)
{
    uint32_t ctrl = bus_ctrl, data = bus_data;
    uint8_t c = command[0] & 0x7f;
    ++stepping;
    if (mode == MODE_I && !(ctrl & S_BSY)) {
        state = IDLE; istatus |= I_DISCONNECT; reset_disconnect(); check_irq();
    }
    int st = (state & SUB_MASK) ? state & SUB_MASK : state & STATE_MASK;
    switch (st) {
    case IDLE: break;
    case BUSRESET_WAIT_INT:
        state=IDLE; drive_ctrl(0,S_RST); reset_disconnect();
        if (!(config & 0x40)) { istatus|=I_SCSI_RESET; check_irq(); } break;
    case ARB_COMPLETE << SUB_SHIFT:
        if (!timeout) break;
        { int win=7; for (; win>=0 && !(data&(1u<<win)); --win) {}
          if (win != scsi_id) {
              drive_data(0); drive_ctrl(0,S_ALL); faults |= NCR53CF96_FAULT_ARBITRATION_LOST;
              status |= S_GROSS_ERROR; istatus |= I_ILLEGAL; state=IDLE; check_irq(); break;
          } }
        state=(state&STATE_MASK)|(ARB_ASSERT_SEL<<SUB_SHIFT); drive_ctrl(S_SEL,S_SEL); delay(6); break;
    case ARB_ASSERT_SEL << SUB_SHIFT:
        if (!timeout) break;
        drive_data((1u<<scsi_id)|(1u<<bus_id)); state=(state&STATE_MASK)|(ARB_SET_DEST<<SUB_SHIFT); delay_cycles(4); break;
    case ARB_SET_DEST << SUB_SHIFT:
        if (!timeout) break;
        state=(state&STATE_MASK)|(ARB_RELEASE_BUSY<<SUB_SHIFT);
        drive_ctrl((c==CD_SELECT_ATN||c==CD_SELECT_ATN_STOP)?S_ATN:0,S_ATN|S_BSY); delay(2); break;
    case ARB_RELEASE_BUSY << SUB_SHIFT:
        if (!timeout) break;
        if (ctrl&S_BSY) { state=(state&STATE_MASK)|(ARB_DESKEW_WAIT<<SUB_SHIFT); if(c==CD_RESELECT) drive_ctrl(S_BSY,S_BSY); delay_cycles(2); }
        else { state=(state&STATE_MASK)|(ARB_TIMEOUT_BUSY<<SUB_SHIFT); delay(1); } break;
    case ARB_DESKEW_WAIT << SUB_SHIFT:
        if (!timeout) break;
        drive_data(0); drive_ctrl(0,S_SEL);
        if(c==CD_RESELECT) mode=MODE_T; else mode=MODE_I;
        state &= STATE_MASK; step(true); break;
    case ARB_TIMEOUT_BUSY << SUB_SHIFT:
        if(timeout) { drive_data(0); state=(state&STATE_MASK)|(ARB_TIMEOUT_ABORT<<SUB_SHIFT); delay(1000); }
        else if(ctrl&S_BSY) { state=(state&STATE_MASK)|(ARB_DESKEW_WAIT<<SUB_SHIFT); if(c==CD_RESELECT)drive_ctrl(S_BSY,S_BSY); delay_cycles(2); } break;
    case ARB_TIMEOUT_ABORT << SUB_SHIFT:
        if (!timeout) break;
        if(ctrl&S_BSY) { state=(state&STATE_MASK)|(ARB_DESKEW_WAIT<<SUB_SHIFT); if(c==CD_RESELECT)drive_ctrl(S_BSY,S_BSY); delay_cycles(2); }
        else { drive_ctrl(0,S_ALL); state=IDLE; istatus|=I_DISCONNECT; reset_disconnect(); check_irq(); } break;
    case SEND_WAIT_SETTLE << SUB_SHIFT:
        if(timeout) { state=(state&STATE_MASK)|(SEND_WAIT_REQ_0<<SUB_SHIFT); step(false); } break;
    case SEND_WAIT_REQ_0 << SUB_SHIFT:
        if(ctrl&S_REQ) break;
        state &= STATE_MASK; drive_data(0); drive_ctrl(0,S_ACK); step(false); break;
    case RECV_WAIT_REQ_1 << SUB_SHIFT:
        if(!(ctrl&S_REQ)) break;
        state=(state&STATE_MASK)|(RECV_WAIT_SETTLE<<SUB_SHIFT); delay_cycles(sync_period); break;
    case RECV_WAIT_SETTLE << SUB_SHIFT:
        if(!timeout) break;
        if((state&STATE_MASK)!=INIT_XFR_RECV_PAD) {
            push(uint8_t(data));
            if(mode==MODE_I && !sync_offset && (ctrl&S_PHASE_MASK)==S_PHASE_DATA_IN) decrement();
        }
        drive_ctrl(S_ACK,S_ACK); state=(state&STATE_MASK)|(RECV_WAIT_REQ_0<<SUB_SHIFT); step(false); break;
    case RECV_WAIT_REQ_0 << SUB_SHIFT:
        if(ctrl&S_REQ) break;
        state &= STATE_MASK; step(false); break;
    case DISC_SEL_ARBITRATION_INIT:
        if(timeout) { state=DISC_SEL_ARBITRATION; step(false); } break;
    case DISC_SEL_ARBITRATION:
        if(!fifo_pos) { seq=1; check_drq(); break; }
        state=(c==CD_SELECT)?DISC_SEL_WAIT_REQ:DISC_SEL_ATN_WAIT_REQ;
        wait_ctrl(S_REQ,S_REQ); step(false); break;
    case DISC_SEL_ATN_WAIT_REQ:
        if(!(ctrl&S_REQ)) break;
        if((ctrl&S_PHASE_MASK)!=S_PHASE_MSG_OUT) { function_complete(); break; }
        if(c==CD_SELECT_ATN) drive_ctrl(0,S_ATN);
        state=DISC_SEL_ATN_SEND_BYTE; send_byte(); break;
    case DISC_SEL_ATN_SEND_BYTE:
        if(c==CD_SELECT_ATN_STOP) { seq=2; function_bus_complete(); }
        else { state=DISC_SEL_WAIT_REQ; step(false); } break;
    case DISC_SEL_WAIT_REQ:
        if(!(ctrl&S_REQ)) break;
        if((ctrl&S_PHASE_MASK)!=S_PHASE_COMMAND) {
            seq=(!dma_command||(status&S_TC0))&&!fifo_pos?4:2;
            wait_ctrl(0,S_REQ); function_bus_complete(); break;
        }
        if(!fifo_pos) break;
        if(seq<3)seq=3;
        state=DISC_SEL_SEND_BYTE; send_byte(); break;
    case DISC_SEL_SEND_BYTE:
        if((!dma_command||(status&S_TC0))&&!fifo_pos) seq=4;
        state=DISC_SEL_WAIT_REQ; step(false); break;
    case INIT_CPT_RECV_BYTE_ACK:
        state=INIT_CPT_RECV_WAIT_REQ; drive_ctrl(0,S_ACK); step(false); break;
    case INIT_CPT_RECV_WAIT_REQ:
        if(!(ctrl&S_REQ))break;
        if((ctrl&S_PHASE_MASK)!=S_PHASE_MSG_IN) { command_pos=0; bus_complete(); }
        else { state=INIT_CPT_RECV_BYTE_NACK; recv_byte(); } break;
    case INIT_CPT_RECV_BYTE_NACK: function_complete(); break;
    case INIT_MSG_WAIT_REQ:
        if((ctrl&(S_REQ|S_BSY))==S_BSY)break; bus_complete(); break;
    case INIT_XFR:
        if(xfr_phase==NCR53CF96_PHASE_DATA_OUT||xfr_phase==NCR53CF96_PHASE_COMMAND||xfr_phase==NCR53CF96_PHASE_MSG_OUT) {
            state=INIT_XFR_SEND_BYTE;
            if(!fifo_pos)break;
            int remain=fifo_pos+(dma_command?int(tcounter):0);
            if(xfr_phase==NCR53CF96_PHASE_MSG_OUT&&remain==1)drive_ctrl(0,S_ATN);
            send_byte();
        } else if(xfr_phase==S_PHASE_DATA_IN||xfr_phase==S_PHASE_STATUS||xfr_phase==S_PHASE_MSG_IN) {
            if(fifo_pos==16)break;
            state=(xfr_phase==S_PHASE_MSG_IN&&(!dma_command||tcounter==1))?INIT_XFR_RECV_BYTE_NACK:INIT_XFR_RECV_BYTE_ACK;
            recv_byte();
        } else function_complete();
        break;
    case INIT_XFR_WAIT_REQ:
        if(!(ctrl&S_REQ))break;
        if((dma_command&&(status&S_TC0)&&(dma_dir==DMA_IN||fifo_pos==0))||
            (!dma_command&&!(xfr_phase&1)&&fifo_pos==0)||
            (!dma_command&&(xfr_phase&1)&&fifo_pos==1)) state=INIT_XFR_BUS_COMPLETE;
        else if((ctrl&S_PHASE_MASK)!=uint32_t(xfr_phase)) { command_pos=0; state=INIT_XFR_BUS_COMPLETE; }
        else state=INIT_XFR;
        step(false); break;
    case INIT_XFR_SEND_BYTE: state=INIT_XFR_WAIT_REQ; step(false); break;
    case INIT_XFR_RECV_BYTE_ACK: state=INIT_XFR_WAIT_REQ; drive_ctrl(0,S_ACK); step(false); break;
    case INIT_XFR_RECV_BYTE_NACK: state=INIT_XFR_FUNCTION_COMPLETE; step(false); break;
    case INIT_XFR_FUNCTION_COMPLETE:
        if(dma_command&&(!(status&S_TC0)||fifo_pos))break; function_complete(); break;
    case INIT_XFR_BUS_COMPLETE:
        if(dma_command&&drq)break; bus_complete(); break;
    case INIT_XFR_SEND_PAD_WAIT_REQ:
        if(!(ctrl&S_REQ))break;
        if((ctrl&S_PHASE_MASK)!=uint32_t(xfr_phase)){command_pos=0;bus_complete();}
        else{state=INIT_XFR_SEND_PAD;send_byte();} break;
    case INIT_XFR_SEND_PAD:
        decrement(); if(!(status&S_TC0)){state=INIT_XFR_SEND_PAD_WAIT_REQ;step(false);}else function_complete(); break;
    case INIT_XFR_RECV_PAD_WAIT_REQ:
        if(!(ctrl&S_REQ))break;
        if((ctrl&S_PHASE_MASK)!=uint32_t(xfr_phase)){command_pos=0;bus_complete();}
        else{state=INIT_XFR_RECV_PAD;recv_byte();} break;
    case INIT_XFR_RECV_PAD:
        decrement(); if(!(status&S_TC0)){state=INIT_XFR_RECV_PAD_WAIT_REQ;drive_ctrl(0,S_ACK);step(false);}else function_complete(); break;
    case CMDSEQ_CMD_PHASE: case CMDSEQ_RECV_BYTE: case TARGET_SEND_BYTE:
    case TARGET_CMD_RECV_BYTE: case TARGET_MSG_RECV_BYTE: case TARGET_MSG_RECV_PAD:
    case TARGET_DISC_SEND_BYTE: case TARGET_DISC_MSG_IN: case TARGET_DISC_SEND_BYTE_2:
    case DISC_REC_ARBITRATION:
        /* Upstream validates some target commands but its start_command
         * switch has no handlers for them and reaches fatalerror(). */
        status |= S_GROSS_ERROR; istatus |= I_ILLEGAL; state=IDLE; dma_set(DMA_NONE); check_irq(); break;
    default:
        status |= S_GROSS_ERROR; istatus |= I_ILLEGAL; state=IDLE; dma_set(DMA_NONE); check_irq(); break;
    }
    --stepping;
}

void Ncr53cf96::start_command()
{
    uint8_t c=command[0]&0x7f;
    if(!valid_command(c)) { istatus|=I_ILLEGAL; set_irq(); return; }
    dma_command=!!(command[0]&0x80);
    if(dma_command) load_tcounter(); else tcounter=0;
    switch(c) {
    case CM_NOP: command_chain(); break;
    case CM_FLUSH_FIFO: fifo_pos=0; command_chain(); break;
    case CM_RESET: reset(); break;
    case CM_RESET_BUS: state=BUSRESET_WAIT_INT; drive_ctrl(S_RST,S_RST); delay(130); break;
    case CD_RESELECT: state=DISC_REC_ARBITRATION; arbitrate(); break;
    case CD_SELECT: case CD_SELECT_ATN: case CD_SELECT_ATN_STOP:
        seq=0; state=DISC_SEL_ARBITRATION_INIT; dma_set(dma_command?DMA_OUT:DMA_NONE); arbitrate(); break;
    case CD_ENABLE_SEL: command_chain(); break;
    case CD_DISABLE_SEL: function_complete(); command_chain(); break;
    case CI_XFER:
        state=INIT_XFR; xfr_phase=int(bus_ctrl&S_PHASE_MASK);
        dma_set(dma_command?((xfr_phase&S_INP)?DMA_IN:DMA_OUT):DMA_NONE); step(false); break;
    case CI_COMPLETE: state=INIT_CPT_RECV_BYTE_ACK; dma_set(dma_command?DMA_IN:DMA_NONE); recv_byte(); break;
    case CI_MSG_ACCEPT:
        state=INIT_MSG_WAIT_REQ; seq=2; drive_ctrl(0,S_ACK); step(false); break;
    case CI_PAD:
        xfr_phase=int(bus_ctrl&S_PHASE_MASK);
        state=(xfr_phase&S_INP)?INIT_XFR_RECV_PAD_WAIT_REQ:INIT_XFR_SEND_PAD_WAIT_REQ;
        drive_ctrl(0,S_ACK); step(false); break;
    case CI_SET_ATN: drive_ctrl(S_ATN,S_ATN); command_chain(); break;
    case CI_RESET_ATN: drive_ctrl(0,S_ATN); command_chain(); break;
    default: istatus|=I_ILLEGAL; set_irq(); break;
    }
}

static uint8_t status_value(const Ncr53cf96 *c)
{
    return uint8_t(c->status | ((c->bus_ctrl&S_MSG)?4:0) |
        ((c->bus_ctrl&S_CTL)?2:0) | ((c->bus_ctrl&S_INP)?1:0) |
        (c->irq?0x80:0));
}

extern "C" Ncr53cf96 *ncr53cf96_create(uint32_t clock_hz, const ncr53cf96_bus_ops_t *ops)
{
    if (!clock_hz || !ops || !ops->drive_ctrl || !ops->drive_data) return nullptr;
    Ncr53cf96 *c = new (std::nothrow) Ncr53cf96;
    if (!c) return nullptr;
    c->clock_hz=clock_hz; c->ops=*ops; c->reset(); return c;
}
extern "C" void ncr53cf96_destroy(Ncr53cf96 *c) { delete c; }
extern "C" void ncr53cf96_reset(Ncr53cf96 *c) { if(c)c->reset(); }

extern "C" uint8_t ncr53cf96_read(Ncr53cf96 *c, unsigned reg)
{
    if(!c||reg>=16)return 0xff;
    switch(reg) {
    case 0:return uint8_t(c->tcounter);
    case 1:return uint8_t(c->tcounter>>8);
    case 2: { uint8_t v=0; if(c->fifo_pos){v=c->fifo[0];--c->fifo_pos;std::memmove(c->fifo.data(),c->fifo.data()+1,size_t(c->fifo_pos));} c->check_drq(); c->step(false); return v; }
    case 3:return c->command[0];
    case 4: { uint8_t v=status_value(c); if(c->irq)c->status&=uint8_t(~(S_GROSS_ERROR|S_PARITY|S_TCC)); return v; }
    case 5: { uint8_t v=c->istatus; if(c->irq){c->status&=uint8_t(~(S_GROSS_ERROR|S_PARITY|S_TCC));c->istatus=0;c->seq=0;} c->set_irq(); if(v)c->command_chain(); return v; }
    case 6:return c->seq;
    case 7:return uint8_t(c->fifo_pos);
    case 8:return c->config;
    case 11:return c->config2;
    case 12:return c->config3;
    case 13:return c->config4;
    case 14:return uint8_t(c->tcounter>>16);
    default:return 0xff;
    }
}

extern "C" void ncr53cf96_write(Ncr53cf96 *c, unsigned reg, uint8_t v)
{
    if(!c||reg>=16)return;
    switch(reg) {
    case 0:c->tcount=(c->tcount&~0xffu)|v;break;
    case 1:c->tcount=(c->tcount&~0xff00u)|(uint32_t(v)<<8);break;
    case 2:if(c->fifo_pos<16)c->fifo[size_t(c->fifo_pos++)]=v;c->check_drq();c->step(false);break;
    case 3:
        if(c->command_pos==2){c->status|=S_GROSS_ERROR;c->check_irq();break;}
        if((v&0x7f)==CM_RESET||(v&0x7f)==CM_RESET_BUS)c->command_pos=0;
        c->command[c->command_pos++]=v;if(c->command_pos==1)c->start_command();break;
    case 4:c->bus_id=v&7;break;
    case 5:c->select_timeout=v;break;
    case 6:c->sync_period=v&0x1f;break;
    case 7:c->sync_offset=v&15;break;
    case 8:
        c->config=v;c->scsi_id=v&7;
        if(v&0x08)c->test_mode=true;
        break;
    case 9:c->clock_conv=v&7;break;
    case 10:break;
    case 11:c->config2=v;c->tcounter_mask=(v&CONF2_S2FE)?0xffffff:0xffff;break;
    case 12:c->config3=v;break;
    case 13:c->config4=v;break;
    case 14:c->tcount=(c->tcount&~0xff0000u)|(uint32_t(v)<<16);break;
    case 15:c->fifo_align=v;break;
    }
}

extern "C" void ncr53cf96_advance_to(Ncr53cf96 *c, uint64_t cycles)
{
    if(!c)return;
    if(cycles<c->now){c->faults|=NCR53CF96_FAULT_TIME_REWIND;return;}
    while(c->deadline!=UINT64_MAX&&c->deadline<=cycles) {
        c->now=c->deadline; c->deadline=UINT64_MAX; c->step(true);
        if(c->deadline==c->now && c->stepping==0) { /* zero-delay source transitions */ continue; }
    }
    c->now=cycles;
}

extern "C" void ncr53cf96_bus_changed(Ncr53cf96 *c,uint32_t ctrl,uint32_t data)
{
    if(!c)return;
    c->external_ctrl=ctrl&S_ALL;c->external_data=data&0xff;
    c->bus_ctrl=(c->host_ctrl|c->external_ctrl)&S_ALL;
    c->bus_data=c->host_data|c->external_data;
    if(c->bus_ctrl&S_RST)return;
    if(!c->stepping)c->step(false);
}

extern "C" uint8_t ncr53cf96_dma_read(Ncr53cf96 *c)
{
    if(!c)return 0xff;uint8_t v=c->pop();
    if(c->sync_offset||((c->bus_ctrl&S_PHASE_MASK)!=S_PHASE_DATA_IN))c->decrement();
    c->check_drq();c->step(false);return v;
}
extern "C" void ncr53cf96_dma_write(Ncr53cf96 *c,uint8_t v)
{if(c){c->push(v);c->decrement();c->check_drq();c->step(false);}}
extern "C" uint16_t ncr53cf96_dma16_read(Ncr53cf96 *c)
{
    if(!c)return 0xffff;
    if(c->fifo_pos<2)return uint16_t(ncr53cf96_dma_read(c)|0xff00);
    uint16_t v=uint16_t(c->fifo[0]|(uint16_t(c->fifo[1])<<8));
    c->fifo_pos-=2;std::memmove(c->fifo.data(),c->fifo.data()+2,size_t(c->fifo_pos));
    if(c->sync_offset||((c->bus_ctrl&S_PHASE_MASK)!=S_PHASE_DATA_IN))c->decrement(2);
    c->check_drq();c->step(false);return v;
}
extern "C" void ncr53cf96_dma16_write(Ncr53cf96 *c,uint16_t v)
{
    if(!c)return;
    if(c->fifo_pos>14||c->tcounter==1){ncr53cf96_dma_write(c,uint8_t(v));return;}
    c->fifo[size_t(c->fifo_pos++)]=uint8_t(v);c->fifo[size_t(c->fifo_pos++)]=uint8_t(v>>8);
    c->decrement(2);c->check_drq();c->step(false);
}
extern "C" void ncr53cf96_get_status(const Ncr53cf96 *c,ncr53cf96_status_t *s)
{
    if(!c||!s)return;std::memset(s,0,sizeof(*s));s->cycles=c->now;s->timer_deadline=c->deadline;
    s->transfer_count=c->tcount;s->transfer_counter=c->tcounter;s->bus_ctrl=c->bus_ctrl;s->bus_data=c->bus_data;
    s->command=c->command[0];s->status=status_value(c);s->interrupt_status=c->istatus;s->sequence=c->seq;
    s->fifo_count=uint8_t(c->fifo_pos);s->phase=uint8_t(c->bus_ctrl&S_PHASE_MASK);s->mode=uint8_t(c->mode);
    s->config=c->config;s->config2=c->config2;s->config3=c->config3;s->config4=c->config4;
    s->irq=c->irq;s->drq=c->drq;s->dma_direction=uint8_t(c->dma_dir);s->selected_target=c->bus_id;
    s->scsi_id=c->scsi_id;s->faults=c->faults;s->test_mode=c->test_mode;
}
