/* license:BSD-3-Clause
 * Screen event timing follows MAME screen_device::configure/vblank_begin.
 * Copyright Aaron Giles.
 */
#include "psx_screen_clock.h"

#include <limits.h>

static int time_compare(psx_screen_time a, psx_screen_time b)
{
	if (a.seconds < b.seconds)
		return -1;
	if (a.seconds > b.seconds)
		return 1;
	if (a.attoseconds < b.attoseconds)
		return -1;
	if (a.attoseconds > b.attoseconds)
		return 1;
	return 0;
}

static psx_screen_time time_add(psx_screen_time t, uint64_t attoseconds)
{
	uint64_t whole_seconds = attoseconds / PSX_SCREEN_ATTOSECONDS_PER_SECOND;
	uint64_t fraction = attoseconds % PSX_SCREEN_ATTOSECONDS_PER_SECOND;
	t.attoseconds += fraction;
	if (t.attoseconds >= PSX_SCREEN_ATTOSECONDS_PER_SECOND) {
		t.attoseconds -= PSX_SCREEN_ATTOSECONDS_PER_SECOND;
		whole_seconds++;
	}
	if (whole_seconds > (uint64_t)INT64_MAX ||
			t.seconds > INT64_MAX - (int64_t)whole_seconds)
		t.seconds = INT64_MAX;
	else
		t.seconds += (int64_t)whole_seconds;
	return t;
}

static uint64_t time_delta_attoseconds(psx_screen_time later,
		psx_screen_time earlier)
{
	uint64_t seconds;
	uint64_t attoseconds;
	if (time_compare(later, earlier) < 0)
		return 0;
	seconds = (uint64_t)(later.seconds - earlier.seconds);
	attoseconds = later.attoseconds;
	if (later.attoseconds < earlier.attoseconds) {
		seconds--;
		attoseconds += PSX_SCREEN_ATTOSECONDS_PER_SECOND;
	}
	attoseconds -= earlier.attoseconds;
	if (seconds > UINT64_MAX / PSX_SCREEN_ATTOSECONDS_PER_SECOND)
		return UINT64_MAX;
	seconds *= PSX_SCREEN_ATTOSECONDS_PER_SECOND;
	if (UINT64_MAX - seconds < attoseconds)
		return UINT64_MAX;
	return seconds + attoseconds;
}

static psx_screen_event make_event(psx_screen_event_type type,
		psx_screen_time time)
{
	psx_screen_event event;
	event.type = type;
	event.time = time;
	return event;
}

static void start_vblank(psx_screen_clock *clock, psx_screen_time time)
{
	clock->now = time;
	clock->last_start = time;
	clock->last_end = time_add(time, PSX_SCREEN_VBLANK_ATTOSECONDS);
	clock->next_end = clock->last_end;
	clock->end_pending = 1;
	clock->in_vblank = 1;
	clock->next_start = time_add(time, clock->frame_period);
	clock->start_pending = 1;
}

void psx_screen_clock_init(psx_screen_clock *clock)
{
	psx_screen_time zero = { 0, 0 };
	clock->now = zero;
	clock->last_start = zero;
	clock->last_end = (psx_screen_time){ 0, PSX_SCREEN_VBLANK_ATTOSECONDS };
	clock->next_start = zero;
	clock->next_end = zero;
	clock->frame_period = 0;
	clock->configured = 0;
	clock->in_vblank = 0;
	clock->start_pending = 0;
	clock->end_pending = 0;
}

int psx_screen_clock_configure(psx_screen_clock *clock, psx_screen_time now,
		uint64_t frame_period, psx_screen_event *event)
{
	uint64_t elapsed;
	if (!clock || !event || !frame_period ||
			frame_period >= PSX_SCREEN_ATTOSECONDS_PER_SECOND)
		return 0;
	clock->now = now;
	clock->frame_period = frame_period;
	clock->configured = 1;
	event->type = PSX_SCREEN_EVENT_NONE;
	event->time = now;

	/* First configure establishes the cold phase at t=0. */
	if (clock->last_start.seconds == 0 && clock->last_start.attoseconds == 0 &&
			!clock->start_pending && !clock->end_pending && !clock->in_vblank) {
		if (time_compare(now, (psx_screen_time){ 0, frame_period }) < 0) {
			clock->next_start = (psx_screen_time){ 0, frame_period };
			clock->start_pending = 1;
			return 0;
		}
		start_vblank(clock, now);
		*event = make_event(PSX_SCREEN_EVENT_VBLANK_START, now);
		return 1;
	}

	/* MAME keeps the last edge as the origin when changing refresh rate. */
	elapsed = time_delta_attoseconds(now, clock->last_start);
	if (elapsed >= frame_period) {
		start_vblank(clock, now);
		*event = make_event(PSX_SCREEN_EVENT_VBLANK_START, now);
		return 1;
	}
	clock->next_start = time_add(clock->last_start, frame_period);
	clock->start_pending = 1;
	return 0;
}

