#include "ncr53cf96.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

struct BusProbe {
    uint32_t host_ctrl = 0, host_data = 0, wait_lines = 0, wait_mask = 0;
    int irq_edges = 0, drq_edges = 0;
    int irq = 0, drq = 0;
};

static void drive_ctrl(void *opaque, uint32_t lines, uint32_t mask)
{
    auto *b = static_cast<BusProbe *>(opaque);
    b->host_ctrl = (b->host_ctrl & ~mask) | (lines & mask);
}
static void drive_data(void *opaque, uint32_t bits)
{ static_cast<BusProbe *>(opaque)->host_data = bits; }
static void wait_ctrl(void *opaque, uint32_t lines, uint32_t mask)
{
    auto *b = static_cast<BusProbe *>(opaque);
    b->wait_lines = lines & mask; b->wait_mask = mask;
}
static void irq(void *opaque, int value)
{ auto *b=static_cast<BusProbe *>(opaque); b->irq=value; ++b->irq_edges; }
static void drq(void *opaque, int value)
{ auto *b=static_cast<BusProbe *>(opaque); b->drq=value; ++b->drq_edges; }

static Ncr53cf96 *make_chip(BusProbe &b)
{
    ncr53cf96_bus_ops_t ops{};
    ops.opaque=&b; ops.drive_ctrl=drive_ctrl; ops.drive_data=drive_data;
    ops.wait_ctrl=wait_ctrl; ops.irq=irq; ops.drq=drq;
    return ncr53cf96_create(16000000, &ops);
}

static void registers_and_count()
{
    BusProbe b;
    Ncr53cf96 *c=make_chip(b);
    assert(c);
    assert(ncr53cf96_read(c,8)==0);
    assert(ncr53cf96_read(c,11)==0);
    assert(ncr53cf96_read(c,12)==0);
    assert(ncr53cf96_read(c,13)==0);
    assert(ncr53cf96_read(c,7)==0);
    assert(ncr53cf96_read(c,15)==0xff);

    /* The board converts low-byte-lane word offsets in its 32-byte window
     * into the controller's sixteen register slots. */
    ncr53cf96_write(c,8,0x07);
    ncr53cf96_write(c,4,0x04);
    ncr53cf96_write(c,16,0xff); /* outside chip register slots is ignored */
    assert(ncr53cf96_read(c,8)==7);

    ncr53cf96_write(c,11,0x08); /* SCSI-2 features enable 24-bit TC */
    ncr53cf96_write(c,0,0x34); ncr53cf96_write(c,1,0x12);
    ncr53cf96_write(c,14,0x56);
    ncr53cf96_status_t s{}; ncr53cf96_get_status(c,&s);
    assert(s.transfer_count==0x561234);
    assert(s.config2==0x08);
    ncr53cf96_write(c,3,0x10); /* CI_XFER is illegal before selection. */
    assert(ncr53cf96_read(c,5)==0x40);
    assert(b.irq==0);
    ncr53cf96_destroy(c);
}

static void fifo_and_dma_count()
{
    BusProbe b;
    Ncr53cf96 *c=make_chip(b);
    assert(c);
    /* Configure 24-bit counter, then enter initiator mode via a scripted
     * target handshake in the selection test. FIFO itself is available in
     * every mode and has exact sixteen-byte capacity/underflow behavior. */
    for(unsigned i=0;i<16;i++) ncr53cf96_write(c,2,uint8_t(0xa0+i));
    ncr53cf96_write(c,2,0xff);
    ncr53cf96_status_t s{}; ncr53cf96_get_status(c,&s);
    assert(s.fifo_count==16);
    for(unsigned i=0;i<16;i++) assert(ncr53cf96_read(c,2)==0xa0+i);
    assert(ncr53cf96_read(c,2)==0);
    ncr53cf96_get_status(c,&s); assert(s.fifo_count==0);
    assert(ncr53cf96_dma16_read(c)==0xffaf); /* source dma_r underflows to stale FIFO[0] */
    ncr53cf96_write(c,15,0x03); /* upstream stores FIFO_ALIGN, no DMA consumer */
    ncr53cf96_dma16_write(c,0x1234);
    ncr53cf96_get_status(c,&s); assert(s.fifo_count==2);
    assert(ncr53cf96_dma16_read(c)==0x1234); /* little-endian FIFO ordering */
    ncr53cf96_get_status(c,&s); assert(s.fifo_count==0);
    ncr53cf96_destroy(c);
}

