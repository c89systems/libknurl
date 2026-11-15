#ifndef KNURL_LOOP_INTERNAL_H
#define KNURL_LOOP_INTERNAL_H

#include "internal.h"
#include <knurl/loop.h>
#include <pthread.h>

typedef enum knurl_loop_kind {
    KNURL_LOOP_WATCH,
    KNURL_LOOP_TIMER,
    KNURL_LOOP_DEFER,
    KNURL_LOOP_WAKER
} knurl_loop_kind;

struct knurl_loop_item {
    void *owner;
    knurl_loop *loop;
    knurl_loop_kind kind;
    knurl_retire_fn retired;
    void *userdata;
    struct knurl_loop_item *next;
    struct knurl_loop_item *previous;
    struct knurl_loop_item *ready_next;
    struct knurl_loop_item *ready_previous;
    struct knurl_loop_item *retire_next;
    int queued;
    int cancelled;
};

struct knurl_watch_impl {
    knurl_watch receiver;
    struct knurl_loop_item item;
    int fd;
    unsigned int interest;
    unsigned int registered;
    unsigned int ready;
    knurl_watch_fn callback;
    void *backend;
};

struct knurl_timer_impl {
    knurl_timer receiver;
    struct knurl_loop_item item;
    knurl_u64 due;
    long repeat;
    knurl_timer_fn callback;
    knurl_timer *timer_next;
    knurl_timer *timer_previous;
    int scheduled;
};

struct knurl_waker_impl {
    knurl_waker receiver;
    struct knurl_loop_item item;
    knurl_waker_fn callback;
};

struct knurl_deferred {
    struct knurl_loop_item item;
    knurl_defer_fn callback;
};

struct knurl_loop_backend {
    void *userdata;
    knurl_status (*watch_update)(void *, knurl_watch *, unsigned int,
                                 unsigned int, knurl_error *);
    void (*watch_release)(void *, knurl_watch *);
    void (*wake)(void *);
};

struct knurl_executor;
struct knurl_storage_operation;
struct knurl_loop_impl {
    knurl_loop receiver;
    unsigned long identity;
    struct knurl_executor *executors;
    struct knurl_executor *storage_executor;
    struct knurl_executor *io_executor;
    struct knurl_storage_operation *storage_operations;
    size_t executor_count, storage_count;
    int executor_stopping, http_worker;
    long process_id;
    pthread_t owner;
    pthread_mutex_t mutex;
    struct knurl_loop_backend backend;
    struct knurl_loop_item *items;
    struct knurl_loop_item *ready_head;
    struct knurl_loop_item *ready_tail;
    struct knurl_loop_item *batch_head;
    struct knurl_loop_item *batch_tail;
    struct knurl_loop_item *retire_head;
    struct knurl_loop_item *retire_tail;
    knurl_timer *timers;
    size_t count;
    size_t deferred_count;
    size_t ready_count;
    size_t batch_count;
    int accepting;
    int native_ready;
    int in_turn;
    knurl_error failure;
};

knurl_status knurl_loop_create(const struct knurl_loop_backend *backend,
                               knurl_loop **out, knurl_error *error);
knurl_status knurl_loop_activate(knurl_loop *loop, knurl_error *error);
void knurl_loop_watch_ready(knurl_watch *watch, unsigned int flags);
void knurl_loop_turn(knurl_loop *loop);
long knurl_loop_wait(knurl_loop *loop);
int knurl_loop_pending(knurl_loop *loop);
void knurl_loop_shutdown(knurl_loop *loop);
void knurl_loop_release(knurl_loop *loop);
void knurl_loop_discard_after_fork(knurl_loop *loop);
knurl_status knurl_loop_owner(const knurl_loop *loop, knurl_error *error);
knurl_status knurl_loop_monotonic(knurl_u64 *out, knurl_error *error);
void knurl_loop_ready(struct knurl_loop_item *item);
void knurl_loop_cancel(struct knurl_loop_item *item);
knurl_status knurl_loop_accept(knurl_loop *loop, struct knurl_loop_item *item,
                               void *owner, knurl_loop_kind kind,
                               knurl_retire_fn retired, void *userdata,
                               knurl_error *error);
void knurl_loop_timer_remove(knurl_timer *timer);
void knurl_loop_timer_schedule(knurl_timer *timer);
knurl_status knurl_loop_deadline(knurl_u64 now, long delay, knurl_u64 *out,
                                 knurl_error *error);

#endif