int psx_screen_clock_next_deadline(const psx_screen_clock *clock,
		psx_screen_event *event)
{
	int choose_start;
	if (!clock || !event || (!clock->start_pending && !clock->end_pending))
		return 0;
	choose_start = clock->start_pending && (!clock->end_pending ||
			time_compare(clock->next_start, clock->next_end) < 0);
	if (choose_start)
		*event = make_event(PSX_SCREEN_EVENT_VBLANK_START, clock->next_start);
	else
		*event = make_event(PSX_SCREEN_EVENT_VBLANK_END, clock->next_end);
	return 1;
}

int psx_screen_clock_is_vblank(const psx_screen_clock *clock,
		psx_screen_time at_time)
{
	return clock && time_compare(at_time, clock->last_end) < 0;
}

int psx_screen_clock_advance_to(psx_screen_clock *clock,
		psx_screen_time target, psx_screen_event *event)
{
	int have_start;
	int have_end;
	int choose_start;
	if (!clock || !event || !clock->configured || time_compare(target, clock->now) < 0)
		return 0;
	have_start = clock->start_pending && time_compare(clock->next_start, target) <= 0;
	have_end = clock->end_pending && time_compare(clock->next_end, target) <= 0;
	if (!have_start && !have_end) {
		clock->now = target;
		event->type = PSX_SCREEN_EVENT_NONE;
		event->time = target;
		return 0;
	}
	choose_start = have_start && (!have_end ||
			time_compare(clock->next_start, clock->next_end) < 0);
	if (choose_start) {
		psx_screen_time when = clock->next_start;
		start_vblank(clock, when);
		*event = make_event(PSX_SCREEN_EVENT_VBLANK_START, when);
	} else {
		clock->now = clock->next_end;
		clock->last_end = clock->next_end;
		clock->end_pending = 0;
		clock->in_vblank = 0;
		*event = make_event(PSX_SCREEN_EVENT_VBLANK_END, clock->now);
	}
	return 1;
}

uint64_t psx_screen_time_to_cycles_ceil(psx_screen_time event_time,
		uint64_t main_clock_hz)
{
	uint64_t seconds;
	uint64_t whole_cycles;
	uint64_t high_product;
	uint64_t low_numerator;
	uint64_t fraction_cycles;
	uint64_t fraction_remainder;
	uint64_t rounded_fraction;
	if (event_time.seconds < 0 || !main_clock_hz)
		return 0;
	if (main_clock_hz > UINT64_C(1000000000))
		return UINT64_MAX;
	seconds = (uint64_t)event_time.seconds;
	if (seconds > UINT64_MAX / main_clock_hz)
		return UINT64_MAX;
	whole_cycles = seconds * main_clock_hz;
	/* Split the fractional second so the intermediate products fit 64 bits. */
	high_product = (event_time.attoseconds / UINT64_C(1000000000)) * main_clock_hz;
	fraction_cycles = high_product / UINT64_C(1000000000);
	fraction_remainder = high_product % UINT64_C(1000000000);
	low_numerator = fraction_remainder * UINT64_C(1000000000) +
		(event_time.attoseconds % UINT64_C(1000000000)) * main_clock_hz;
	if (low_numerator >= PSX_SCREEN_ATTOSECONDS_PER_SECOND) {
		fraction_cycles++;
		low_numerator -= PSX_SCREEN_ATTOSECONDS_PER_SECOND;
	}
	rounded_fraction = fraction_cycles + (low_numerator != 0);
	if (UINT64_MAX - whole_cycles < rounded_fraction)
		return UINT64_MAX;
	return whole_cycles + rounded_fraction;
}
