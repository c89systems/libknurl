#ifndef ROTMOS_SERVER_H
#define ROTMOS_SERVER_H

#include <rotmos/http.h>
#include <rotmos/request.h>
#include <rotmos/tls.h>
#include <rotmos/dispatch.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rotmos_server_state {
    ROTMOS_SERVER_DECLARED = 0,
    ROTMOS_SERVER_STARTING,
    ROTMOS_SERVER_READY_PAUSED,
    ROTMOS_SERVER_SERVING,
    ROTMOS_SERVER_STOPPING,
    ROTMOS_SERVER_STOPPED,
    ROTMOS_SERVER_FAILED
} rotmos_server_state;

typedef enum rotmos_process_role {
    ROTMOS_PROCESS_PARENT = 0,
    ROTMOS_PROCESS_HTTP,
    ROTMOS_PROCESS_KEY_MANAGER,
    ROTMOS_PROCESS_ACME
} rotmos_process_role;

typedef enum rotmos_run_reason {
    ROTMOS_RUN_REQUESTED_STOP = 0,
    ROTMOS_RUN_TERMINATING_SIGNAL,
    ROTMOS_RUN_STARTUP_FAILURE,
    ROTMOS_RUN_BACKEND_FAILURE
} rotmos_run_reason;

typedef struct rotmos_bound_listener {
    const char *name;
    rotmos_bytes address;
    unsigned short port;
    int tls;
} rotmos_bound_listener;

struct rotmos_process_context {
    rotmos_process_role role;
    long process_id;
    unsigned int worker_index;
    int restarted;
};

typedef enum rotmos_listener_kind {
    ROTMOS_LISTENER_HTTP = 0,
    ROTMOS_LISTENER_HTTPS,
    ROTMOS_LISTENER_REDIRECT
} rotmos_listener_kind;

typedef struct rotmos_listener_config {
    const char *name;
    const char *bind;
    unsigned short port;
    rotmos_listener_kind kind;
    size_t identity_index;
    unsigned short presented_port;
    unsigned short redirect_https_port;
} rotmos_listener_config;

typedef struct rotmos_deadline {
    long milliseconds;
    int disabled;
} rotmos_deadline;

typedef enum rotmos_worker_death_policy {
    ROTMOS_WORKER_RESTART = 0,
    ROTMOS_WORKER_TERMINATE
} rotmos_worker_death_policy;

typedef struct rotmos_worker_config {
    size_t count;
    size_t accept_threshold;
    size_t rlimit_nofiles;
    int affinity_disabled;
    long shutdown_timeout_ms;
    rotmos_worker_death_policy death_policy;
} rotmos_worker_config;

typedef struct rotmos_limit_config {
    size_t max_connections;
    size_t request_limit;
    size_t max_request_header_bytes;
    size_t max_response_header_bytes; /* Zero adds no response-head ceiling. */
    rotmos_u64 default_body_bytes;
    rotmos_u64 hard_body_bytes;
    const char *body_spool_directory;
    rotmos_u64 body_min_rate_bytes_per_second;
    int body_min_rate_disabled;
    size_t websocket_max_frame_bytes;
} rotmos_limit_config;

typedef struct rotmos_timeout_config {
    rotmos_deadline startup;
    rotmos_deadline request_header;
    rotmos_deadline request_body_idle;
    rotmos_deadline response_write_idle;
    rotmos_deadline body_min_rate_grace;
    rotmos_deadline connection_idle;
    rotmos_deadline keepalive;
    rotmos_deadline websocket_idle;
    rotmos_deadline deferred_response;
} rotmos_timeout_config;

typedef struct rotmos_http_config {
    const char *server_header;
    const char *access_log_path;
    int keepalive_disabled;
    unsigned int keepalive_max_requests;
    unsigned int socket_backlog;
    unsigned int request_process_budget_ms;
    unsigned long hsts_max_age_seconds;
    unsigned long backend_curl_timeout_seconds;
    rotmos_u64 backend_curl_recv_max_bytes;
    int backend_quiet;
    int pretty_error_pages;
} rotmos_http_config;

