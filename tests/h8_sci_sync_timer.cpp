// Source contract tests for MAME's global H8 SCI sync timer.
#include "../libpcsxcore/arcade/h8.h"
#include "../libpcsxcore/arcade/h8_cycle_clock.h"
#include "../libpcsxcore/arcade/h8_sci.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using arcade::H8CycleClock;

struct Notice {
    unsigned channel;
    bool armed;
    uint64_t generation;
    H8Time expires;
};

struct World {
    H8CPU *cpu = nullptr;
    std::vector<Notice> notices;
    bool nest_on_channel_one = false;
    H8Time nested_time{0, 0};
};

static void scheduler_timer_changed(void *opaque, unsigned channel, int armed,
                                     uint64_t generation, uint64_t seconds,
                                     uint64_t attoseconds) {
    World &world = *static_cast<World *>(opaque);
    world.notices.push_back({channel, armed != 0, generation,
                             {seconds, attoseconds}});
    if (armed && channel == 1 && world.nest_on_channel_one) {
        world.nest_on_channel_one = false;
        h8_write_byte(world.cpu, 0xffffb2, 0x10); // SCI0 receiver enable
        h8_serial_rx_pin_at_time(world.cpu, 0, 0, world.nested_time);
    }
}

static bool equal_time(H8Time a, H8Time b) {
    return a.seconds == b.seconds && a.attoseconds == b.attoseconds;
}
static H8Time never_time() {
    return {UINT64_C(1000000000), 0};
}

static H8Time expected_expiry(H8Time now, uint64_t divider) {
    const H8CycleClock::Time source{now.seconds, now.attoseconds};
    const uint64_t ticks = H8CycleClock::asTicks(source);
    const uint64_t delay = (ticks / divider + 1) * divider - ticks;
    const H8CycleClock::Time expiry = H8CycleClock::addTime(source,
        H8CycleClock::fromTicks(delay));
    return {expiry.seconds, expiry.attoseconds};
}

static H8CPU *create_cpu(World &world) {
    H8Config config{};
    config.opaque = &world;
    config.scheduler_timer_changed = scheduler_timer_changed;
    world.cpu = h8_create(&config);
    assert(world.cpu);
    return world.cpu;
}

static void explicit_pin_time_and_nested_context() {
    World world;
    H8CPU *cpu = create_cpu(world);
    const H8Time outer{2, UINT64_C(123456789012345)};
    const H8Time nested{5, UINT64_C(987654321012345678)};
    world.nest_on_channel_one = true;
    world.nested_time = nested;
    h8_write_byte(cpu, 0xffffba, 0x10); // SCI1 receiver enable
    h8_serial_rx_pin_at_time(cpu, 1, 0, outer);

    H8SyncTimer sci1{}, sci0{};
    assert(h8_get_sync_timer(cpu, 1, &sci1) == 0 && sci1.armed);
    assert(h8_get_sync_timer(cpu, 0, &sci0) == 0 && sci0.armed);
    assert(equal_time(sci1.expires, expected_expiry(outer, 512)));
    assert(equal_time(sci0.expires, expected_expiry(nested, 512)));
    assert(sci1.generation == 1 && sci0.generation == 1);

    unsigned next_channel = 99;
    H8SyncTimer next{};
    assert(h8_peek_next_sync_timer(cpu, &next_channel, &next) == 0);
    assert(next_channel == 1 && next.generation == sci1.generation);
    assert(equal_time(next.expires, sci1.expires));

    // The stale generation must not consume a replaced or still-pending event.
    assert(h8_fire_sync_timer(cpu, 1, sci1.generation + 1) == 0);
    assert(h8_get_sync_timer(cpu, 1, &next) == 0 && next.armed);
    const uint8_t status_before_fire = h8_read_byte(cpu, 0xffffbc);
    const uint64_t cycle_before_fire = h8_current_cycle(cpu);
    assert(h8_fire_sync_timer(cpu, 1, sci1.generation) == 1);
    assert(h8_current_cycle(cpu) == cycle_before_fire);
    assert(h8_get_sync_timer(cpu, 1, &next) == 0 && !next.armed);
    assert(equal_time(next.expires, never_time()));
    assert(h8_read_byte(cpu, 0xffffbc) == status_before_fire);
    assert(h8_fire_sync_timer(cpu, 1, sci1.generation) == 0);

    // Rearming replaces the old event and advances the generation. A queued
    // callback carrying the previous generation cannot disarm the new event.
    h8_write_byte(cpu, 0xffffba, 0x00);
    h8_serial_rx_pin_at_time(cpu, 1, 1, {8, 0});
    h8_write_byte(cpu, 0xffffba, 0x10);
    h8_serial_rx_pin_at_time(cpu, 1, 0, {9, UINT64_C(111000000000000)});
    H8SyncTimer replacement{};
    assert(h8_get_sync_timer(cpu, 1, &replacement) == 0 && replacement.armed);
    assert(replacement.generation > sci1.generation);
    assert(h8_fire_sync_timer(cpu, 1, sci1.generation) == 0);
    assert(h8_get_sync_timer(cpu, 1, &next) == 0 && next.armed &&
           next.generation == replacement.generation);
    h8_destroy(cpu);
}

