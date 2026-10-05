/* license:BSD-3-Clause
 * Portions modeled on MAME screen_device timing.
 * Copyright Aaron Giles.
 */
#ifndef LIBPCSXCORE_ARCADE_PSX_SCREEN_CLOCK_H
#define LIBPCSXCORE_ARCADE_PSX_SCREEN_CLOCK_H

#include <stdint.h>

#define PSX_SCREEN_ATTOSECONDS_PER_SECOND UINT64_C(1000000000000000000)
#define PSX_SCREEN_VBLANK_ATTOSECONDS UINT64_C(2500000000000000)

/* Values are the results of MAME HZ_TO_ATTOSECONDS(double) for these rates. */
#define PSX_SCREEN_NTSC_240_PERIOD UINT64_C(16715113233669674)
#define PSX_SCREEN_NTSC_480_PERIOD UINT64_C(16683335451858734)
#define PSX_SCREEN_PAL_PERIOD      UINT64_C(20000000000000000)

typedef struct psx_screen_time {
	int64_t seconds;
	uint64_t attoseconds;
} psx_screen_time;

typedef enum psx_screen_event_type {
	PSX_SCREEN_EVENT_NONE = 0,
	PSX_SCREEN_EVENT_VBLANK_START,
	PSX_SCREEN_EVENT_VBLANK_END
} psx_screen_event_type;

typedef struct psx_screen_event {
	psx_screen_event_type type;
	psx_screen_time time;
} psx_screen_event;

typedef struct psx_screen_clock {
	psx_screen_time now;
	psx_screen_time last_start;
	psx_screen_time last_end;
	psx_screen_time next_start;
	psx_screen_time next_end;
	uint64_t frame_period;
	int configured;
	int in_vblank;
	int start_pending;
	int end_pending;
} psx_screen_clock;

/* Cold screen phase is zero: first start is one frame period after time zero. */
void psx_screen_clock_init(psx_screen_clock *clock);

/*
 * Apply a screen configuration at absolute time `now`.
 * Returns 1 and fills `event` if the new configuration starts VBLANK now;
 * otherwise returns 0. The first configuration schedules the cold start.
 */
int psx_screen_clock_configure(psx_screen_clock *clock, psx_screen_time now,
		uint64_t frame_period, psx_screen_event *event);

/* Read the next armed timer without advancing the clock. */
int psx_screen_clock_next_deadline(const psx_screen_clock *clock,
		psx_screen_event *event);

/* Match screen_device::vblank(): true while absolute time is before last_end. */
int psx_screen_clock_is_vblank(const psx_screen_clock *clock,
		psx_screen_time at_time);

/* Advance to the next event at or before `target`. Returns 1 on an event. */
int psx_screen_clock_advance_to(psx_screen_clock *clock,
		psx_screen_time target, psx_screen_event *event);

/* Convert an absolute event time to a main-CPU cycle deadline using ceil. */
uint64_t psx_screen_time_to_cycles_ceil(psx_screen_time event_time,
		uint64_t main_clock_hz);

#endif
