#include "../libpcsxcore/arcade/psx_screen_clock.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int compare(psx_screen_time a, psx_screen_time b)
{
	if (a.seconds != b.seconds)
		return a.seconds < b.seconds ? -1 : 1;
	if (a.attoseconds != b.attoseconds)
		return a.attoseconds < b.attoseconds ? -1 : 1;
	return 0;
}

static psx_screen_time add(psx_screen_time t, uint64_t attoseconds)
{
	t.attoseconds += attoseconds;
	if (t.attoseconds >= PSX_SCREEN_ATTOSECONDS_PER_SECOND) {
		t.attoseconds -= PSX_SCREEN_ATTOSECONDS_PER_SECOND;
		t.seconds++;
	}
	return t;
}

static void expect_event(psx_screen_clock *clock, psx_screen_time target,
		psx_screen_event_type type, psx_screen_time when)
{
	psx_screen_event event;
	assert(psx_screen_clock_advance_to(clock, target, &event));
	assert(event.type == type);
	assert(compare(event.time, when) == 0);
}

static void test_cold_first_period_and_initial_blank(void)
{
	psx_screen_clock clock;
	psx_screen_event event;
	psx_screen_time zero = { 0, 0 };
	psx_screen_time first = { 0, PSX_SCREEN_NTSC_240_PERIOD };
	psx_screen_clock_init(&clock);
	assert(compare(clock.last_start, zero) == 0);
	assert(clock.last_end.seconds == 0);
	assert(clock.last_end.attoseconds == PSX_SCREEN_VBLANK_ATTOSECONDS);
	assert(!clock.end_pending);
	assert(psx_screen_clock_is_vblank(&clock, zero));
	assert(!psx_screen_clock_configure(&clock, zero,
			PSX_SCREEN_NTSC_240_PERIOD, &event));
	assert(event.type == PSX_SCREEN_EVENT_NONE);
	assert(psx_screen_clock_next_deadline(&clock, &event));
	assert(event.type == PSX_SCREEN_EVENT_VBLANK_START);
	assert(compare(event.time, first) == 0);
	assert(!psx_screen_clock_advance_to(&clock,
			(psx_screen_time){ 0, PSX_SCREEN_NTSC_240_PERIOD - 1 }, &event));
	expect_event(&clock, first, PSX_SCREEN_EVENT_VBLANK_START, first);
	assert(clock.in_vblank && clock.end_pending);
	assert(compare(clock.last_end, add(first, PSX_SCREEN_VBLANK_ATTOSECONDS)) == 0);
	assert(psx_screen_clock_is_vblank(&clock, first));
	assert(!psx_screen_clock_is_vblank(&clock, clock.last_end));
}

static void test_continuous_fractional_phase(void)
{
	psx_screen_clock clock;
	psx_screen_event event;
	psx_screen_time expected = { 0, PSX_SCREEN_NTSC_240_PERIOD };
	unsigned i;
	psx_screen_clock_init(&clock);
	assert(!psx_screen_clock_configure(&clock, (psx_screen_time){ 0, 0 },
			PSX_SCREEN_NTSC_240_PERIOD, &event));
	for (i = 0; i < 1000; i++) {
		psx_screen_time end = add(expected, PSX_SCREEN_VBLANK_ATTOSECONDS);
		expect_event(&clock, expected, PSX_SCREEN_EVENT_VBLANK_START, expected);
		expect_event(&clock, end, PSX_SCREEN_EVENT_VBLANK_END, end);
		expected = add(expected, PSX_SCREEN_NTSC_240_PERIOD);
		assert(psx_screen_clock_next_deadline(&clock, &event));
		assert(event.type == PSX_SCREEN_EVENT_VBLANK_START);
		assert(compare(event.time, expected) == 0);
	}
}

static void test_mode_change_retains_end_and_reschedules_start(void)
{
	psx_screen_clock clock;
	psx_screen_event event;
	psx_screen_time first = { 0, PSX_SCREEN_NTSC_240_PERIOD };
	psx_screen_time old_end = add(first, PSX_SCREEN_VBLANK_ATTOSECONDS);
	psx_screen_time change = add(first, UINT64_C(1000000000000000));
	psx_screen_time next_start = add(first, PSX_SCREEN_NTSC_480_PERIOD);
	psx_screen_clock_init(&clock);
	assert(!psx_screen_clock_configure(&clock, (psx_screen_time){ 0, 0 },
			PSX_SCREEN_NTSC_240_PERIOD, &event));
	expect_event(&clock, first, PSX_SCREEN_EVENT_VBLANK_START, first);
	assert(!psx_screen_clock_configure(&clock, change,
			PSX_SCREEN_NTSC_480_PERIOD, &event));
	assert(psx_screen_clock_next_deadline(&clock, &event));
	assert(event.type == PSX_SCREEN_EVENT_VBLANK_END);
	assert(compare(event.time, old_end) == 0);
	expect_event(&clock, old_end, PSX_SCREEN_EVENT_VBLANK_END, old_end);
	assert(psx_screen_clock_next_deadline(&clock, &event));
	assert(event.type == PSX_SCREEN_EVENT_VBLANK_START);
	assert(compare(event.time, next_start) == 0);
	expect_event(&clock, next_start, PSX_SCREEN_EVENT_VBLANK_START, next_start);
}