static void stop_and_soft_reset_keep_armed_expiry() {
    World world;
    H8CPU *cpu = create_cpu(world);
    const H8Time start{7, UINT64_C(123400000000000)};
    h8_write_byte(cpu, 0xffffba, 0x10);
    h8_serial_rx_pin_at_time(cpu, 1, 0, start);
    H8SyncTimer before{};
    assert(h8_get_sync_timer(cpu, 1, &before) == 0 && before.armed);

    // Source clock_stop resets internal state but does not cancel m_sync_timer.
    h8_write_byte(cpu, 0xffffba, 0x00);
    H8SyncTimer after_stop{};
    assert(h8_get_sync_timer(cpu, 1, &after_stop) == 0 && after_stop.armed);
    assert(after_stop.generation == before.generation);
    assert(equal_time(after_stop.expires, before.expires));

    // Source device_reset also leaves its independent global timer untouched.
    assert(h8_soft_reset(cpu, 0) == 0);
    H8SyncTimer after_reset{};
    assert(h8_get_sync_timer(cpu, 1, &after_reset) == 0 && after_reset.armed);
    assert(after_reset.generation == before.generation);
    assert(equal_time(after_reset.expires, before.expires));
    assert(h8_fire_sync_timer(cpu, 1, before.generation) == 1);
    h8_destroy(cpu);
}

static void cold_reset_invalidates_old_generation() {
    World world;
    H8CPU *cpu = create_cpu(world);
    h8_write_byte(cpu, 0xffffba, 0x10);
    h8_serial_rx_pin_at_time(cpu, 1, 0, {3, UINT64_C(800000000000000000)});
    H8SyncTimer old{};
    assert(h8_get_sync_timer(cpu, 1, &old) == 0 && old.armed);

    h8_reset(cpu);
    H8SyncTimer reset{};
    assert(h8_get_sync_timer(cpu, 1, &reset) == 0 && !reset.armed);
    assert(equal_time(reset.expires, never_time()));
    assert(reset.generation > old.generation);
    assert(h8_fire_sync_timer(cpu, 1, old.generation) == 0);
    assert(!world.notices.empty());
    assert(!world.notices.back().armed && world.notices.back().channel == 1);
    h8_destroy(cpu);
}

struct SciClock {
    uint64_t tx_now = 0;
    uint64_t rx_now = 0;
    unsigned adjustments = 0;
    uint64_t last_delay = 0;
    unsigned channel = 99;
};

static uint64_t tx_now(void *opaque) {
    return static_cast<SciClock *>(opaque)->tx_now;
}
static uint64_t rx_now(void *opaque) {
    return static_cast<SciClock *>(opaque)->rx_now;
}
static void timer_adjust(void *opaque, unsigned channel, uint64_t delay) {
    SciClock &clock = *static_cast<SciClock *>(opaque);
    ++clock.adjustments;
    clock.channel = channel;
    clock.last_delay = delay;
}

