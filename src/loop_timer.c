#define _POSIX_C_SOURCE 200809L
#include "loop_internal.h"
#include <knurl/loop.h>

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
    if (!timer->impl->scheduled)
        return;
    loop = timer->impl->item.loop;
    if (timer->impl->timer_previous != NULL)
        timer->impl->timer_previous->impl->timer_next = timer->impl->timer_next;
    else
        loop->impl->timers = timer->impl->timer_next;
    if (timer->impl->timer_next != NULL)
        timer->impl->timer_next->impl->timer_previous =
            timer->impl->timer_previous;
    timer->impl->timer_next = timer->impl->timer_previous = NULL;
    timer->impl->scheduled = 0;
}

void knurl_loop_timer_schedule(knurl_timer *timer)
{
    knurl_loop *loop;
    knurl_timer *cursor;
    knurl_timer *previous;
    int order;
    loop = timer->impl->item.loop;
    previous = NULL;
    for (cursor = loop->impl->timers; cursor != NULL;
         cursor = cursor->impl->timer_next) {
        (void)knurl_u64_compare(timer->impl->due, cursor->impl->due, &order,
                                NULL);
        if (order < 0)
            break;
        previous = cursor;
    }
    timer->impl->timer_previous = previous;
    timer->impl->timer_next = cursor;
    if (previous != NULL)
        previous->impl->timer_next = timer;
    else
        loop->impl->timers = timer;
    if (cursor != NULL)
        cursor->impl->timer_previous = timer;
    timer->impl->scheduled = 1;
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
    timer = calloc(1, sizeof(struct knurl_timer_impl));
    if (timer == NULL)
        return knurl_fail(error, KNURL_NOMEM, "loop_timer",
                          "cannot allocate timer");
    knurl_timer_receiver_init(timer);
    status = knurl_loop_deadline(now, delay_ms, &timer->impl->due, error);
    if (status == KNURL_OK)
        status = knurl_loop_accept(loop, &timer->impl->item, timer,
                                   KNURL_LOOP_TIMER, retired, userdata, error);
    if (status != KNURL_OK) {
        free(timer);
        return status;
    }
    timer->impl->repeat = repeat_ms;
    timer->impl->callback = callback;
    knurl_loop_timer_schedule(timer);
    loop->impl->backend.wake(loop->impl->backend.userdata);
    *out = timer;
    return KNURL_OK;
}

void knurl_timer_cancel(knurl_timer *timer)
{
    if (timer != NULL &&
        knurl_loop_owner(timer->impl->item.loop, NULL) == KNURL_OK)
        knurl_loop_cancel(&timer->impl->item);
}

void knurl_timer_receiver_init(knurl_timer *receiver)
{
    static const knurl_timer methods = {knurl_timer_cancel, NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_timer_impl *)receiver;
}
