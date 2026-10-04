#include "mb3773.h"
#include <assert.h>
#include <stdio.h>
typedef struct Trace { MB3773 *device; unsigned calls; uint64_t time; } Trace;
static void request(void *opaque)
{
    Trace *trace = opaque;
    trace->calls++;
    trace->time = mb3773_elapsed_us(trace->device);
    assert(!mb3773_armed(trace->device));
}
int main(void)
{
    Trace trace = {0};
    MB3773Config config = {&trace, request};
    assert(mb3773_create(&config, &trace.device) == 0);
    assert(mb3773_ck(trace.device) == 0);
    assert(mb3773_advance_to_us(trace.device, 4999999) == 0);
    assert(mb3773_write_ck(trace.device, 0) == 0); /* Low alone does not kick. */
    assert(!trace.calls);
    assert(mb3773_advance_to_us(trace.device, 5000000) == 0);
    assert(trace.calls == 1 && trace.time == 5000000);
    assert(mb3773_advance_to_us(trace.device, 15000000) == 0);
    assert(trace.calls == 1); /* No periodic reassertion without reset/edge. */
    assert(mb3773_reset(trace.device) == 0);
    assert(mb3773_write_ck(trace.device, 1) == 0);
    assert(mb3773_advance_to_us(trace.device, 19000000) == 0);
    assert(mb3773_write_ck(trace.device, 1) == 0); /* Rising/repeated high no kick. */
    assert(mb3773_write_ck(trace.device, 0) == 0); /* Falling rearms at 24 s. */
    assert(mb3773_advance_to_us(trace.device, 23999999) == 0 && trace.calls == 1);
    assert(mb3773_advance_to_us(trace.device, 25000000) == 0);
    assert(trace.calls == 2 && trace.time == 24000000);
    assert(mb3773_write_ck(trace.device, 1) == 0);
    assert(mb3773_reset(trace.device) == 0 && mb3773_ck(trace.device) == 1);
    assert(mb3773_advance_to_us(trace.device, 29000000) == 0);
    assert(mb3773_write_ck(trace.device, 0) == 0); /* Reset retained high CK. */
    assert(mb3773_advance_to_us(trace.device, 33999999) == 0 && trace.calls == 2);
    assert(mb3773_advance_to_us(trace.device, 34000000) == 0 && trace.calls == 3);
    assert(mb3773_advance_to_us(trace.device, 33999999) == MB3773_TIME);
    assert(mb3773_elapsed_us(trace.device) == 34000000);
    assert(mb3773_write_ck(trace.device, 2) == MB3773_ARGUMENT);
    assert(mb3773_advance_to_us(trace.device, UINT64_MAX) == 0);
    assert(mb3773_reset(trace.device) == MB3773_TIME);
    assert(mb3773_write_ck(trace.device, 1) == 0);
    assert(mb3773_write_ck(trace.device, 0) == MB3773_TIME);
    assert(mb3773_ck(trace.device) == 1 && !mb3773_armed(trace.device));
    assert(mb3773_reset_requests(trace.device) == 3);
    mb3773_destroy(trace.device);
    puts("MB3773 source timer/edge/reset/scheduler tests passed");
    return 0;
}
