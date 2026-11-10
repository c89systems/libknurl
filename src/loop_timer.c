#define _POSIX_C_SOURCE 200809L
#include "loop_internal.h"

#include <stdlib.h>
#include <time.h>

knurl_status knurl_loop_monotonic(knurl_u64 *out, knurl_error *error)
{
    struct timespec now;
    time_t seconds;
    unsigned long words[4];
    unsigned long carry;
    unsigned long product;
    size_t i;
    knurl_u64 fraction;
    knurl_u64 result;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return knurl_fail(error, KNURL_IO, "loop_now",
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
        return knurl_fail(error, KNURL_OVERFLOW, "loop_now",
                          "monotonic milliseconds exceed the wide counter");
    result.high = words[0] * 65536UL + words[1];
    result.low = words[2] * 65536UL + words[3];
    fraction.high = 0;
    fraction.low = (unsigned long)(now.tv_nsec / 1000000);
    return knurl_u64_add(result, fraction, out, error);
}

knurl_status knurl_loop_now(const knurl_loop *loop, knurl_u64 *out,
                            knurl_error *error)
{
    knurl_status status;
    status = knurl_loop_owner(loop, error);
    if (status != KNURL_OK)
        return status;
    if (out == NULL)
        return knurl_fail(error, KNURL_INVALID, "loop_now",
                          "timestamp output is required");
    return knurl_loop_monotonic(out, error);
}

knurl_status knurl_loop_deadline(knurl_u64 now, long delay, knurl_u64 *out,
                                 knurl_error *error)
{
    knurl_u64 relative;
    knurl_status status;
    status = knurl_u64_from_size((size_t)delay, &relative, error);
    if (status != KNURL_OK)
        return status;
    return knurl_u64_add(now, relative, out, error);
}

void knurl_loop_timer_remove(knurl_timer *timer)
{
    knurl_loop *loop;
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

void knurl_loop_timer_schedule(knurl_timer *timer)
{
    knurl_loop *loop;
    knurl_timer *cursor;
    knurl_timer *previous;
    int order;
    loop = timer->item.loop;
    previous = NULL;
    for (cursor = loop->timers; cursor != NULL; cursor = cursor->timer_next) {
        (void)knurl_u64_compare(timer->due, cursor->due, &order, NULL);
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

knurl_status knurl_loop_timer(knurl_loop *loop, long delay_ms, long repeat_ms,
                              knurl_timer_fn callback, knurl_retire_fn retired,
                              void *userdata, knurl_timer **out,
                              knurl_error *error)
{
    knurl_timer *timer;
    knurl_u64 now;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    status = knurl_loop_owner(loop, error);
    if (status != KNURL_OK)
        return status;
    if (delay_ms < 0 || repeat_ms < 0 || callback == NULL || out == NULL)
        return knurl_fail(
            error, KNURL_INVALID, "loop_timer",
            "nonnegative delays, callback and output are required");
    status = knurl_loop_monotonic(&now, error);
    if (status != KNURL_OK)
        return status;
    timer = calloc(1, sizeof(*timer));
    if (timer == NULL)
        return knurl_fail(error, KNURL_NOMEM, "loop_timer",
                          "cannot allocate timer");
    status = knurl_loop_deadline(now, delay_ms, &timer->due, error);
    if (status == KNURL_OK)
        status = knurl_loop_accept(loop, &timer->item, KNURL_LOOP_TIMER,
                                   retired, userdata, error);
    if (status != KNURL_OK) {
        free(timer);
        return status;
    }
    timer->repeat = repeat_ms;
    timer->callback = callback;
    knurl_loop_timer_schedule(timer);
    loop->backend.wake(loop->backend.userdata);
    *out = timer;
    return KNURL_OK;
}

void knurl_timer_cancel(knurl_timer *timer)
{
    if (timer != NULL && knurl_loop_owner(timer->item.loop, NULL) == KNURL_OK)
        knurl_loop_cancel(&timer->item);
}
