#define _POSIX_C_SOURCE 200809L
#include "loop_internal.h"
#include "server_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

rotmos_loop *rotmos_server_loop(rotmos_server *server)
{
    if (server == NULL || server->loop == NULL ||
        server->loop->process_id != (long)getpid())
        return NULL;
    return server->loop;
}

rotmos_status rotmos_loop_owner(const rotmos_loop *loop, rotmos_error *error)
{
    if (loop == NULL || loop->process_id != (long)getpid() ||
        !pthread_equal(loop->owner, pthread_self()))
        return rotmos_fail(
            error, ROTMOS_STATE, "loop_owner",
            "operation requires this process's owner loop thread");
    return ROTMOS_OK;
}

rotmos_status rotmos_loop_create(const struct rotmos_loop_backend *backend,
                                 rotmos_loop **out, rotmos_error *error)
{
    rotmos_loop *loop;
    *out = NULL;
    loop = calloc(1, sizeof(*loop));
    if (loop == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "loop_create",
                           "cannot allocate loop");
    if (pthread_mutex_init(&loop->mutex, NULL) != 0) {
        free(loop);
        return rotmos_fail(error, ROTMOS_IO, "loop_create",
                           "cannot initialize loop wake lock");
    }
    loop->process_id = (long)getpid();
    loop->owner = pthread_self();
    loop->backend = *backend;
    loop->accepting = 1;
    *out = loop;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_loop_accept(rotmos_loop *loop,
                                 struct rotmos_loop_item *item,
                                 rotmos_loop_kind kind,
                                 rotmos_retire_fn retired, void *userdata,
                                 rotmos_error *error)
{
    rotmos_status status;
    status = rotmos_loop_owner(loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (!loop->accepting)
        return rotmos_fail(error, ROTMOS_STATE, "loop_register",
                           "loop is closing");
    if (loop->count >= ROTMOS_LOOP_MAX_REGISTRATIONS ||
        (kind == ROTMOS_LOOP_DEFER &&
         loop->deferred_count >= ROTMOS_LOOP_MAX_DEFERRED))
        return rotmos_fail(error, ROTMOS_LIMIT, "loop_register",
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
    if (kind == ROTMOS_LOOP_DEFER)
        ++loop->deferred_count;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static void unlink_item(struct rotmos_loop_item *item)
{
    rotmos_loop *loop;
    loop = item->loop;
    if (item->previous != NULL)
        item->previous->next = item->next;
    else
        loop->items = item->next;
    if (item->next != NULL)
        item->next->previous = item->previous;
    --loop->count;
    if (item->kind == ROTMOS_LOOP_DEFER)
        --loop->deferred_count;
}

/* Called with the wake lock held, including for owner-thread event delivery. */
static void ready_remove(struct rotmos_loop_item *item)
{
    rotmos_loop *loop;
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

void rotmos_loop_ready(struct rotmos_loop_item *item)
{
    rotmos_loop *loop;
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

void rotmos_loop_cancel(struct rotmos_loop_item *item)
{
    rotmos_loop *loop;
    rotmos_watch *watch;
    rotmos_error error;
    rotmos_status status;
    loop = item->loop;
    (void)pthread_mutex_lock(&loop->mutex);
    if (item->cancelled) {
        (void)pthread_mutex_unlock(&loop->mutex);
        return;
    }
    item->cancelled = 1;
    ready_remove(item);
    (void)pthread_mutex_unlock(&loop->mutex);
    if (item->kind == ROTMOS_LOOP_WATCH) {
        watch = (rotmos_watch *)item;
        if (watch->registered != 0 && loop->native_ready) {
            status = loop->backend.watch_update(loop->backend.userdata, watch,
                                                watch->registered, 0, &error);
            if (status == ROTMOS_OK)
                watch->registered = 0;
            else if (loop->failure.status == ROTMOS_OK)
                loop->failure = error;
        }
    } else if (item->kind == ROTMOS_LOOP_TIMER) {
        rotmos_loop_timer_remove((rotmos_timer *)item);
    }
    if (loop->retire_tail != NULL)
        loop->retire_tail->retire_next = item;
    else
        loop->retire_head = item;
    loop->retire_tail = item;
    loop->backend.wake(loop->backend.userdata);
}

rotmos_status rotmos_loop_watch(rotmos_loop *loop, int fd,
                                unsigned int interest, rotmos_watch_fn callback,
                                rotmos_retire_fn retired, void *userdata,
                                rotmos_watch **out, rotmos_error *error)
{
    struct rotmos_loop_item *item;
    rotmos_watch *watch;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    status = rotmos_loop_owner(loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (out == NULL || callback == NULL || fd < 0 ||
        (interest & ~(ROTMOS_READABLE | ROTMOS_WRITABLE)) != 0 ||
        fcntl(fd, F_GETFD) == -1)
        return rotmos_fail(error, ROTMOS_INVALID, "loop_watch",
                           "valid descriptor, callback, output and read/write "
                           "interest are required");
    for (item = loop->items; item != NULL; item = item->next) {
        if (item->kind == ROTMOS_LOOP_WATCH && ((rotmos_watch *)item)->fd == fd)
            return rotmos_fail(error, ROTMOS_CONFLICT, "loop_watch",
                               "descriptor already belongs to a watch");
    }
    watch = calloc(1, sizeof(*watch));
    if (watch == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "loop_watch",
                           "cannot allocate watch");
    watch->fd = fd;
    watch->callback = callback;
    watch->interest = interest;
    status = rotmos_loop_accept(loop, &watch->item, ROTMOS_LOOP_WATCH, retired,
                                userdata, error);
    if (status != ROTMOS_OK) {
        free(watch);
        return status;
    }
    if (loop->native_ready && interest != 0) {
        status = loop->backend.watch_update(loop->backend.userdata, watch, 0,
                                            interest, error);
        if (status != ROTMOS_OK) {
            unlink_item(&watch->item);
            loop->backend.watch_release(loop->backend.userdata, watch);
            free(watch);
            return status;
        }
        watch->registered = interest;
    }
    *out = watch;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_watch_update(rotmos_watch *watch, unsigned int interest,
                                  rotmos_error *error)
{
    rotmos_loop *loop;
    rotmos_status status;
    if (watch == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "watch_update",
                           "watch is required");
    loop = watch->item.loop;
    status = rotmos_loop_owner(loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (watch->item.cancelled || !loop->accepting)
        return rotmos_fail(error, ROTMOS_STATE, "watch_update",
                           "watch is cancelled or loop is closing");
    if ((interest & ~(ROTMOS_READABLE | ROTMOS_WRITABLE)) != 0)
        return rotmos_fail(error, ROTMOS_INVALID, "watch_update",
                           "interest must contain only read/write flags");
    if (interest != watch->interest && loop->native_ready) {
        status = loop->backend.watch_update(loop->backend.userdata, watch,
                                            watch->registered, interest, error);
        if (status != ROTMOS_OK)
            return status;
        watch->registered = interest;
    }
    watch->interest = interest;
    watch->ready &= interest | ROTMOS_READ_CLOSED | ROTMOS_EVENT_ERROR;
    if (interest == 0) {
        watch->ready = 0;
        (void)pthread_mutex_lock(&loop->mutex);
        ready_remove(&watch->item);
        (void)pthread_mutex_unlock(&loop->mutex);
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_watch_cancel(rotmos_watch *watch)
{
    if (watch != NULL && rotmos_loop_owner(watch->item.loop, NULL) == ROTMOS_OK)
        rotmos_loop_cancel(&watch->item);
}

void rotmos_loop_watch_ready(rotmos_watch *watch, unsigned int flags)
{
    if (watch->item.cancelled || watch->interest == 0)
        return;
    watch->ready |=
        flags & (watch->interest | ROTMOS_READ_CLOSED | ROTMOS_EVENT_ERROR);
    if (watch->ready != 0)
        rotmos_loop_ready(&watch->item);
}

rotmos_status rotmos_loop_activate(rotmos_loop *loop, rotmos_error *error)
{
    struct rotmos_loop_item *item;
    rotmos_watch *watch;
    rotmos_status status;
    loop->native_ready = 1;
    for (item = loop->items; item != NULL; item = item->next) {
        if (item->kind != ROTMOS_LOOP_WATCH || item->cancelled)
            continue;
        watch = (rotmos_watch *)item;
        if (watch->interest == watch->registered)
            continue;
        status = loop->backend.watch_update(loop->backend.userdata, watch,
                                            watch->registered, watch->interest,
                                            error);
        if (status != ROTMOS_OK)
            return status;
        watch->registered = watch->interest;
    }
    return ROTMOS_OK;
}

rotmos_status rotmos_loop_defer(rotmos_loop *loop, rotmos_defer_fn callback,
                                rotmos_retire_fn retired, void *userdata,
                                rotmos_error *error)
{
    struct rotmos_deferred *deferred;
    rotmos_status status;
    if (callback == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "loop_defer",
                           "callback is required");
    deferred = calloc(1, sizeof(*deferred));
    if (deferred == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "loop_defer",
                           "cannot allocate deferred callback");
    deferred->callback = callback;
    status = rotmos_loop_accept(loop, &deferred->item, ROTMOS_LOOP_DEFER,
                                retired, userdata, error);
    if (status != ROTMOS_OK) {
        free(deferred);
        return status;
    }
    rotmos_loop_ready(&deferred->item);
    return ROTMOS_OK;
}

static int retire_one(rotmos_loop *loop)
{
    struct rotmos_loop_item *item;
    item = loop->retire_head;
    if (item == NULL)
        return 0;
    /* A failed OS deregistration must not free an event pointer still owned
     * by the backend. Runtime teardown closes that backend before release. */
    if (item->kind == ROTMOS_LOOP_WATCH &&
        ((rotmos_watch *)item)->registered != 0)
        return 0;
    loop->retire_head = item->retire_next;
    if (loop->retire_head == NULL)
        loop->retire_tail = NULL;
    unlink_item(item);
    if (item->kind == ROTMOS_LOOP_WATCH)
        loop->backend.watch_release(loop->backend.userdata,
                                    (rotmos_watch *)item);
    if (item->retired != NULL)
        item->retired(item->userdata);
    free(item);
    return 1;
}

void rotmos_loop_turn(rotmos_loop *loop)
{
    struct rotmos_loop_item *item;
    struct rotmos_deferred *deferred;
    rotmos_watch *watch;
    rotmos_timer *timer;
    rotmos_u64 now;
    rotmos_error error;
    unsigned int flags;
    unsigned int budget;
    unsigned int promoted;
    size_t ready;
    int order;
    if (loop == NULL || loop->in_turn || !loop->native_ready)
        return;
    loop->in_turn = 1;
    budget = ROTMOS_LOOP_CALLBACKS_PER_TURN;
    while (budget != 0 && retire_one(loop))
        --budget;
    if (rotmos_loop_monotonic(&now, &error) != ROTMOS_OK)
        loop->failure = error;
    else {
        for (promoted = 0;
             promoted < ROTMOS_LOOP_CALLBACKS_PER_TURN && loop->timers != NULL;
             ++promoted) {
            timer = loop->timers;
            (void)rotmos_u64_compare(timer->due, now, &order, NULL);
            if (order > 0)
                break;
            rotmos_loop_timer_remove(timer);
            rotmos_loop_ready(&timer->item);
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
        case ROTMOS_LOOP_WATCH:
            watch = (rotmos_watch *)item;
            flags = watch->ready;
            watch->ready = 0;
            if (flags != 0 && !item->cancelled)
                watch->callback(watch, flags, item->userdata);
            break;
        case ROTMOS_LOOP_TIMER:
            timer = (rotmos_timer *)item;
            timer->callback(timer, item->userdata);
            if (!item->cancelled && timer->repeat != 0) {
                if (rotmos_loop_monotonic(&now, &error) == ROTMOS_OK &&
                    rotmos_loop_deadline(now, timer->repeat, &timer->due,
                                         &error) == ROTMOS_OK)
                    rotmos_loop_timer_schedule(timer);
                else {
                    loop->failure = error;
                    rotmos_loop_cancel(item);
                }
            } else if (!item->cancelled)
                rotmos_loop_cancel(item);
            break;
        case ROTMOS_LOOP_DEFER:
            deferred = (struct rotmos_deferred *)item;
            deferred->callback(loop, item->userdata);
            rotmos_loop_cancel(item);
            break;
        case ROTMOS_LOOP_WAKER:
            ((rotmos_waker *)item)->callback(loop, item->userdata);
            break;
        }
    }
    while (budget != 0 && retire_one(loop))
        --budget;
    loop->in_turn = 0;
}

long rotmos_loop_wait(rotmos_loop *loop)
{
    rotmos_u64 now;
    rotmos_u64 difference;
    rotmos_error error;
    size_t delay;
    size_t ready;
    int order;
    (void)pthread_mutex_lock(&loop->mutex);
    ready = loop->ready_count + loop->batch_count;
    (void)pthread_mutex_unlock(&loop->mutex);
    if (ready != 0 || loop->retire_head != NULL ||
        loop->failure.status != ROTMOS_OK)
        return 0;
    if (loop->timers == NULL)
        return LONG_MAX;
    if (rotmos_loop_monotonic(&now, &error) != ROTMOS_OK) {
        loop->failure = error;
        return 0;
    }
    (void)rotmos_u64_compare(loop->timers->due, now, &order, NULL);
    if (order <= 0)
        return 0;
    difference.high = loop->timers->due.high - now.high;
    difference.low = (loop->timers->due.low - now.low) & 0xffffffffUL;
    if (loop->timers->due.low < now.low)
        --difference.high;
    if (rotmos_u64_to_size(difference, &delay, NULL) != ROTMOS_OK ||
        delay > (size_t)LONG_MAX)
        return LONG_MAX;
    return (long)delay;
}

int rotmos_loop_pending(rotmos_loop *loop)
{
    return loop != NULL && loop->count != 0;
}

void rotmos_loop_shutdown(rotmos_loop *loop)
{
    struct rotmos_loop_item *item;
    if (loop == NULL)
        return;
    (void)pthread_mutex_lock(&loop->mutex);
    loop->accepting = 0;
    (void)pthread_mutex_unlock(&loop->mutex);
    for (item = loop->items; item != NULL; item = item->next)
        rotmos_loop_cancel(item);
    while (retire_one(loop)) {
    }
}

void rotmos_loop_release(rotmos_loop *loop)
{
    struct rotmos_loop_item *item;
    if (loop == NULL)
        return;
    loop->native_ready = 0;
    for (item = loop->items; item != NULL; item = item->next)
        if (item->kind == ROTMOS_LOOP_WATCH)
            ((rotmos_watch *)item)->registered = 0;
    rotmos_loop_shutdown(loop);
    (void)pthread_mutex_destroy(&loop->mutex);
    free(loop);
}

void rotmos_loop_discard_after_fork(rotmos_loop *loop)
{
    struct rotmos_loop_item *item;
    struct rotmos_loop_item *next;
    if (loop == NULL)
        return;
    for (item = loop->items; item != NULL; item = next) {
        next = item->next;
        if (item->kind == ROTMOS_LOOP_WATCH)
            loop->backend.watch_release(loop->backend.userdata,
                                        (rotmos_watch *)item);
        free(item);
    }
    (void)pthread_mutex_destroy(&loop->mutex);
    free(loop);
}