static void absent_target_times_out_without_ready()
{
    BusProbe b;
    Ncr53cf96 *c=make_chip(b);
    assert(c);
    ncr53cf96_status_t s{}; ncr53cf96_get_status(c,&s);
    assert(s.scsi_id==7); /* MAME NSCSI endpoint default, before register write */
    ncr53cf96_write(c,4,4); /* target ID 4 */
    ncr53cf96_write(c,5,0x55); /* active MAME DELAY_HACK ignores this value */
    ncr53cf96_write(c,3,0x41); /* SELECT, no DMA */
    ncr53cf96_get_status(c,&s);
    assert(s.timer_deadline==22);
    assert(b.host_data==(1u<<7));
    ncr53cf96_advance_to(c,22);
    assert(b.host_ctrl & NCR53CF96_S_SEL);
    ncr53cf96_advance_to(c,34);
    assert(b.host_data==((1u<<7)|(1u<<4)));
    ncr53cf96_advance_to(c,42);
    assert((b.host_ctrl & NCR53CF96_S_BSY)==0); /* initiator releases BSY */
    ncr53cf96_advance_to(c,44);
    ncr53cf96_advance_to(c,2044);
    ncr53cf96_get_status(c,&s);
    assert((s.interrupt_status & 0x20)!=0); /* disconnect after selection timeout */
    assert(s.irq && b.irq);
    assert((s.bus_ctrl & NCR53CF96_S_BSY)==0);
    assert(ncr53cf96_read(c,5)==0x20);
    assert(!b.irq);
    ncr53cf96_destroy(c);
}

static void arbitration_loser_is_reported_as_fault()
{
    BusProbe b;
    Ncr53cf96 *c=make_chip(b);
    assert(c);
    ncr53cf96_write(c,8,5); /* CONF write changes m_scsi_id in upstream */
    ncr53cf96_write(c,4,4);
    ncr53cf96_write(c,3,0x41);
    ncr53cf96_bus_changed(c,0,1u<<6); /* competing initiator 6 wins over ID 5 */
    ncr53cf96_advance_to(c,22);
    ncr53cf96_status_t s{}; ncr53cf96_get_status(c,&s);
    assert(s.scsi_id==5);
    assert(s.faults & NCR53CF96_FAULT_ARBITRATION_LOST);
    assert(s.interrupt_status==0x40 && s.irq); /* error, not clean disconnect */
    assert((s.interrupt_status&0x20)==0);
    ncr53cf96_destroy(c);
}

static void reset_retains_pending_timer_and_full_command_fifo_order()
{
    BusProbe b;
    Ncr53cf96 *c=make_chip(b);
    assert(c);
    ncr53cf96_write(c,8,7);
    ncr53cf96_write(c,4,4);
    ncr53cf96_write(c,3,0x41); /* active selection arms a timer */
    ncr53cf96_status_t s{}; ncr53cf96_get_status(c,&s);
    assert(s.timer_deadline==22);
    ncr53cf96_write(c,3,0x02); /* chip reset does not cancel upstream timer */
    ncr53cf96_get_status(c,&s);
    assert(s.timer_deadline==22);
    ncr53cf96_advance_to(c,22);
    ncr53cf96_get_status(c,&s);
    assert(!s.irq && s.interrupt_status==0);

    /* Reset commands only get prioritized after the two-entry command buffer
     * full check in pinned source. */
    ncr53cf96_reset(c);
    ncr53cf96_write(c,8,7);
    ncr53cf96_write(c,4,4);
    ncr53cf96_write(c,3,0x41); /* command[0] remains active */
    ncr53cf96_write(c,3,0x00); /* chained command[1] */
    ncr53cf96_write(c,3,0x03); /* RESET BUS is rejected as queue overflow */
    assert(ncr53cf96_read(c,3)==0x41);
    assert(ncr53cf96_read(c,4)&0x40); /* gross error */
    assert((b.host_ctrl & NCR53CF96_S_RST)==0);
    ncr53cf96_destroy(c);
}

