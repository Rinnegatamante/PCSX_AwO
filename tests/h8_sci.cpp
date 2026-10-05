/* SPDX-License-Identifier: BSD-3-Clause */
#include "../libpcsxcore/arcade/h8_sci.h"
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

struct PinEvent { uint64_t cycle; char pin; int level; };
struct TestBus {
    uint64_t cycle;
    std::vector<PinEvent> pins;
    std::vector<unsigned> irqs;
    std::vector<std::pair<uint64_t, uint8_t> > frames;
    unsigned reschedules;
    TestBus() : cycle(0), pins(), irqs(), frames(), reschedules(0) {}
};

static uint64_t now_cb(void *opaque) { return static_cast<TestBus *>(opaque)->cycle; }
static void tx_cb(void *opaque, int level) {
    TestBus &bus = *static_cast<TestBus *>(opaque);
    bus.pins.push_back(PinEvent{bus.cycle, 't', level});
}
static void clk_cb(void *opaque, int level) {
    TestBus &bus = *static_cast<TestBus *>(opaque);
    bus.pins.push_back(PinEvent{bus.cycle, 'c', level});
}
static void irq_cb(void *opaque, unsigned vector) {
    static_cast<TestBus *>(opaque)->irqs.push_back(vector);
}
static void frame_cb(void *opaque, unsigned channel, uint8_t byte) {
    TestBus &bus = *static_cast<TestBus *>(opaque);
    assert(channel == 1);
    bus.frames.push_back(std::make_pair(bus.cycle, byte));
}
static void reschedule_cb(void *opaque) {
    ++static_cast<TestBus *>(opaque)->reschedules;
}

static arcade::H8Sci make_sci(TestBus &bus, unsigned vector_base = 52,
                              bool pins = true) {
    arcade::H8Sci sci;
    sci.setCallbacks(&bus, now_cb, pins ? tx_cb : 0, pins ? clk_cb : 0,
                     irq_cb, vector_base, 1,
                     frame_cb, reschedule_cb);
    sci.reset();
    bus.pins.clear();
    bus.irqs.clear();
    bus.frames.clear();
    bus.reschedules = 0;
    return sci;
}

static void start_transmit(arcade::H8Sci &sci, TestBus &bus, uint8_t value,
                           uint8_t scr, uint8_t smr = 0, uint8_t brr = 0) {
    sci.write(0, smr);
    sci.write(1, brr);
    sci.write(2, scr);
    sci.write(3, value);
    const uint8_t status = sci.read(4);
    sci.write(4, static_cast<uint8_t>(status & ~0x80));
    assert(sci.next_event() || (scr & 0x02));
    (void)bus;
}

static void run_until(arcade::H8Sci &sci, TestBus &bus, uint64_t target) {
    for (;;) {
        const uint64_t event = sci.next_event();
        if (!event || event > target)
            break;
        bus.cycle = event;
        sci.update(event);
    }
    bus.cycle = target;
}

static void register_reset_and_write_zero_clear() {
    TestBus bus;
    arcade::H8Sci sci = make_sci(bus, 27);
    assert(sci.read(0) == 0 && sci.read(1) == 0xff && sci.read(2) == 0);
    assert(sci.read(3) == 0xff && sci.read(5) == 0);
    assert(bus.pins.size() == 0);

    // TDRE clears only after SSR was read, and only while TE is set.
    sci.write(2, 0xa0);
    sci.write(1, 0);
    sci.write(3, 0x5a);
    sci.write(4, 0x04); // No preceding SSR read: source preserves TDRE.
    assert(sci.next_event() == 0);
    assert(sci.read(4) & 0x80);
    sci.write(4, 0x04);
    assert(sci.next_event() == 2);
    assert(!bus.irqs.empty() && bus.irqs.back() == 29); // TXI = base + 2
}

static void reset_preserves_source_read_latches() {
    TestBus txbus;
    arcade::H8Sci tx = make_sci(txbus);
    (void)tx.read(4); // MAME deliberately preserves m_ssr_read over reset.
    tx.reset();
    tx.write(2, 0x20);
    tx.write(1, 0);
    tx.write(3, 0x7e);
    tx.write(4, 0x04); // No post-reset SSR read; old read latch permits clear.
    assert(tx.next_event() == 2);

    TestBus rxbus;
    arcade::H8Sci rx = make_sci(rxbus);
    rx.rx_w(0); // Store a low pin while receiver is disabled.
    rx.reset(); // MAME reset does not restore m_rx_value.
    rx.write(1, 0);
    rx.write(2, 0x10); // Enable receiver while line is already low.
    rx.rx_w(0);
    assert(rx.next_event() == 0); // No new falling edge, so no frame start.
}