typedef struct rotmos_io_config {
    size_t chunk_bytes;
    size_t input_queue_bytes;
    size_t output_queue_bytes;
} rotmos_io_config;

typedef enum rotmos_signal_mode {
    ROTMOS_SIGNALS_HOST = 0,
    ROTMOS_SIGNALS_LIBRARY
} rotmos_signal_mode;

typedef struct rotmos_signal_config {
    rotmos_signal_mode mode;
} rotmos_signal_config;

typedef rotmos_status (*rotmos_process_fn)(
    rotmos_server *server, const rotmos_process_context *process,
    void *userdata, rotmos_error *error);
typedef int (*rotmos_worker_exit_fn)(rotmos_server *server,
                                     const rotmos_process_context *process,
                                     void *userdata);

typedef enum rotmos_worker_event_kind {
    ROTMOS_WORKER_STARTED = 0,
    ROTMOS_WORKER_EXITED,
    ROTMOS_WORKER_RESTARTED,
    ROTMOS_WORKER_ESCALATED
} rotmos_worker_event_kind;

typedef struct rotmos_worker_event {
    rotmos_worker_event_kind kind;
    rotmos_process_context process;
    int exit_status;
    int signal_number;
    int abandoned;
    int expected;
    rotmos_error error;
} rotmos_worker_event;

typedef struct rotmos_run_result {
    rotmos_run_reason reason;
    int signal_number;
    int escalated;
    int has_failure;
    rotmos_worker_event last_failure;
    rotmos_u64 recovered_failures;
} rotmos_run_result;

typedef void (*rotmos_worker_event_fn)(rotmos_server *server,
                                       const rotmos_worker_event *event,
                                       void *userdata);

typedef struct rotmos_connection_info {
    rotmos_endpoint peer, local;
    size_t listener_index;
    rotmos_process_context process;
    int tls;
} rotmos_connection_info;
typedef enum rotmos_observation_kind {
    ROTMOS_OBSERVE_CONNECTION_ACCEPTED = 0,
    ROTMOS_OBSERVE_TCP_STALL,
    ROTMOS_OBSERVE_TLS_FAILURE,
    ROTMOS_OBSERVE_HTTP_STATUS,
    ROTMOS_OBSERVE_REQUEST_STARTED,
    ROTMOS_OBSERVE_REQUEST_REJECTED,
    ROTMOS_OBSERVE_RESPONSE_COMMITTED,
    ROTMOS_OBSERVE_REQUEST_COMPLETE,
    ROTMOS_OBSERVE_APPLICATION
} rotmos_observation_kind;
typedef enum rotmos_disconnect_side {
    ROTMOS_DISCONNECT_UNKNOWN = 0,
    ROTMOS_DISCONNECT_DOWNSTREAM,
    ROTMOS_DISCONNECT_UPSTREAM,
    ROTMOS_DISCONNECT_HOST
} rotmos_disconnect_side;
typedef struct rotmos_observation {
    rotmos_observation_kind kind;
    rotmos_connection_info connection;
    rotmos_request *request;
    rotmos_bytes effective_address, correlation_id, name;
    int status, committed, has_outcome, input_counts_known;
    rotmos_status cause;
    rotmos_disconnect_side disconnect_side;
    rotmos_request_outcome outcome;
} rotmos_observation;
/* Borrowed snapshots, owner-loop only. Transport observations have request=NULL
 * and always identify the real TCP peer. Final counts/duration are available
 * only when has_outcome is true; input_counts_known distinguishes admitted
 * managed input from early rejection/takeover. Unknown status is zero.
 * Observers cannot perform request/response I/O or recursively emit events. */
typedef void (*rotmos_observe_fn)(rotmos_server *, const rotmos_observation *,
                                  void *);
typedef rotmos_status (*rotmos_request_start_fn)(rotmos_server *,
                                                 rotmos_request *, void *,
                                                 rotmos_error *);
typedef void (*rotmos_request_end_fn)(rotmos_server *, rotmos_request *,
                                      const rotmos_request_outcome *, void *);