static void reset_preserves_driven_data_and_test_mode_latch()
{
    BusProbe b;
    Ncr53cf96 *c=make_chip(b);
    assert(c);
    ncr53cf96_write(c,4,4);
    ncr53cf96_write(c,3,0x41); /* arbitration drives default endpoint ID 7 */
    assert(b.host_data==(1u<<7));
    ncr53cf96_write(c,3,0x02); /* device_reset/reset_disconnect don't data_w(0) */
    assert(b.host_data==(1u<<7));
    ncr53cf96_status_t s{}; ncr53cf96_get_status(c,&s);
    assert(s.bus_data==(1u<<7));

    ncr53cf96_write(c,8,0x0f); /* CONF bit 3 sets sticky test mode */
    ncr53cf96_write(c,8,0x07); /* clearing bit 3 in CONF does not clear test mode */
    ncr53cf96_write(c,10,0x55); /* upstream only logs this in test mode */
    ncr53cf96_get_status(c,&s); assert(s.test_mode);
    ncr53cf96_reset(c);
    ncr53cf96_get_status(c,&s); assert(!s.test_mode);
    ncr53cf96_destroy(c);
}

static void external_bus_reset_does_not_run_controller_step()
{
    BusProbe b;
    Ncr53cf96 *c=make_chip(b);
    assert(c);
    ncr53cf96_write(c,3,0x10); /* illegal command sets I_ILLEGAL */
    ncr53cf96_status_t before{}, after{};
    ncr53cf96_get_status(c,&before);
    assert(before.interrupt_status==0x40 && before.irq);
    ncr53cf96_bus_changed(c,NCR53CF96_S_RST,0);
    ncr53cf96_get_status(c,&after);
    assert(after.interrupt_status==before.interrupt_status && after.irq);
    assert(ncr53cf96_read(c,5)==0x40);
    ncr53cf96_bus_changed(c,0,0); /* deasserting RST can notify controller */
    ncr53cf96_destroy(c);
}

static void cycle_horizon_and_rewind_are_bounded_faults()
{
    BusProbe b;
    Ncr53cf96 *c=make_chip(b);
    assert(c);
    ncr53cf96_advance_to(c,UINT64_MAX-10);
    ncr53cf96_write(c,3,0x41); /* scheduling 22 cycles overflows horizon */
    ncr53cf96_status_t s{}; ncr53cf96_get_status(c,&s);
    assert(s.faults & NCR53CF96_FAULT_TIME_OVERFLOW);
    assert(s.timer_deadline==UINT64_MAX);
    ncr53cf96_advance_to(c,UINT64_MAX); /* idle sentinel must not be treated as an event */
    ncr53cf96_get_status(c,&s); assert(s.cycles==UINT64_MAX);
    ncr53cf96_advance_to(c,UINT64_MAX-1);
    ncr53cf96_get_status(c,&s);
    assert(s.faults & NCR53CF96_FAULT_TIME_REWIND);
    ncr53cf96_destroy(c);
}

