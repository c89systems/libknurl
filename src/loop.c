#define _POSIX_C_SOURCE 200809L
#include "loop_internal.h"
#include "server_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

knurl_loop *knurl_server_loop(knurl_server *server)
{
    if (server == NULL || server->loop == NULL ||
        server->loop->process_id != (long)getpid())
        return NULL;
    return server->loop;
}

knurl_status knurl_loop_owner(const knurl_loop *loop, knurl_error *error)
{
    if (loop == NULL || loop->process_id != (long)getpid() ||
        !pthread_equal(loop->owner, pthread_self()))
        return knurl_fail(
            error, KNURL_STATE, "loop_owner",
            "operation requires this process's owner loop thread");
    return KNURL_OK;
}

knurl_status knurl_loop_create(const struct knurl_loop_backend *backend,
                               knurl_loop **out, knurl_error *error)
{
    knurl_loop *loop;
    *out = NULL;
    loop = calloc(1, sizeof(*loop));
    if (loop == NULL)
        return knurl_fail(error, KNURL_NOMEM, "loop_create",
                          "cannot allocate loop");
    if (pthread_mutex_init(&loop->mutex, NULL) != 0) {
        free(loop);
        return knurl_fail(error, KNURL_IO, "loop_create",
                          "cannot initialize loop wake lock");
    }
    {
        static unsigned long identity;
        loop->identity = __atomic_add_fetch(&identity, 1UL, __ATOMIC_RELAXED);
    }
    loop->process_id = (long)getpid();
    loop->owner = pthread_self();
    loop->backend = *backend;
    loop->accepting = 1;
    *out = loop;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_loop_accept(knurl_loop *loop, struct knurl_loop_item *item,
                               knurl_loop_kind kind, knurl_retire_fn retired,
                               void *userdata, knurl_error *error)
{
    knurl_status status;
    status = knurl_loop_owner(loop, error);
    if (status != KNURL_OK)
        return status;
    if (!loop->accepting)
        return knurl_fail(error, KNURL_STATE, "loop_register",
                          "loop is closing");
    if (loop->count >= KNURL_LOOP_MAX_REGISTRATIONS ||
        (kind == KNURL_LOOP_DEFER &&
         loop->deferred_count >= KNURL_LOOP_MAX_DEFERRED))
        return knurl_fail(error, KNURL_LIMIT, "loop_register",
                          "loop registration budget is full");
    item->loop = loop;
    item->kind = kind;
    item->userdata = userdata;
    item->retired = retired;
    item->next = loop->items;
    if (loop->items != NULL)
        loop->items->previous = item;
    loop->items = item;
    ++loop->count;
    if (kind == KNURL_LOOP_DEFER)
        ++loop->deferred_count;
    knurl_error_clear(error);
    return KNURL_OK;
}

static void unlink_item(struct knurl_loop_item *item)
{
    knurl_loop *loop;
    loop = item->loop;
    if (item->previous != NULL)
        item->previous->next = item->next;
    else
        loop->items = item->next;
    if (item->next != NULL)
        item->next->previous = item->previous;
    --loop->count;
    if (item->kind == KNURL_LOOP_DEFER)
        --loop->deferred_count;
}

/* Called with the wake lock held, including for owner-thread event delivery. */
static void ready_remove(struct knurl_loop_item *item)
{
    knurl_loop *loop;
    loop = item->loop;
    if (!item->queued)
        return;
    if (item->ready_previous != NULL)
        item->ready_previous->ready_next = item->ready_next;
    else if (item->queued == 2)
        loop->batch_head = item->ready_next;
    else
        loop->ready_head = item->ready_next;
    if (item->ready_next != NULL)
        item->ready_next->ready_previous = item->ready_previous;
    else if (item->queued == 2)
        loop->batch_tail = item->ready_previous;
    else
        loop->ready_tail = item->ready_previous;
    item->ready_next = item->ready_previous = NULL;
    if (item->queued == 2)
        --loop->batch_count;
    else
        --loop->ready_count;
    item->queued = 0;
}

void knurl_loop_ready(struct knurl_loop_item *item)
{
    knurl_loop *loop;
    int queued;
    loop = item->loop;
    queued = 0;
    (void)pthread_mutex_lock(&loop->mutex);
    if (!item->cancelled && !item->queued && loop->accepting) {
        item->queued = 1;
        item->ready_previous = loop->ready_tail;
        if (loop->ready_tail != NULL)
            loop->ready_tail->ready_next = item;
        else
            loop->ready_head = item;
        loop->ready_tail = item;
        ++loop->ready_count;
        queued = 1;
    }
    (void)pthread_mutex_unlock(&loop->mutex);
    if (queued)
        loop->backend.wake(loop->backend.userdata);
}

void knurl_loop_cancel(struct knurl_loop_item *item)
{
    knurl_loop *loop;
    knurl_watch *watch;
    knurl_error error;
    knurl_status status;
    loop = item->loop;
    (void)pthread_mutex_lock(&loop->mutex);
    if (item->cancelled) {
        (void)pthread_mutex_unlock(&loop->mutex);
        return;
    }
    item->cancelled = 1;
    ready_remove(item);
    (void)pthread_mutex_unlock(&loop->mutex);
    if (item->kind == KNURL_LOOP_WATCH) {
        watch = (knurl_watch *)item;
        if (watch->registered != 0 && loop->native_ready) {
            status = loop->backend.watch_update(loop->backend.userdata, watch,
                                                watch->registered, 0, &error);
            if (status == KNURL_OK)
                watch->registered = 0;
            else if (loop->failure.status == KNURL_OK)
                loop->failure = error;
        }
    } else if (item->kind == KNURL_LOOP_TIMER) {
        knurl_loop_timer_remove((knurl_timer *)item);
    }
    if (loop->retire_tail != NULL)
        loop->retire_tail->retire_next = item;
    else
        loop->retire_head = item;
    loop->retire_tail = item;
    loop->backend.wake(loop->backend.userdata);
}

knurl_status knurl_loop_watch(knurl_loop *loop, int fd, unsigned int interest,
                              knurl_watch_fn callback, knurl_retire_fn retired,
                              void *userdata, knurl_watch **out,
                              knurl_error *error)
{
    struct knurl_loop_item *item;
    knurl_watch *watch;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    status = knurl_loop_owner(loop, error);
    if (status != KNURL_OK)
        return status;
    if (out == NULL || callback == NULL || fd < 0 ||
        (interest & ~(KNURL_READABLE | KNURL_WRITABLE)) != 0 ||
        fcntl(fd, F_GETFD) == -1)
        return knurl_fail(error, KNURL_INVALID, "loop_watch",
                          "valid descriptor, callback, output and read/write "
                          "interest are required");
    for (item = loop->items; item != NULL; item = item->next) {
        if (!item->cancelled && item->kind == KNURL_LOOP_WATCH &&
            ((knurl_watch *)item)->fd == fd)
            return knurl_fail(error, KNURL_CONFLICT, "loop_watch",
                              "descriptor already belongs to a watch");
    }
    watch = calloc(1, sizeof(*watch));
    if (watch == NULL)
        return knurl_fail(error, KNURL_NOMEM, "loop_watch",
                          "cannot allocate watch");
    watch->fd = fd;
    watch->callback = callback;
    watch->interest = interest;
    status = knurl_loop_accept(loop, &watch->item, KNURL_LOOP_WATCH, retired,
                               userdata, error);
    if (status != KNURL_OK) {
        free(watch);
        return status;
    }
    if (loop->native_ready && interest != 0) {
        status = loop->backend.watch_update(loop->backend.userdata, watch, 0,
                                            interest, error);
        if (status != KNURL_OK) {
            unlink_item(&watch->item);
            loop->backend.watch_release(loop->backend.userdata, watch);
            free(watch);
            return status;
        }
        watch->registered = interest;
    }
    *out = watch;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_watch_update(knurl_watch *watch, unsigned int interest,
                                knurl_error *error)
{
    knurl_loop *loop;
    knurl_status status;
    if (watch == NULL)
        return knurl_fail(error, KNURL_INVALID, "watch_update",
                          "watch is required");
    loop = watch->item.loop;
    status = knurl_loop_owner(loop, error);
    if (status != KNURL_OK)
        return status;
    if (watch->item.cancelled || !loop->accepting)
        return knurl_fail(error, KNURL_STATE, "watch_update",
                          "watch is cancelled or loop is closing");
    if ((interest & ~(KNURL_READABLE | KNURL_WRITABLE)) != 0)
        return knurl_fail(error, KNURL_INVALID, "watch_update",
                          "interest must contain only read/write flags");
    if (interest != watch->interest && loop->native_ready) {
        status = loop->backend.watch_update(loop->backend.userdata, watch,
                                            watch->registered, interest, error);
        if (status != KNURL_OK)
            return status;
        watch->registered = interest;
    }
    watch->interest = interest;
    watch->ready &= interest | KNURL_READ_CLOSED | KNURL_EVENT_ERROR;
    if (interest == 0) {
        watch->ready = 0;
        (void)pthread_mutex_lock(&loop->mutex);
        ready_remove(&watch->item);
        (void)pthread_mutex_unlock(&loop->mutex);
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_watch_cancel(knurl_watch *watch)
{
    if (watch != NULL && knurl_loop_owner(watch->item.loop, NULL) == KNURL_OK)
        knurl_loop_cancel(&watch->item);
}

void knurl_loop_watch_ready(knurl_watch *watch, unsigned int flags)
{
    if (watch->item.cancelled || watch->interest == 0)
        return;
    watch->ready |=
        flags & (watch->interest | KNURL_READ_CLOSED | KNURL_EVENT_ERROR);
    if (watch->ready != 0)
        knurl_loop_ready(&watch->item);
}

knurl_status knurl_loop_activate(knurl_loop *loop, knurl_error *error)
{
    struct knurl_loop_item *item;
    knurl_watch *watch;
    knurl_status status;
    loop->native_ready = 1;
    for (item = loop->items; item != NULL; item = item->next) {
        if (item->kind != KNURL_LOOP_WATCH || item->cancelled)
            continue;
        watch = (knurl_watch *)item;
        if (watch->interest == watch->registered)
            continue;
        status = loop->backend.watch_update(loop->backend.userdata, watch,
                                            watch->registered, watch->interest,
                                            error);
        if (status != KNURL_OK)
            return status;
        watch->registered = watch->interest;
    }
    return KNURL_OK;
}

knurl_status knurl_loop_defer(knurl_loop *loop, knurl_defer_fn callback,
                              knurl_retire_fn retired, void *userdata,
                              knurl_error *error)
{
    struct knurl_deferred *deferred;
    knurl_status status;
    if (callback == NULL)
        return knurl_fail(error, KNURL_INVALID, "loop_defer",
                          "callback is required");
    deferred = calloc(1, sizeof(*deferred));
    if (deferred == NULL)
        return knurl_fail(error, KNURL_NOMEM, "loop_defer",
                          "cannot allocate deferred callback");
    deferred->callback = callback;
    status = knurl_loop_accept(loop, &deferred->item, KNURL_LOOP_DEFER, retired,
                               userdata, error);
    if (status != KNURL_OK) {
        free(deferred);
        return status;
    }
    knurl_loop_ready(&deferred->item);
    return KNURL_OK;
}

static int retire_one(knurl_loop *loop)
{
    struct knurl_loop_item *item;
    item = loop->retire_head;
    if (item == NULL)
        return 0;
    /* A failed OS deregistration must not free an event pointer still owned
     * by the backend. Runtime teardown closes that backend before release. */
    if (item->kind == KNURL_LOOP_WATCH &&
        ((knurl_watch *)item)->registered != 0)
        return 0;
    loop->retire_head = item->retire_next;
    if (loop->retire_head == NULL)
        loop->retire_tail = NULL;
    unlink_item(item);
    if (item->kind == KNURL_LOOP_WATCH)
        loop->backend.watch_release(loop->backend.userdata,
                                    (knurl_watch *)item);
    if (item->retired != NULL)
        item->retired(item->userdata);
    free(item);
    return 1;
}

void knurl_loop_turn(knurl_loop *loop)
{
    struct knurl_loop_item *item;
    struct knurl_deferred *deferred;
    knurl_watch *watch;
    knurl_timer *timer;
    knurl_u64 now;
    knurl_error error;
    unsigned int flags;
    unsigned int budget;
    unsigned int promoted;
    size_t ready;
    int order;
    if (loop == NULL || loop->in_turn || !loop->native_ready)
        return;
    loop->in_turn = 1;
    budget = KNURL_LOOP_CALLBACKS_PER_TURN;
    while (budget != 0 && retire_one(loop))
        --budget;
    if (knurl_loop_monotonic(&now, &error) != KNURL_OK)
        loop->failure = error;
    else {
        for (promoted = 0;
             promoted < KNURL_LOOP_CALLBACKS_PER_TURN && loop->timers != NULL;
             ++promoted) {
            timer = loop->timers;
            (void)knurl_u64_compare(timer->due, now, &order, NULL);
            if (order > 0)
                break;
            knurl_loop_timer_remove(timer);
            knurl_loop_ready(&timer->item);
        }
    }
    (void)pthread_mutex_lock(&loop->mutex);
    if (loop->batch_head == NULL) {
        loop->batch_head = loop->ready_head;
        loop->batch_tail = loop->ready_tail;
        loop->batch_count = loop->ready_count;
        loop->ready_head = loop->ready_tail = NULL;
        loop->ready_count = 0;
        for (item = loop->batch_head; item != NULL; item = item->ready_next)
            item->queued = 2;
    }
    ready = loop->batch_count;
    (void)pthread_mutex_unlock(&loop->mutex);
    while (ready != 0 && budget != 0 && loop->accepting) {
        --ready;
        --budget;
        (void)pthread_mutex_lock(&loop->mutex);
        item = loop->batch_head;
        if (item != NULL)
            ready_remove(item);
        (void)pthread_mutex_unlock(&loop->mutex);
        if (item == NULL)
            break;
        switch (item->kind) {
        case KNURL_LOOP_WATCH:
            watch = (knurl_watch *)item;
            flags = watch->ready;
            watch->ready = 0;
            if (flags != 0 && !item->cancelled)
                watch->callback(watch, flags, item->userdata);
            break;
        case KNURL_LOOP_TIMER:
            timer = (knurl_timer *)item;
            timer->callback(timer, item->userdata);
            if (!item->cancelled && timer->repeat != 0) {
                if (knurl_loop_monotonic(&now, &error) == KNURL_OK &&
                    knurl_loop_deadline(now, timer->repeat, &timer->due,
                                        &error) == KNURL_OK)
                    knurl_loop_timer_schedule(timer);
                else {
                    loop->failure = error;
                    knurl_loop_cancel(item);
                }
            } else if (!item->cancelled)
                knurl_loop_cancel(item);
            break;
        case KNURL_LOOP_DEFER:
            deferred = (struct knurl_deferred *)item;
            deferred->callback(loop, item->userdata);
            knurl_loop_cancel(item);
            break;
        case KNURL_LOOP_WAKER:
            ((knurl_waker *)item)->callback(loop, item->userdata);
            break;
        }
    }
    while (budget != 0 && retire_one(loop))
        --budget;
    loop->in_turn = 0;
}

long knurl_loop_wait(knurl_loop *loop)
{
    knurl_u64 now;
    knurl_u64 difference;
    knurl_error error;
    size_t delay;
    size_t ready;
    int order;
    (void)pthread_mutex_lock(&loop->mutex);
    ready = loop->ready_count + loop->batch_count;
    (void)pthread_mutex_unlock(&loop->mutex);
    if (ready != 0 || loop->retire_head != NULL ||
        loop->failure.status != KNURL_OK)
        return 0;
    if (loop->timers == NULL)
        return LONG_MAX;
    if (knurl_loop_monotonic(&now, &error) != KNURL_OK) {
        loop->failure = error;
        return 0;
    }
    (void)knurl_u64_compare(loop->timers->due, now, &order, NULL);
    if (order <= 0)
        return 0;
    difference.high = loop->timers->due.high - now.high;
    difference.low = (loop->timers->due.low - now.low) & 0xffffffffUL;
    if (loop->timers->due.low < now.low)
        --difference.high;
    if (knurl_u64_to_size(difference, &delay, NULL) != KNURL_OK ||
        delay > (size_t)LONG_MAX)
        return LONG_MAX;
    return (long)delay;
}

int knurl_loop_pending(knurl_loop *loop)
{
    return loop != NULL && loop->count != 0;
}

void knurl_loop_shutdown(knurl_loop *loop)
{
    struct knurl_loop_item *item;
    if (loop == NULL)
        return;
    (void)pthread_mutex_lock(&loop->mutex);
    loop->accepting = 0;
    (void)pthread_mutex_unlock(&loop->mutex);
    for (item = loop->items; item != NULL; item = item->next)
        knurl_loop_cancel(item);
    while (retire_one(loop)) {
    }
}

void knurl_loop_release(knurl_loop *loop)
{
    struct knurl_loop_item *item;
    if (loop == NULL)
        return;
    loop->native_ready = 0;
    for (item = loop->items; item != NULL; item = item->next)
        if (item->kind == KNURL_LOOP_WATCH)
            ((knurl_watch *)item)->registered = 0;
    knurl_loop_shutdown(loop);
    (void)pthread_mutex_destroy(&loop->mutex);
    free(loop);
}

void knurl_loop_discard_after_fork(knurl_loop *loop)
{
    struct knurl_loop_item *item;
    struct knurl_loop_item *next;
    if (loop == NULL)
        return;
    for (item = loop->items; item != NULL; item = next) {
        next = item->next;
        if (item->kind == KNURL_LOOP_WATCH)
            loop->backend.watch_release(loop->backend.userdata,
                                        (knurl_watch *)item);
        free(item);
    }
    (void)pthread_mutex_destroy(&loop->mutex);
    free(loop);
}