static void internal_async_transmit_pin_timing_and_te_irq() {
    TestBus bus;
    arcade::H8Sci sci = make_sci(bus);
    start_transmit(sci, bus, 0xa5, 0xa4); // TE + TIE + TEIE, internal async
    assert(sci.next_event() == 2);
    assert(bus.reschedules >= 1);
    run_until(sci, bus, 322);
    assert(sci.next_event() == 0);
    assert(sci.read(4) & 0x84); // TDRE and TEND

    std::vector<PinEvent> tx;
    for (size_t i = 0; i < bus.pins.size(); ++i)
        if (bus.pins[i].pin == 't') tx.push_back(bus.pins[i]);
    assert(tx.size() == 11);
    assert(tx[0].cycle == 2 && tx[0].level == 0); // Start bit
    assert(tx[1].cycle == 34 && tx[1].level == 1); // Data bit 0 of 0xa5
    assert(tx[2].cycle == 66 && tx[2].level == 0);
    assert(tx[9].cycle == 290 && tx[9].level == 1); // Stop bit
    assert(tx[10].cycle == 322 && tx[10].level == 1); // End of stop frame
    assert(bus.frames.empty()); // Frame callback is the no-pin fallback.
    assert(bus.irqs.size() >= 2);
    assert(bus.irqs[0] == 54); // TXI at immediate start
    assert(bus.irqs.back() == 55); // TEI when physical frame completes
    assert(bus.reschedules >= 2);
}

static void legacy_frame_fallback_at_physical_completion() {
    TestBus bus;
    arcade::H8Sci sci = make_sci(bus, 52, false);
    start_transmit(sci, bus, 0x3c, 0xa4);
    run_until(sci, bus, 321);
    assert(bus.frames.empty());
    run_until(sci, bus, 322);
    assert(bus.frames.size() == 1 && bus.frames[0].first == 322 &&
           bus.frames[0].second == 0x3c);
}

static void internal_async_clock_output() {
    TestBus bus;
    arcade::H8Sci sci = make_sci(bus);
    start_transmit(sci, bus, 0x00, 0xa5); // TE+TIE+TEIE + CKE0 output
    run_until(sci, bus, 36);
    std::vector<PinEvent> clk;
    for (size_t i = 0; i < bus.pins.size(); ++i)
        if (bus.pins[i].pin == 'c') clk.push_back(bus.pins[i]);
    assert(clk.size() >= 3);
    assert(clk[0].cycle == 2 && clk[0].level == 0);
    assert(clk[1].cycle == 18 && clk[1].level == 1);
    assert(clk[2].cycle == 34 && clk[2].level == 0);
}

static void external_async_clock_edges() {
    TestBus bus;
    arcade::H8Sci sci = make_sci(bus);
    start_transmit(sci, bus, 0x01, 0xa6); // TE+TIE+TEIE+CKE1
    assert(sci.next_event() == 0); // Wait for caller-driven clock edges.
    for (unsigned edge = 1; edge <= 16; ++edge) {
        bus.cycle = edge;
        sci.clk_w((edge & 1) ? 0 : 1);
    }
    assert(bus.pins.size() == 1 && bus.pins[0].pin == 't' &&
           bus.pins[0].cycle == 1 && bus.pins[0].level == 0);
    for (unsigned edge = 17; edge <= 32; ++edge) {
        bus.cycle = edge;
        sci.clk_w((edge & 1) ? 0 : 1);
    }
    assert(bus.pins.size() == 2 && bus.pins[1].cycle == 17 &&
           bus.pins[1].level == 1);
}

static void external_rate_async_clock_mapping() {
    TestBus bus;
    arcade::H8Sci sci = make_sci(bus);
    sci.setExternalClockPeriodAS(542534722222ULL, 16000000ULL);
    start_transmit(sci, bus, 0x00, 0xa6);
    // MAME's split double conversion rounds the first two external edges to
    // H8 cycles 9 and 18 for this board clock/period pair.
    assert(sci.next_event() == 9);
    bus.cycle = 9;
    assert(sci.update(9) == 18);
    assert(bus.pins.size() == 1 && bus.pins[0].cycle == 9 &&
           bus.pins[0].level == 0);
}