static void test_mode_change_immediate_start_is_not_backdated(void)
{
	psx_screen_clock clock;
	psx_screen_event event;
	psx_screen_time first = { 0, PSX_SCREEN_NTSC_240_PERIOD };
	psx_screen_time now = add(first, UINT64_C(1500000000000000));
	psx_screen_time expected_end = add(now, PSX_SCREEN_VBLANK_ATTOSECONDS);
	uint64_t short_period = UINT64_C(1000000000000000);
	psx_screen_clock_init(&clock);
	assert(!psx_screen_clock_configure(&clock, (psx_screen_time){ 0, 0 },
			PSX_SCREEN_NTSC_240_PERIOD, &event));
	expect_event(&clock, first, PSX_SCREEN_EVENT_VBLANK_START, first);
	assert(psx_screen_clock_is_vblank(&clock, now));
	assert(psx_screen_clock_configure(&clock, now, short_period, &event));
	assert(event.type == PSX_SCREEN_EVENT_VBLANK_START);
	assert(compare(event.time, now) == 0);
	assert(compare(clock.last_start, now) == 0);
	assert(compare(clock.last_end, expected_end) == 0);
	assert(psx_screen_clock_next_deadline(&clock, &event));
	assert(event.type == PSX_SCREEN_EVENT_VBLANK_START);
	assert(compare(event.time, add(now, short_period)) == 0);
}

static void test_seconds_rollover_and_absolute_cycle_ceil(void)
{
	psx_screen_clock clock;
	psx_screen_event event;
	unsigned i;
	psx_screen_clock_init(&clock);
	assert(!psx_screen_clock_configure(&clock, (psx_screen_time){ 0, 0 },
			PSX_SCREEN_PAL_PERIOD, &event));
	for (i = 0; i < 50; i++) {
		psx_screen_event next;
		assert(psx_screen_clock_next_deadline(&clock, &next));
		assert(next.type == PSX_SCREEN_EVENT_VBLANK_START);
		expect_event(&clock, next.time, PSX_SCREEN_EVENT_VBLANK_START, next.time);
		if (i != 49) {
			assert(psx_screen_clock_next_deadline(&clock, &next));
			assert(next.type == PSX_SCREEN_EVENT_VBLANK_END);
			expect_event(&clock, next.time, PSX_SCREEN_EVENT_VBLANK_END, next.time);
		}
	}
	assert(clock.last_start.seconds == 1);
	assert(clock.last_start.attoseconds == 0);
	assert(psx_screen_time_to_cycles_ceil((psx_screen_time){ 0, 1 }, UINT64_C(50000000)) == 1);
	assert(psx_screen_time_to_cycles_ceil((psx_screen_time){ 0, 20 }, UINT64_C(50000000)) == 1);
	assert(psx_screen_time_to_cycles_ceil((psx_screen_time){ 100, 0 }, UINT64_C(50000000)) == UINT64_C(5000000000));
}

static void test_soft_reset_does_not_rephase_clock(void)
{
	psx_screen_clock clock;
	psx_screen_event event;
	psx_screen_time first = { 0, PSX_SCREEN_NTSC_480_PERIOD };
	psx_screen_clock_init(&clock);
	assert(!psx_screen_clock_configure(&clock, (psx_screen_time){ 0, 0 },
			PSX_SCREEN_NTSC_480_PERIOD, &event));
	expect_event(&clock, first, PSX_SCREEN_EVENT_VBLANK_START, first);
	/* A PSX soft reset changes display registers, not this independent clock. */
	assert(psx_screen_clock_next_deadline(&clock, &event));
	assert(event.type == PSX_SCREEN_EVENT_VBLANK_END);
	assert(compare(event.time, add(first, PSX_SCREEN_VBLANK_ATTOSECONDS)) == 0);
}

int main(void)
{
	test_cold_first_period_and_initial_blank();
	test_continuous_fractional_phase();
	test_mode_change_retains_end_and_reschedules_start();
	test_mode_change_immediate_start_is_not_backdated();
	test_seconds_rollover_and_absolute_cycle_ceil();
	test_soft_reset_does_not_rephase_clock();
	puts("psx screen clock tests passed");
	return 0;
}
