#ifndef ROTMOS_KORE_INTERNAL_H
#define ROTMOS_KORE_INTERNAL_H

#include "library.h"
#include "http.h"
#include "curl.h"
#include "server_internal.h"
#include "request_internal.h"
#include "loop_internal.h"

#include <setjmp.h>
#include <pthread.h>

struct rotmos_runtime_shared {
    int serving;
};

struct rotmos_runtime {
    rotmos_server *server;
    jmp_buf unwind;
    rotmos_error error;
    rotmos_status status;
    int initialized;
    int ready;
    int cleaning;
    int wake[2];
    struct kore_event wake_event;
    struct kore_native_callbacks callbacks;
    rotmos_bound_listener *bound;
    struct kore_server **native_servers;
    char (*addresses)[46];
    size_t bound_count;
    int worker_started;
    int worker_stopped;
    pid_t parent_pid;
    pthread_t owner_thread;
    struct rotmos_runtime_shared *shared;
    int gate_selected;
    struct sigaction saved_signals[4];
    size_t installed_signals;
    int stop_sent;
    int cancelled;
    u_int16_t curl_timeout;
    u_int64_t curl_recv_max;
    rotmos_watch *control_watch;
};

rotmos_status rotmos_runtime_hook(struct rotmos_runtime *runtime,
                                  rotmos_process_fn function);
int rotmos_kore_prebody(struct http_request *native, const void *data,
                        size_t length);
int rotmos_kore_dispatch(struct http_request *native);
void rotmos_kore_request_free(struct http_request *native);
rotmos_status rotmos_kore_loop_create(struct rotmos_runtime *runtime,
                                      rotmos_error *error);
u_int64_t rotmos_kore_loop_wait(void *userdata, u_int64_t maximum);
void rotmos_kore_loop_turn(void *userdata);
void rotmos_kore_parent_drain(void *userdata, u_int64_t maximum);
void rotmos_kore_loop_shutdown(void *userdata);
void rotmos_kore_loop_finish(struct rotmos_runtime *runtime);
rotmos_status rotmos_kore_control_start(struct rotmos_runtime *runtime,
                                        rotmos_error *error);
void rotmos_kore_control_close(struct rotmos_runtime *runtime);

#endif
