#define _POSIX_C_SOURCE 200809L
#include "loop_internal.h"

#include <stdlib.h>
#include <time.h>

rotmos_status rotmos_loop_monotonic(rotmos_u64 *out, rotmos_error *error)
{
    struct timespec now;
    time_t seconds;
    unsigned long words[4];
    unsigned long carry;
    unsigned long product;
    size_t i;
    rotmos_u64 fraction;
    rotmos_u64 result;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return rotmos_fail(error, ROTMOS_IO, "loop_now",
                           "cannot read the monotonic clock");
    seconds = now.tv_sec;
    for (i = 4; i != 0; --i) {
        words[i - 1] = (unsigned long)(seconds % (time_t)65536);
        seconds /= (time_t)65536;
    }
    carry = 0;
    for (i = 4; i != 0; --i) {
        product = words[i - 1] * 1000UL + carry;
        words[i - 1] = product % 65536UL;
        carry = product / 65536UL;
    }
    if (seconds != 0 || carry != 0)
        return rotmos_fail(error, ROTMOS_OVERFLOW, "loop_now",
                           "monotonic milliseconds exceed the wide counter");
    result.high = words[0] * 65536UL + words[1];
    result.low = words[2] * 65536UL + words[3];
    fraction.high = 0;
    fraction.low = (unsigned long)(now.tv_nsec / 1000000);
    return rotmos_u64_add(result, fraction, out, error);
}

rotmos_status rotmos_loop_now(const rotmos_loop *loop, rotmos_u64 *out,
                              rotmos_error *error)
{
    rotmos_status status;
    status = rotmos_loop_owner(loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "loop_now",
                           "timestamp output is required");
    return rotmos_loop_monotonic(out, error);
}

rotmos_status rotmos_loop_deadline(rotmos_u64 now, long delay, rotmos_u64 *out,
                                   rotmos_error *error)
{
    rotmos_u64 relative;
    rotmos_status status;
    status = rotmos_u64_from_size((size_t)delay, &relative, error);
    if (status != ROTMOS_OK)
        return status;
    return rotmos_u64_add(now, relative, out, error);
}

void rotmos_loop_timer_remove(rotmos_timer *timer)
{
    rotmos_loop *loop;
    if (!timer->scheduled)
        return;
    loop = timer->item.loop;
    if (timer->timer_previous != NULL)
        timer->timer_previous->timer_next = timer->timer_next;
    else
        loop->timers = timer->timer_next;
    if (timer->timer_next != NULL)
        timer->timer_next->timer_previous = timer->timer_previous;
    timer->timer_next = timer->timer_previous = NULL;
    timer->scheduled = 0;
}

void rotmos_loop_timer_schedule(rotmos_timer *timer)
{
    rotmos_loop *loop;
    rotmos_timer *cursor;
    rotmos_timer *previous;
    int order;
    loop = timer->item.loop;
    previous = NULL;
    for (cursor = loop->timers; cursor != NULL; cursor = cursor->timer_next) {
        (void)rotmos_u64_compare(timer->due, cursor->due, &order, NULL);
        if (order < 0)
            break;
        previous = cursor;
    }
    timer->timer_previous = previous;
    timer->timer_next = cursor;
    if (previous != NULL)
        previous->timer_next = timer;
    else
        loop->timers = timer;
    if (cursor != NULL)
        cursor->timer_previous = timer;
    timer->scheduled = 1;
}

rotmos_status rotmos_loop_timer(rotmos_loop *loop, long delay_ms,
                                long repeat_ms, rotmos_timer_fn callback,
                                rotmos_retire_fn retired, void *userdata,
                                rotmos_timer **out, rotmos_error *error)
{
    rotmos_timer *timer;
    rotmos_u64 now;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    status = rotmos_loop_owner(loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (delay_ms < 0 || repeat_ms < 0 || callback == NULL || out == NULL)
        return rotmos_fail(
            error, ROTMOS_INVALID, "loop_timer",
            "nonnegative delays, callback and output are required");
    status = rotmos_loop_monotonic(&now, error);
    if (status != ROTMOS_OK)
        return status;
    timer = calloc(1, sizeof(*timer));
    if (timer == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "loop_timer",
                           "cannot allocate timer");
    status = rotmos_loop_deadline(now, delay_ms, &timer->due, error);
    if (status == ROTMOS_OK)
        status = rotmos_loop_accept(loop, &timer->item, ROTMOS_LOOP_TIMER,
                                    retired, userdata, error);
    if (status != ROTMOS_OK) {
        free(timer);
        return status;
    }
    timer->repeat = repeat_ms;
    timer->callback = callback;
    rotmos_loop_timer_schedule(timer);
    loop->backend.wake(loop->backend.userdata);
    *out = timer;
    return ROTMOS_OK;
}

void rotmos_timer_cancel(rotmos_timer *timer)
{
    if (timer != NULL && rotmos_loop_owner(timer->item.loop, NULL) == ROTMOS_OK)
        rotmos_loop_cancel(&timer->item);
}
