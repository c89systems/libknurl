#ifndef ROTMOS_KORE_INTERNAL_H
#define ROTMOS_KORE_INTERNAL_H

#include "library.h"
#include "http.h"
#include "curl.h"
#include "server_internal.h"
#include "request_internal.h"
#include "loop_internal.h"
#include "tls_internal.h"

#include <setjmp.h>
#include <pthread.h>

struct rotmos_runtime_shared {
    int serving;
    struct {
        int valid;
        rotmos_error error;
    } failures[KORE_WORKER_MAX];
};

struct rotmos_delivery;

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
    struct rotmos_delivery *responses;
    struct rotmos_delivery *response_head;
    struct rotmos_delivery *response_tail;
    size_t response_count;
    struct rotmos_tls_prepared tls;
    int has_failure;
    rotmos_worker_event last_failure;
    rotmos_u64 recovered_failures;
    int recovery_pending[KORE_WORKER_MAX];
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
rotmos_status rotmos_kore_response_create(struct rotmos_runtime *runtime,
                                          rotmos_request *request,
                                          rotmos_error *error);
void rotmos_kore_response_progress(void *userdata, struct netbuf *buffer,
                                   size_t amount, int retry);
int rotmos_kore_response_release(void *userdata, struct http_request *native);
void rotmos_kore_responses_turn(struct rotmos_runtime *runtime);
u_int64_t rotmos_kore_responses_wait(struct rotmos_runtime *runtime,
                                     u_int64_t maximum);
void rotmos_kore_responses_shutdown(struct rotmos_runtime *runtime);
rotmos_status rotmos_kore_body_create(rotmos_request *request, const void *data,
                                      size_t length, rotmos_error *error);
void rotmos_kore_body_cancel(rotmos_request *request, rotmos_status cause);
void rotmos_kore_body_step(rotmos_request *request);
rotmos_status rotmos_kore_body_empty_reply(rotmos_request *request,
                                           rotmos_error *error);
void rotmos_kore_body_timeout(rotmos_request *request);
u_int64_t rotmos_kore_body_wait(rotmos_request *request, u_int64_t maximum);
void rotmos_kore_request_fail(rotmos_request *request, rotmos_status cause,
                              int status);
void rotmos_kore_request_closed(rotmos_request *request);
void rotmos_kore_response_body_end(rotmos_request *request);
void rotmos_kore_body_initial(rotmos_request *request, rotmos_bytes *out);
void rotmos_kore_body_transferred(rotmos_request *request);
int rotmos_kore_channel_flush_http(rotmos_request *request);
void rotmos_kore_channel_http_progress(struct connection *connection,
                                       size_t amount);
void rotmos_kore_channels_stopping(struct rotmos_runtime *runtime);
void rotmos_kore_listener_domains(struct rotmos_runtime *runtime,
                                  struct kore_server *server,
                                  const rotmos_listener_config *listener);
int rotmos_kore_redirect(rotmos_request *request);
void rotmos_kore_child_failure(struct rotmos_runtime *runtime,
                               const rotmos_error *error);
void rotmos_kore_worker_event(void *userdata, const struct kore_worker *child,
                              int kind, int status, int expected,
                              int abandoned);

#endif
