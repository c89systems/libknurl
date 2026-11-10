#ifndef KNURL_KORE_INTERNAL_H
#define KNURL_KORE_INTERNAL_H

#include "library.h"
#include "http.h"
#include "curl.h"
#include "server_internal.h"
#include "request_internal.h"
#include "loop_internal.h"
#include "tls_internal.h"
#include "acme_internal.h"
#include <knurl/channel.h>

#include <setjmp.h>
#include <pthread.h>

struct knurl_runtime_shared {
    int serving;
    int acme_identity_valid;
    size_t acme_identity;
    size_t acme_domain;
    struct {
        int valid;
        knurl_error error;
    } failures[KORE_WORKER_MAX];
};

struct knurl_delivery;
struct knurl_log_runtime;

struct knurl_tls_binding {
    struct kore_domain *domain;
    size_t identity;
};

struct knurl_runtime {
    knurl_server *server;
    jmp_buf unwind;
    knurl_error error;
    knurl_status status;
    int initialized;
    int ready;
    int cleaning;
    int wake[2];
    struct kore_event wake_event;
    struct kore_native_callbacks callbacks;
    knurl_bound_listener *bound;
    struct kore_server **native_servers;
    char (*addresses)[46];
    size_t bound_count;
    int worker_started;
    int worker_stopped;
    pid_t parent_pid;
    pthread_t owner_thread;
    struct knurl_runtime_shared *shared;
    struct knurl_log_runtime *logging;
    int gate_selected;
    struct sigaction saved_signals[4];
    size_t installed_signals;
    int stop_sent;
    int cancelled;
    u_int16_t curl_timeout;
    u_int64_t curl_recv_max;
    knurl_watch *control_watch;
    struct knurl_delivery *responses;
    struct knurl_delivery *response_head;
    struct knurl_delivery *response_tail;
    size_t response_count;
    knurl_websocket *websockets;
    struct knurl_tls_prepared tls;
    char *acme_root;
    unsigned char *acme_ca;
    size_t acme_ca_length;
    char *acme_ca_directory;
    struct knurl_tls_binding *tls_bindings;
    size_t tls_binding_count;
    int *tls_installed;
    size_t tls_installed_bytes;
    int *tls_observed;
    int *tls_seen;
    int has_failure;
    knurl_worker_event last_failure;
    knurl_u64 recovered_failures;
    int recovery_pending[KORE_WORKER_MAX];
};

knurl_status knurl_runtime_hook(struct knurl_runtime *runtime,
                                knurl_process_fn function);
void knurl_kore_endpoint(int, int, knurl_endpoint *, char *);
int knurl_kore_accept(void *, struct connection *);
void knurl_kore_tcp_stall(void *, struct connection *);
void knurl_kore_tls_failure(void *, struct connection *);
void knurl_kore_http_status(void *, struct http_request *, int);
void knurl_kore_connection_status(void *, struct connection *, int);
int knurl_kore_prebody(struct http_request *native, const void *data,
                       size_t length);
int knurl_kore_dispatch(struct http_request *native);
void knurl_kore_request_free(struct http_request *native);
knurl_status knurl_kore_loop_create(struct knurl_runtime *runtime,
                                    knurl_error *error);
u_int64_t knurl_kore_loop_wait(void *userdata, u_int64_t maximum);
void knurl_kore_loop_turn(void *userdata);
void knurl_kore_parent_drain(void *userdata, u_int64_t maximum);
void knurl_kore_loop_shutdown(void *userdata);
void knurl_kore_loop_finish(struct knurl_runtime *runtime);
knurl_status knurl_kore_control_start(struct knurl_runtime *runtime,
                                      knurl_error *error);
void knurl_kore_control_close(struct knurl_runtime *runtime);
knurl_status knurl_kore_response_create(struct knurl_runtime *runtime,
                                        knurl_request *request,
                                        knurl_error *error);
void knurl_kore_response_progress(void *userdata, struct netbuf *buffer,
                                  size_t amount, int retry);
int knurl_kore_response_release(void *userdata, struct http_request *native);
void knurl_kore_responses_turn(struct knurl_runtime *runtime);
u_int64_t knurl_kore_responses_wait(struct knurl_runtime *runtime,
                                    u_int64_t maximum);
void knurl_kore_responses_shutdown(struct knurl_runtime *runtime);
knurl_status knurl_kore_body_create(knurl_request *request, const void *data,
                                    size_t length, knurl_error *error);
void knurl_kore_body_cancel(knurl_request *request, knurl_status cause);
void knurl_kore_body_step(knurl_request *request);
knurl_status knurl_kore_body_empty_reply(knurl_request *request,
                                         knurl_error *error);
void knurl_kore_body_timeout(knurl_request *request);
u_int64_t knurl_kore_body_wait(knurl_request *request, u_int64_t maximum);
void knurl_kore_request_fail(knurl_request *request, knurl_status cause,
                             int status);
void knurl_kore_request_fail_message(knurl_request *, knurl_status, int,
                                     const char *);
void knurl_kore_request_closed(knurl_request *request);
void knurl_kore_response_body_end(knurl_request *request);
void knurl_kore_body_initial(knurl_request *request, knurl_bytes *out);
void knurl_kore_body_transferred(knurl_request *request);
int knurl_kore_channel_flush_http(knurl_request *request);
void knurl_kore_channel_http_progress(struct connection *connection,
                                      size_t amount);
void knurl_kore_channels_stopping(struct knurl_runtime *runtime);
knurl_status knurl_kore_channel_prepare(knurl_request *,
                                        const knurl_channel_config *,
                                        knurl_channel **, knurl_error *);
void knurl_kore_channel_attach(knurl_channel *);
void knurl_kore_channel_detach(knurl_channel *);
void knurl_kore_websocket_handshake(knurl_request *, int);
void knurl_kore_websockets_turn(struct knurl_runtime *);
u_int64_t knurl_kore_websockets_wait(struct knurl_runtime *, u_int64_t);
void knurl_kore_websockets_stopping(struct knurl_runtime *);
void knurl_kore_websockets_shutdown(struct knurl_runtime *);
void knurl_kore_listener_domains(struct knurl_runtime *runtime,
                                 struct kore_server *server,
                                 const knurl_listener_config *listener);
int knurl_kore_redirect(knurl_request *request);
void knurl_kore_child_failure(struct knurl_runtime *runtime,
                              const knurl_error *error);
void knurl_kore_worker_event(void *userdata, const struct kore_worker *child,
                             int kind, int status, int expected, int abandoned);
knurl_status knurl_kore_acme_prepare(struct knurl_runtime *runtime,
                                     knurl_error *error);
void knurl_kore_acme_configure(struct knurl_runtime *runtime);
void knurl_kore_acme_clear(struct knurl_runtime *runtime);
int knurl_kore_acme_start(void *userdata);
int knurl_kore_acme_persist(void *userdata, const char *domain);
int knurl_kore_acme_request(void *userdata, struct kore_curl *request);
void knurl_kore_tls_updated(void *userdata, struct kore_domain *domain,
                            int usable);
knurl_status knurl_kore_tls_registry(struct knurl_runtime *runtime,
                                     knurl_error *error);
void knurl_kore_tls_worker(struct knurl_runtime *runtime, size_t index,
                           int starting);
void knurl_kore_tls_observe(struct knurl_runtime *runtime,
                            const knurl_error *failure);
void knurl_kore_tls_clear(struct knurl_runtime *runtime);

#endif