static void selected_target_command_phase_and_dma_data_in()
{
    BusProbe b;
    Ncr53cf96 *c=make_chip(b);
    assert(c);
    ncr53cf96_write(c,8,7); /* initiator ID */
    ncr53cf96_write(c,4,4); /* target ID */
    ncr53cf96_write(c,2,0x12); /* one-byte CDB for this transport fixture */
    ncr53cf96_write(c,3,0x41); /* SELECT */
    ncr53cf96_advance_to(c,22);
    assert(b.host_ctrl & NCR53CF96_S_SEL);
    ncr53cf96_advance_to(c,34);
    ncr53cf96_advance_to(c,38);
    /* Scripted target notices selection, asserts BSY, command phase, and REQ.
     * This target exists only in the test; production has no fabricated target. */
    ncr53cf96_bus_changed(c, NCR53CF96_S_BSY|NCR53CF96_S_REQ|NCR53CF96_S_CTL, 0);
    ncr53cf96_advance_to(c,42);
    ncr53cf96_advance_to(c,44);
    assert(b.host_data==0x12);
    assert(b.host_ctrl & NCR53CF96_S_ACK);
    /* Target accepts the command byte, then changes to STATUS phase. */
    ncr53cf96_bus_changed(c,NCR53CF96_S_BSY|NCR53CF96_S_CTL,0);
    ncr53cf96_advance_to(c,49);
    assert((b.host_ctrl & NCR53CF96_S_ACK)==0);
    ncr53cf96_bus_changed(c,NCR53CF96_S_BSY|NCR53CF96_S_REQ|
        NCR53CF96_PHASE_STATUS,0);
    ncr53cf96_status_t s{}; ncr53cf96_get_status(c,&s);
    assert((s.interrupt_status & 0x18)==0x18); /* function + bus complete */
    assert(b.irq);
    assert(ncr53cf96_read(c,5)==0x18);

    /* Select a one-byte target-to-host DMA transfer while still in STATUS. */
    ncr53cf96_bus_changed(c,NCR53CF96_S_BSY|NCR53CF96_PHASE_STATUS,0);
    ncr53cf96_write(c,11,0x08); /* 24-bit transfer count */
    ncr53cf96_write(c,12,0x04); /* LAST BYTE TRANSFER mode: ignored on BUSMD_0 */
    ncr53cf96_write(c,0,1); ncr53cf96_write(c,1,0);
    ncr53cf96_write(c,14,0);
    ncr53cf96_write(c,3,0x90); /* DMA + initiator transfer */
    ncr53cf96_bus_changed(c,NCR53CF96_S_BSY|NCR53CF96_S_REQ|
        NCR53CF96_PHASE_STATUS,0);
    ncr53cf96_advance_to(c,54);
    ncr53cf96_get_status(c,&s);
    assert(s.fifo_count==1 && s.drq);
    assert(ncr53cf96_dma_read(c)==0);
    ncr53cf96_get_status(c,&s);
    assert(s.transfer_counter==0 && (s.status&0x10));
    assert(!s.drq);

    /* Complete status phase and begin one-byte data-out. The odd 16-bit DMA
     * case must transfer only the low byte when TC == 1. */
    ncr53cf96_bus_changed(c,NCR53CF96_S_BSY|NCR53CF96_PHASE_STATUS,0);
    ncr53cf96_bus_changed(c,NCR53CF96_S_BSY|NCR53CF96_S_REQ|
        NCR53CF96_PHASE_STATUS,0);
    ncr53cf96_get_status(c,&s);
    assert((s.interrupt_status&0x10)!=0);
    ncr53cf96_read(c,5);
    ncr53cf96_bus_changed(c,NCR53CF96_S_BSY|NCR53CF96_S_REQ|
        NCR53CF96_PHASE_DATA_OUT,0);
    ncr53cf96_write(c,0,1); ncr53cf96_write(c,1,0); ncr53cf96_write(c,14,0);
    ncr53cf96_write(c,3,0x90);
    ncr53cf96_dma16_write(c,0xbeef);
    assert(b.host_data==0xef);
    ncr53cf96_get_status(c,&s);
    assert(s.transfer_counter==0 && (s.status&0x10));
    ncr53cf96_destroy(c);
}

int main()
{
    registers_and_count();
    fifo_and_dma_count();
    absent_target_times_out_without_ready();
    arbitration_loser_is_reported_as_fault();
    reset_retains_pending_timer_and_full_command_fifo_order();
    reset_preserves_driven_data_and_test_mode_latch();
    external_bus_reset_does_not_run_controller_step();
    cycle_horizon_and_rewind_are_bounded_faults();
    selected_target_command_phase_and_dma_data_in();
    std::puts("NCR53CF96 register/FIFO/selection/phase/DMA tests: PASS");
}