static void internal_rate_and_external_rate_timer_lifecycle() {
    SciClock clock;
    arcade::H8Sci sci;
    sci.setCallbacks(&clock, tx_now, nullptr, nullptr, nullptr, 56, 1,
                     nullptr, nullptr, rx_now, timer_adjust);
    sci.setExternalClockPeriodAS(0, H8CycleClock::frequency);
    sci.reset();
    clock.adjustments = 0;
    sci.write(1, 0); // Divider 2
    sci.write(2, 0x10); // Receiver enable
    clock.rx_now = 1003;
    sci.rx_w(0); // Physical start edge
    assert(clock.adjustments == 1 && clock.channel == 1);
    assert(clock.last_delay == 1 && sci.next_event() == 1004);

    clock.rx_now = 1004;
    sci.update(1004);
    assert(clock.adjustments == 2);
    assert(clock.last_delay == 2 && sci.next_event() == 1006);

    // Source internal_update keeps the clock deadline but does not adjust
    // the global timer when that deadline is behind now_as_cycles().
    clock.rx_now = 2000;
    sci.update(1006);
    assert(sci.next_event() == 1008 && clock.adjustments == 2);

    const unsigned before_stop = clock.adjustments;
    sci.write(2, 0);
    assert(clock.adjustments == before_stop);
    const unsigned before_reset = clock.adjustments;
    sci.reset();
    assert(clock.adjustments == before_reset);

    // A physical external clock has no scheduler timer; clock edges arrive
    // directly through clk_w().
    sci.setExternalClockPeriodAS(0, H8CycleClock::frequency);
    sci.write(1, 0);
    sci.write(2, 0x12);
    sci.rx_w(1);
    sci.rx_w(0);
    assert(clock.adjustments == before_reset);
    sci.reset();

    // Simulated external-rate clock uses the same global timer adjustment
    // contract; a physical external-clock mode does not schedule one.
    sci.setExternalClockPeriodAS(UINT64_C(542534722222), H8CycleClock::frequency);
    sci.write(1, 0);
    sci.write(2, 0x12); // RE + CKE1: external-rate async receiver
    clock.rx_now = 2000;
    sci.rx_w(1);
    sci.rx_w(0);
    assert(clock.adjustments == before_reset + 1);
    const uint64_t first_event = sci.next_event();
    clock.rx_now = first_event;
    sci.update(first_event);
    assert(clock.adjustments == before_reset + 2);
    assert(sci.next_event() > first_event);
    const unsigned before_external_stop = clock.adjustments;
    sci.write(2, 0);
    assert(clock.adjustments == before_external_stop);
}

static void transmit_start_uses_tx_clock_and_adjusts_global_timer() {
    SciClock clock;
    arcade::H8Sci sci;
    sci.setCallbacks(&clock, tx_now, nullptr, nullptr, nullptr, 56, 1,
                     nullptr, nullptr, rx_now, timer_adjust);
    sci.setExternalClockPeriodAS(0, H8CycleClock::frequency);
    sci.reset();
    clock.adjustments = 0;
    clock.tx_now = 301;
    sci.write(1, 0); // Divider 2
    sci.write(2, 0x20); // Transmitter enable
    sci.write(3, 0x5a);
    const uint8_t status = sci.read(4);
    sci.write(4, static_cast<uint8_t>(status & ~0x80));
    assert(clock.adjustments == 1 && clock.channel == 1);
    assert(clock.last_delay == 1 && sci.next_event() == 302);
}

int main() {
    explicit_pin_time_and_nested_context();
    stop_and_soft_reset_keep_armed_expiry();
    cold_reset_invalidates_old_generation();
    internal_rate_and_external_rate_timer_lifecycle();
    transmit_start_uses_tx_clock_and_adjusts_global_timer();
    std::puts("H8 SCI sync-timer tests passed");
}
