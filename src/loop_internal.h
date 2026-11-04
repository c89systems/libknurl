#ifndef ROTMOS_LOOP_INTERNAL_H
#define ROTMOS_LOOP_INTERNAL_H

#include "internal.h"
#include <rotmos/loop.h>
#include <pthread.h>

typedef enum rotmos_loop_kind {
    ROTMOS_LOOP_WATCH,
    ROTMOS_LOOP_TIMER,
    ROTMOS_LOOP_DEFER,
    ROTMOS_LOOP_WAKER
} rotmos_loop_kind;

struct rotmos_loop_item {
    rotmos_loop *loop;
    rotmos_loop_kind kind;
    rotmos_retire_fn retired;
    void *userdata;
    struct rotmos_loop_item *next;
    struct rotmos_loop_item *previous;
    struct rotmos_loop_item *ready_next;
    struct rotmos_loop_item *ready_previous;
    struct rotmos_loop_item *retire_next;
    int queued;
    int cancelled;
};

struct rotmos_watch {
    struct rotmos_loop_item item;
    int fd;
    unsigned int interest;
    unsigned int registered;
    unsigned int ready;
    rotmos_watch_fn callback;
    void *backend;
};

struct rotmos_timer {
    struct rotmos_loop_item item;
    rotmos_u64 due;
    long repeat;
    rotmos_timer_fn callback;
    rotmos_timer *timer_next;
    rotmos_timer *timer_previous;
    int scheduled;
};

struct rotmos_waker {
    struct rotmos_loop_item item;
    rotmos_waker_fn callback;
};

struct rotmos_deferred {
    struct rotmos_loop_item item;
    rotmos_defer_fn callback;
};

struct rotmos_loop_backend {
    void *userdata;
    rotmos_status (*watch_update)(void *, rotmos_watch *, unsigned int,
                                  unsigned int, rotmos_error *);
    void (*watch_release)(void *, rotmos_watch *);
    void (*wake)(void *);
};

struct rotmos_executor;
struct rotmos_storage_operation;
struct rotmos_loop {
    unsigned long identity;
    struct rotmos_executor *executors;
    struct rotmos_executor *storage_executor;
    struct rotmos_storage_operation *storage_operations;
    size_t executor_count, storage_count;
    int executor_stopping, http_worker;
    long process_id;
    pthread_t owner;
    pthread_mutex_t mutex;
    struct rotmos_loop_backend backend;
    struct rotmos_loop_item *items;
    struct rotmos_loop_item *ready_head;
    struct rotmos_loop_item *ready_tail;
    struct rotmos_loop_item *batch_head;
    struct rotmos_loop_item *batch_tail;
    struct rotmos_loop_item *retire_head;
    struct rotmos_loop_item *retire_tail;
    rotmos_timer *timers;
    size_t count;
    size_t deferred_count;
    size_t ready_count;
    size_t batch_count;
    int accepting;
    int native_ready;
    int in_turn;
    rotmos_error failure;
};

rotmos_status rotmos_loop_create(const struct rotmos_loop_backend *backend,
                                 rotmos_loop **out, rotmos_error *error);
rotmos_status rotmos_loop_activate(rotmos_loop *loop, rotmos_error *error);
void rotmos_loop_watch_ready(rotmos_watch *watch, unsigned int flags);
void rotmos_loop_turn(rotmos_loop *loop);
long rotmos_loop_wait(rotmos_loop *loop);
int rotmos_loop_pending(rotmos_loop *loop);
void rotmos_loop_shutdown(rotmos_loop *loop);
void rotmos_loop_release(rotmos_loop *loop);
void rotmos_loop_discard_after_fork(rotmos_loop *loop);
rotmos_status rotmos_loop_owner(const rotmos_loop *loop, rotmos_error *error);
rotmos_status rotmos_loop_monotonic(rotmos_u64 *out, rotmos_error *error);
void rotmos_loop_ready(struct rotmos_loop_item *item);
void rotmos_loop_cancel(struct rotmos_loop_item *item);
rotmos_status rotmos_loop_accept(rotmos_loop *loop,
                                 struct rotmos_loop_item *item,
                                 rotmos_loop_kind kind,
                                 rotmos_retire_fn retired, void *userdata,
                                 rotmos_error *error);
void rotmos_loop_timer_remove(rotmos_timer *timer);
void rotmos_loop_timer_schedule(rotmos_timer *timer);
rotmos_status rotmos_loop_deadline(rotmos_u64 now, long delay, rotmos_u64 *out,
                                   rotmos_error *error);

#endif