/* A start callback sees resolved identity, can associate application userdata,
 * and cannot begin I/O. Close follows response/plan cleanup and final
 * observation, with valid metadata, once per initialized managed request;
 * resolver/start/admission failure before dispatch is included. In-flight
 * native sources close after acknowledgement and may outlive this callback. */
typedef enum rotmos_admission_action {
    ROTMOS_ADMISSION_ALLOW = 0,
    ROTMOS_ADMISSION_REJECT,
    ROTMOS_ADMISSION_CLOSE
} rotmos_admission_action;
typedef rotmos_status (*rotmos_connection_admit_fn)(
    rotmos_server *, const rotmos_connection_info *, rotmos_admission_action *,
    void *, rotmos_error *);
typedef rotmos_status (*rotmos_request_admit_fn)(rotmos_request *,
                                                 rotmos_reply *,
                                                 rotmos_admission_action *,
                                                 void *, rotmos_error *);
typedef struct rotmos_admission_config {
    rotmos_connection_admit_fn on_connection;
    rotmos_request_admit_fn on_request;
    void *userdata;
} rotmos_admission_config;
/* ALLOW is the default. Connection REJECT/CLOSE closes before TLS work.
 * Request REJECT submits the supplied staged status/headers/copied bytes
 * (default 403); file/source payloads and bodies over the output bound fail.
 * CLOSE sends no HTTP reply. No body/proxy allocation precedes admission. */
typedef struct rotmos_server_callbacks {
    rotmos_process_fn on_process_start;
    rotmos_process_fn on_parent_prepare;
    rotmos_process_fn on_parent_ready;
    rotmos_process_fn on_parent_stopping;
    rotmos_process_fn on_parent_stopped;
    rotmos_process_fn on_worker_start;
    rotmos_process_fn on_worker_stopping;
    rotmos_worker_exit_fn on_worker_exit;
    rotmos_worker_event_fn on_worker_event;
    rotmos_tls_identity_fn on_tls_identity;
    rotmos_request_start_fn on_request_start;
    rotmos_request_end_fn on_request_close;
    rotmos_observe_fn on_observe;
    void *userdata;
} rotmos_server_callbacks;

#define ROTMOS_CONTROL_READ 1U
#define ROTMOS_CONTROL_WRITE 2U
#define ROTMOS_CONTROL_EOF 4U
#define ROTMOS_CONTROL_ERROR 8U

typedef rotmos_status (*rotmos_control_fn)(rotmos_server *server,
                                           unsigned int events, void *userdata,
                                           rotmos_error *error);

typedef struct rotmos_supervision_config {
    int enabled;
    int control_fd;
    unsigned int interest;
    rotmos_control_fn on_event;
    void *userdata;
} rotmos_supervision_config;

#define ROTMOS_CORRELATION_ID_MAX 128U
/* Write into the supplied bounded buffer and return its length (no trailing
 * NUL required). Address resolvers return a numeric IPv4/IPv6 address;
 * correlation IDs may be empty but cannot contain NUL/control bytes. */
typedef rotmos_status (*rotmos_request_resolve_fn)(rotmos_request *, char *,
                                                   size_t capacity,
                                                   size_t *length, void *,
                                                   rotmos_error *);
typedef struct rotmos_identifier_config {
    rotmos_request_resolve_fn resolve;
    void *userdata;
} rotmos_identifier_config;
typedef struct rotmos_client_ip_config {
    const char *const *trusted_proxies;
    size_t trusted_proxy_count;
    rotmos_request_resolve_fn resolve;
    void *userdata;
} rotmos_client_ip_config;

typedef struct rotmos_autoblock_status_rule {
    unsigned int status;
    unsigned int threshold;
} rotmos_autoblock_status_rule;

typedef struct rotmos_autoblock_event_rule {
    const char *name;
    unsigned int threshold;
} rotmos_autoblock_event_rule;

typedef struct rotmos_autoblock_config {
    int enabled;
    unsigned int window_seconds;
    unsigned int block_seconds;
    size_t max_entries;
    unsigned int tcp_stall_threshold;
    unsigned int tls_failure_threshold;
    const rotmos_autoblock_status_rule *status_rules;
    size_t status_rule_count;
    const rotmos_autoblock_event_rule *event_rules;
    size_t event_rule_count;
} rotmos_autoblock_config;