static void internal_sync_transmit_and_receive() {
    TestBus bus;
    arcade::H8Sci sci = make_sci(bus);
    start_transmit(sci, bus, 0x96, 0xa4, 0x80); // synchronous, output clock
    run_until(sci, bus, 34);
    assert(sci.next_event() == 0);
    assert(bus.frames.empty());
    assert(bus.pins[0].cycle == 2 && bus.pins[0].pin == 't');
    assert(sci.read(4) & 0x84);
    assert(bus.irqs.back() == 55);

    // Synchronous receive samples eight data bits on alternate clock edges.
    TestBus rxbus;
    arcade::H8Sci rx = make_sci(rxbus, 52);
    rx.write(0, 0x80);
    rx.write(1, 0);
    rx.write(2, 0x50); // RE + RIE; MAME starts sync receive when SMR is set.
    const uint8_t incoming = 0x69;
    for (unsigned bit = 0; bit < 8; ++bit) {
        rx.rx_w((incoming >> bit) & 1);
        run_until(rx, rxbus, 4 + bit * 4);
    }
    assert(rx.read(5) == incoming);
    assert(rx.read(4) & 0x40);
    assert(!rxbus.irqs.empty() && rxbus.irqs.back() == 53); // RXI = base+1
    const uint8_t status = rx.read(4);
    rx.write(4, static_cast<uint8_t>(status & ~0x40));
    assert(!(rx.read(4) & 0x40)); // RDRF clears after status read + write zero.
    assert(rx.read(5) == incoming); // Ordinary CPU RDR read has no side effect.
}

static void async_receive_and_framing_error() {
    TestBus bus;
    arcade::H8Sci sci = make_sci(bus, 56);
    sci.write(0, 0);
    sci.write(1, 0);
    sci.write(2, 0x50); // RE + RIE
    bus.cycle = 0;
    sci.rx_w(0); // Falling edge starts; sample the start bit at cycle 18.
    run_until(sci, bus, 18);
    const uint8_t incoming = 0x53;
    for (unsigned bit = 0; bit < 8; ++bit) {
        const uint64_t sample_cycle = 50 + bit * 32;
        const uint64_t edge_cycle = sample_cycle - 16;
        run_until(sci, bus, edge_cycle);
        sci.rx_w((incoming >> bit) & 1);
        run_until(sci, bus, sample_cycle);
    }
    run_until(sci, bus, 290); // First stop-bit sample.
    sci.rx_w(1);
    run_until(sci, bus, 306);
    assert(sci.read(5) == incoming);
    assert((sci.read(4) & 0x40) != 0);
    assert(!bus.irqs.empty() && bus.irqs.back() == 57); // RXI = base+1
    const uint8_t status = sci.read(4);
    sci.write(4, static_cast<uint8_t>(status & ~0x40));
    assert(!(sci.read(4) & 0x40));

    // A second frame with a low stop bit reports FER through ERI.
    (void)sci.read(4);
    sci.write(4, static_cast<uint8_t>(sci.read(4) & ~0x40));
    bus.cycle = 400;
    sci.rx_w(0);
    run_until(sci, bus, 418);
    for (unsigned bit = 0; bit < 8; ++bit) {
        const uint64_t sample_cycle = 450 + bit * 32;
        run_until(sci, bus, sample_cycle - 16);
        sci.rx_w((incoming >> bit) & 1);
        run_until(sci, bus, sample_cycle);
    }
    run_until(sci, bus, 690);
    sci.rx_w(0);
    run_until(sci, bus, 706);
    assert(sci.read(4) & 0x10);
    assert(bus.irqs.back() == 56); // ERI = base+0
}

int main() {
    register_reset_and_write_zero_clear();
    reset_preserves_source_read_latches();
    internal_async_transmit_pin_timing_and_te_irq();
    legacy_frame_fallback_at_physical_completion();
    internal_async_clock_output();
    external_async_clock_edges();
    external_rate_async_clock_mapping();
    internal_sync_transmit_and_receive();
    async_receive_and_framing_error();
    std::puts("H8 SCI pin/timing tests passed");
    return 0;
}
