#ifndef ROTMOS_SERVER_H
#define ROTMOS_SERVER_H

#include <rotmos/http.h>
#include <rotmos/tls.h>

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
} rotmos_worker_event;

typedef void (*rotmos_worker_event_fn)(rotmos_server *server,
                                       const rotmos_worker_event *event,
                                       void *userdata);

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

typedef struct rotmos_client_ip_config {
    const char *const *trusted_proxies;
    size_t trusted_proxy_count;
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

typedef struct rotmos_server_config {
    const char *name;
    const rotmos_listener_config *listeners;
    size_t listener_count;
    rotmos_worker_config workers;
    rotmos_limit_config limits;
    rotmos_timeout_config timeouts;
    rotmos_http_config http;
    rotmos_io_config io;
    rotmos_tls_config tls;
    rotmos_signal_config signals;
    rotmos_server_callbacks callbacks;
    rotmos_supervision_config supervision;
    rotmos_client_ip_config client_ip;
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
ROTMOS_API rotmos_status rotmos_server_destroy(rotmos_server *server,
                                               rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