typedef struct rotmos_proxy_resource_config {
    /* Zero inherits 16 active exchanges and four idle connections per pool. */
    size_t active_exchanges;
    size_t idle_connections;
} rotmos_proxy_resource_config;

typedef struct rotmos_server_config {
    const char *name;
    const rotmos_listener_config *listeners;
    size_t listener_count;
    rotmos_worker_config workers;
    rotmos_limit_config limits;
    rotmos_timeout_config timeouts;
    rotmos_http_config http;
    rotmos_io_config io;
    rotmos_proxy_resource_config proxy;
    rotmos_tls_config tls;
    rotmos_signal_config signals;
    rotmos_server_callbacks callbacks;
    rotmos_dispatch_config dispatch;
    rotmos_supervision_config supervision;
    rotmos_client_ip_config client_ip;
    rotmos_identifier_config correlation_id;
    rotmos_admission_config admission;
    rotmos_autoblock_config autoblock;
    int start_paused;
} rotmos_server_config;

/* The config view borrows server-owned storage. Timeouts in config are resolved
 * and rounded; requested_timeouts preserves zero-inheritance/disable choices.
 */
typedef struct rotmos_server_effective {
    rotmos_server_config config;
    rotmos_timeout_config requested_timeouts;
    size_t requested_worker_count;
    size_t max_header_fields;
    const char *backend_name;
} rotmos_server_effective;

ROTMOS_API void rotmos_deadline_init(rotmos_deadline *deadline);
ROTMOS_API void rotmos_listener_config_init(rotmos_listener_config *config);
ROTMOS_API void rotmos_server_config_init(rotmos_server_config *config);
ROTMOS_API void
rotmos_server_config_init_production_webserver(rotmos_server_config *config);
/* Inert declaration copying; no callbacks, files, sockets, signals or forks. */
ROTMOS_API rotmos_status rotmos_server_new(const rotmos_server_config *config,
                                           rotmos_server **out,
                                           rotmos_error *error);
ROTMOS_API rotmos_status rotmos_server_effective_config(
    const rotmos_server *server, rotmos_server_effective *out,
    rotmos_error *error);
ROTMOS_API rotmos_server_state
rotmos_server_get_state(const rotmos_server *server);
ROTMOS_API rotmos_router *rotmos_server_router(rotmos_server *server);
ROTMOS_API rotmos_status rotmos_server_router_take(rotmos_server *server,
                                                   rotmos_router *router,
                                                   rotmos_error *error);
ROTMOS_API rotmos_status rotmos_server_supervision_take(
    rotmos_server *server, const rotmos_supervision_config *config,
    rotmos_error *error);
ROTMOS_API rotmos_status rotmos_server_control_interest(rotmos_server *server,
                                                        unsigned int interest,
                                                        rotmos_error *error);
ROTMOS_API rotmos_status rotmos_server_destroy(rotmos_server *server,
                                               rotmos_error *error);
ROTMOS_API rotmos_status rotmos_server_validate(rotmos_server *server,
                                                rotmos_error *error);
ROTMOS_API rotmos_status rotmos_server_run(rotmos_server *server,
                                           rotmos_run_result *out,
                                           rotmos_error *error);
ROTMOS_API rotmos_status rotmos_server_request_stop(rotmos_server *server,
                                                    rotmos_error *error);
/* Async-signal-safe during a run; the caller keeps the server alive. */
ROTMOS_API void rotmos_server_notify_signal(rotmos_server *server,
                                            int signal_number);
ROTMOS_API rotmos_status rotmos_server_set_serving(rotmos_server *server,
                                                   int serving,
                                                   rotmos_error *error);
/* Borrows addresses acquired by the active run, including ephemeral ports. */
ROTMOS_API rotmos_status rotmos_server_listener_at(const rotmos_server *server,
                                                   size_t index,
                                                   rotmos_bound_listener *out,
                                                   rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
