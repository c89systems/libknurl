#ifndef ROTMOS_SERVER_H
#define ROTMOS_SERVER_H

/** @file server.h Public server API. Expected failures return rotmos_status and
 * optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/http.h>
#include <rotmos/request.h>
#include <rotmos/tls.h>
#include <rotmos/dispatch.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for server state. */
typedef enum rotmos_server_state {
    /** Server lifecycle phase: declared. */
    ROTMOS_SERVER_DECLARED = 0,
    /** Server lifecycle phase: starting. */
    ROTMOS_SERVER_STARTING,
    /** Server lifecycle phase: ready paused. */
    ROTMOS_SERVER_READY_PAUSED,
    /** Server lifecycle phase: serving. */
    ROTMOS_SERVER_SERVING,
    /** Server lifecycle phase: stopping. */
    ROTMOS_SERVER_STOPPING,
    /** Server lifecycle phase: stopped. */
    ROTMOS_SERVER_STOPPED,
    /** Server lifecycle phase: failed. */
    ROTMOS_SERVER_FAILED
} rotmos_server_state;

/** Values for process role. */
typedef enum rotmos_process_role {
    /** Server process role: parent. */
    ROTMOS_PROCESS_PARENT = 0,
    /** Server process role: http. */
    ROTMOS_PROCESS_HTTP,
    /** Server process role: key manager. */
    ROTMOS_PROCESS_KEY_MANAGER,
    /** Server process role: acme. */
    ROTMOS_PROCESS_ACME
} rotmos_process_role;

/** Values for run reason. */
typedef enum rotmos_run_reason {
    /** Server run termination reason: requested stop. */
    ROTMOS_RUN_REQUESTED_STOP = 0,
    /** Server run termination reason: terminating signal. */
    ROTMOS_RUN_TERMINATING_SIGNAL,
    /** Server run termination reason: startup failure. */
    ROTMOS_RUN_STARTUP_FAILURE,
    /** Server run termination reason: backend failure. */
    ROTMOS_RUN_BACKEND_FAILURE
} rotmos_run_reason;

/** Active-run address/port view, including OS-selected ephemeral ports. Its
 * address storage is borrowed from the server.
 */
typedef struct rotmos_bound_listener {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Numeric address byte view, borrowed from its owner. */
    rotmos_bytes address;
    /** TCP port; zero permits an ephemeral bound port where supported. */
    unsigned short port;
    /** Nonzero when this downstream connection uses TLS. */
    int tls;
} rotmos_bound_listener;

/** Process role snapshot borrowed during host callbacks; worker_index is zero-
 * based.
 */
struct rotmos_process_context {
    /** Parent, HTTP worker, key manager or ACME process role. */
    rotmos_process_role role;
    /** Operating-system process identifier. */
    long process_id;
    /** Zero-based HTTP worker index when applicable. */
    unsigned int worker_index;
    /** Nonzero for a restarted child process. */
    int restarted;
};

/** Values for listener kind. */
typedef enum rotmos_listener_kind {
    /** Listener transport: http. */
    ROTMOS_LISTENER_HTTP = 0,
    /** Listener transport: https. */
    ROTMOS_LISTENER_HTTPS,
    /** Listener transport: redirect. */
    ROTMOS_LISTENER_REDIRECT
} rotmos_listener_kind;

/** Configuration for listener. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_listener_config {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Listener bind address copied with server declarations. */
    const char *bind;
    /** TCP port; zero permits an ephemeral bound port where supported. */
    unsigned short port;
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    rotmos_listener_kind kind;
    /** Zero-based TLS identity index for an HTTPS listener. */
    size_t identity_index;
    /** Externally presented listener port for generated URLs/redirects. */
    unsigned short presented_port;
    /** HTTPS destination port of a redirect listener. */
    unsigned short redirect_https_port;
} rotmos_listener_config;

/** Milliseconds plus explicit disable. Zero duration inherits the owning
 * operation default, rather than disabling it.
 */
typedef struct rotmos_deadline {
    /** Deadline duration; zero inherits the documented default unless disabled.
     */
    long milliseconds;
    /** Nonzero disables this deadline; independent of zero-value inheritance.
     */
    int disabled;
} rotmos_deadline;

/** Values for worker death policy. */
typedef enum rotmos_worker_death_policy {
    /** Worker lifecycle policy/event: restart. */
    ROTMOS_WORKER_RESTART = 0,
    /** Worker lifecycle policy/event: terminate. */
    ROTMOS_WORKER_TERMINATE
} rotmos_worker_death_policy;

/** Configuration for worker. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_worker_config {
    /** Number of elements or requested worker processes, according to the
     * containing type.
     */
    size_t count;
    /** Worker accept distribution threshold. */
    size_t accept_threshold;
    /** Requested worker open-file limit. */
    size_t rlimit_nofiles;
    /** Nonzero disables worker CPU affinity. */
    int affinity_disabled;
    /** Grace period in milliseconds before shutdown escalation. */
    long shutdown_timeout_ms;
    /** Restart a failed worker or terminate the server run. */
    rotmos_worker_death_policy death_policy;
} rotmos_worker_config;

/** Configuration for limit. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_limit_config {
    /** Maximum concurrent downstream connections. */
    size_t max_connections;
    /** Backend HTTP request capacity. */
    size_t request_limit;
    /** Maximum accepted downstream request-head bytes. */
    size_t max_request_header_bytes;
    /** Optional final-response-head ceiling; zero adds no ceiling. */
    size_t max_response_header_bytes; /** Zero adds no response-head ceiling. */
    /** Default route body admission ceiling; resolved by the server
     * initializer.
     */
    rotmos_u64 default_body_bytes;
    /** Server-wide body ceiling, including route-specific overrides. */
    rotmos_u64 hard_body_bytes;
    /** Transport spool directory; copied declaration, files remain request-
     * scoped.
     */
    const char *body_spool_directory;
    /** Minimum admitted request-body delivery rate in bytes/second. */
    rotmos_u64 body_min_rate_bytes_per_second;
    /** Nonzero disables minimum request-body rate enforcement. */
    int body_min_rate_disabled;
    /** Maximum accepted WebSocket frame payload bytes. */
    size_t websocket_max_frame_bytes;
} rotmos_limit_config;

/** Configuration for timeout. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_timeout_config {
    /** Server startup/readiness deadline. */
    rotmos_deadline startup;
    /** Downstream request-head receive deadline. */
    rotmos_deadline request_header;
    /** Request-body no-progress deadline. */
    rotmos_deadline request_body_idle;
    /** Downstream output no-progress deadline. */
    rotmos_deadline response_write_idle;
    /** Grace period before minimum-body-rate enforcement. */
    rotmos_deadline body_min_rate_grace;
    /** Idle downstream connection deadline. */
    rotmos_deadline connection_idle;
    /** HTTP keepalive idle deadline. */
    rotmos_deadline keepalive;
    /** Application WebSocket idle deadline. */
    rotmos_deadline websocket_idle;
    /** Deferred application response deadline; Vectis adapters and the Lua
     * facade explicitly disable the extra deadline by default.
     */
    rotmos_deadline deferred_response;
} rotmos_timeout_config;

/** Configuration for http. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_http_config {
    /** Copied extension overrides for every static file/directory/asset mount;
     * route overrides win. Zero count inherits built-in MIME inference.
     */
    const rotmos_mime_type *mime_types;
    /** Number of entries in mime_types; nonzero requires a non-NULL array. */
    size_t mime_type_count;
    /** Copied Server response-header value; defaults to "rotmos", independently
     * of the instance name. Must be a valid HTTP header value of 1..53 bytes.
     */
    const char *server_header;
    /** Independent file access-log path; unaffected by structured logger
     * disable.
     */
    const char *access_log_path;
    /** File access-log rotation threshold; zero inherits 10 MiB. Complete
     * records are preserved, so one oversized record may exceed this value.
     */
    rotmos_u64 access_log_max_bytes;
    /** Total files including the active file; zero inherits three. One keeps
     * only the active file, truncating it at rotation boundaries.
     */
    size_t access_log_file_count;
    /** Nonzero explicitly disables size-based file access-log rotation. */
    int access_log_rotation_disabled;
    /** Nonzero disables HTTP connection reuse. */
    int keepalive_disabled;
    /** Maximum requests per reusable connection. */
    unsigned int keepalive_max_requests;
    /** Listener accept backlog. */
    unsigned int socket_backlog;
    /** Owner-loop request-processing budget in milliseconds. */
    unsigned int request_process_budget_ms;
    /** Strict-Transport-Security max-age in seconds. */
    unsigned long hsts_max_age_seconds;
    /** Kore backend curl timeout, distinct from reverse-proxy deadlines. */
    unsigned long backend_curl_timeout_seconds;
    /** Kore backend curl receive ceiling, distinct from live proxy queues. */
    rotmos_u64 backend_curl_recv_max_bytes;
    /** Nonzero selects quiet backend logging. */
    int backend_quiet;
    /** Nonzero selects formatted backend error pages. */
    int pretty_error_pages;
} rotmos_http_config;

/** Infer a filename's Content-Type using this instance's overrides, then
 * built-ins, then application/octet-stream. NULL server uses only built-ins.
 * No filesystem access. Result is borrowed until server destruction (or has
 * static lifetime for built-ins); path is a filename/path, not a URL.
 */
ROTMOS_API const char *rotmos_server_content_type(const rotmos_server *,
                                                  const char *path);

/** Configuration for io. Use the owning operation's initializer before applying
 * overrides.
 */
typedef struct rotmos_io_config {
    /** Bounded transport/generation chunk size in bytes. */
    size_t chunk_bytes;
    /** Bounded downstream input queue capacity in bytes. */
    size_t input_queue_bytes;
    /** HTTP output queue capacity in bytes; zero inherits 256 KiB. Resolved
     * capacity must be at least 33 bytes and must hold chunk_bytes.
     */
    size_t output_queue_bytes;
} rotmos_io_config;

/** Values for signal mode. */
typedef enum rotmos_signal_mode {
    /** The embedding host owns signal delivery and calls the public notifier.
     */
    ROTMOS_SIGNALS_HOST = 0,
    /** Install server-owned signal handling for the active run. */
    ROTMOS_SIGNALS_LIBRARY
} rotmos_signal_mode;

/** Configuration for signal. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_signal_config {
    /** Selected policy mode; use the enclosing initializer for defaults. */
    rotmos_signal_mode mode;
} rotmos_signal_config;

/** Synchronous host lifecycle callback in the indicated process. Return a
 * portable status with bounded error detail; process context is borrowed for
 * the call.
 */
typedef rotmos_status (*rotmos_process_fn)(
    rotmos_server *server, const rotmos_process_context *process,
    void *userdata, rotmos_error *error);
/** Terminal callback after native teardown, in the exiting fork only. The
 * context is a borrowed snapshot. It may destroy this fork's server/host state;
 * Core does not access either after return. Nonzero becomes the exit status. */
typedef int (*rotmos_worker_exit_fn)(rotmos_server *server,
                                     const rotmos_process_context *process,
                                     void *userdata);

/** Values for worker event kind. */
typedef enum rotmos_worker_event_kind {
    /** Worker lifecycle policy/event: started. */
    ROTMOS_WORKER_STARTED = 0,
    /** Worker lifecycle policy/event: exited. */
    ROTMOS_WORKER_EXITED,
    /** Worker lifecycle policy/event: restarted. */
    ROTMOS_WORKER_RESTARTED,
    /** Worker lifecycle policy/event: escalated. */
    ROTMOS_WORKER_ESCALATED
} rotmos_worker_event_kind;

/** Copied child lifecycle/failure snapshot supplied to parent observations. */
typedef struct rotmos_worker_event {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    rotmos_worker_event_kind kind;
    /** Copied process-role snapshot; contains no backend-owned process
     * pointers.
     */
    rotmos_process_context process;
    /** Observed child exit code when applicable. */
    int exit_status;
    /** Observed/requested operating-system signal number. */
    int signal_number;
    /** Nonzero when shutdown could not await normal child completion. */
    int abandoned;
    /** Nonzero for an expected child exit during shutdown. */
    int expected;
    /** Copied bounded diagnostic; OK/NONE when no error is attached. */
    rotmos_error error;
} rotmos_worker_event;

/** Terminal run result after process/native cleanup, including recovered
 * failures and the last child failure when present.
 */
typedef struct rotmos_run_result {
    /** Terminal reason for leaving the server run. */
    rotmos_run_reason reason;
    /** Observed/requested operating-system signal number. */
    int signal_number;
    /** Nonzero when shutdown required escalation. */
    int escalated;
    /** Nonzero when last_failure contains an observed child failure. */
    int has_failure;
    /** Most recent observed worker failure when has_failure is set. */
    rotmos_worker_event last_failure;
    /** Exact count of worker failures recovered by restart. */
    rotmos_u64 recovered_failures;
} rotmos_run_result;

/** Parent-loop observation of child start/exit/restart/escalation. The event
 * snapshot is borrowed only for this call.
 */
typedef void (*rotmos_worker_event_fn)(rotmos_server *server,
                                       const rotmos_worker_event *event,
                                       void *userdata);

/** Connection info. */
typedef struct rotmos_connection_info {
    /** Real downstream TCP peer endpoint, independent of forwarded-address
     * policy.
     */
    rotmos_endpoint peer;
    /** Accepted local endpoint. */
    rotmos_endpoint local;
    /** Zero-based configured listener index. */
    size_t listener_index;
    /** Copied process-role snapshot; contains no backend-owned process
     * pointers.
     */
    rotmos_process_context process;
    /** Nonzero when this downstream connection uses TLS. */
    int tls;
} rotmos_connection_info;
/** Values for observation kind. */
typedef enum rotmos_observation_kind {
    /** Owner-loop observation: connection accepted. */
    ROTMOS_OBSERVE_CONNECTION_ACCEPTED = 0,
    /** Owner-loop observation: tcp stall. */
    ROTMOS_OBSERVE_TCP_STALL,
    /** Owner-loop observation: tls failure. */
    ROTMOS_OBSERVE_TLS_FAILURE,
    /** Owner-loop observation: http status. */
    ROTMOS_OBSERVE_HTTP_STATUS,
    /** Owner-loop observation: request started. */
    ROTMOS_OBSERVE_REQUEST_STARTED,
    /** Owner-loop observation: request rejected. */
    ROTMOS_OBSERVE_REQUEST_REJECTED,
    /** Owner-loop observation: response committed. */
    ROTMOS_OBSERVE_RESPONSE_COMMITTED,
    /** Owner-loop observation: request complete. */
    ROTMOS_OBSERVE_REQUEST_COMPLETE,
    /** Owner-loop observation: application. */
    ROTMOS_OBSERVE_APPLICATION
} rotmos_observation_kind;
/** Values for disconnect side. */
typedef enum rotmos_disconnect_side {
    /** Disconnect attribution: unknown. */
    ROTMOS_DISCONNECT_UNKNOWN = 0,
    /** Disconnect attribution: downstream. */
    ROTMOS_DISCONNECT_DOWNSTREAM,
    /** Disconnect attribution: upstream. */
    ROTMOS_DISCONNECT_UPSTREAM,
    /** Disconnect attribution: host. */
    ROTMOS_DISCONNECT_HOST
} rotmos_disconnect_side;
/** Borrowed owner-loop observation. Transport events have request NULL. Final
 * counters exist only with has_outcome; early rejection/takeover may leave
 * input_counts_known false.
 */
typedef struct rotmos_observation {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    rotmos_observation_kind kind;
    /** Copied accepted-connection metadata for this observation. */
    rotmos_connection_info connection;
    /** Borrowed request handle; NULL for transport-only observations. */
    rotmos_request *request;
    /** Resolved client address; does not replace the real TCP peer. */
    rotmos_bytes effective_address;
    /** Host-resolved request identifier; empty when no resolver is configured.
     */
    rotmos_bytes correlation_id;
    /** Name of this declaration, field or listed resource. */
    rotmos_bytes name;
    /** HTTP status code; zero means unknown/unset where documented by this
     * event/builder.
     */
    int status;
    /** Nonzero when output or a storage mutation has crossed its commitment
     * point.
     */
    int committed;
    /** Nonzero when final request counts/duration are available. */
    int has_outcome;
    /** Nonzero when managed input accounting is available; early
     * rejection/takeover may not supply it.
     */
    int input_counts_known;
    /** Portable terminal status; OK indicates successful completion. */
    rotmos_status cause;
    /** Downstream/upstream/host attribution when known. */
    rotmos_disconnect_side disconnect_side;
    /** Borrowed or copied terminal accounting according to the containing
     * callback/result.
     */
    rotmos_request_outcome outcome;
} rotmos_observation;
/** Borrowed snapshots, owner-loop only. Transport observations have
 * request=NULL and always identify the real TCP peer. Final counts/duration are
 * available only when has_outcome is true; input_counts_known distinguishes
 * admitted managed input from early rejection/takeover. Unknown status is zero.
 * Observers cannot perform request/response I/O or recursively emit events. */
typedef void (*rotmos_observe_fn)(rotmos_server *, const rotmos_observation *,
                                  void *);
/** Owner-loop metadata callback after identity resolution. May associate
 * application userdata; cannot begin request/response I/O.
 */
typedef rotmos_status (*rotmos_request_start_fn)(rotmos_server *,
                                                 rotmos_request *, void *,
                                                 rotmos_error *);
/** Once-per-initialized-managed-request callback after plan/response cleanup
 * and final observation, including failed admission/start. Metadata is valid
 * during the call; native source acknowledgements may finish later.
 */
typedef void (*rotmos_request_end_fn)(rotmos_server *, rotmos_request *,
                                      const rotmos_request_outcome *, void *);
/** Pre-body admission decision; ALLOW is the initialized default. Connection
 * rejection closes before TLS, request rejection uses the staged reply.
 */
typedef enum rotmos_admission_action {
    /** Admission decision: allow. */
    ROTMOS_ADMISSION_ALLOW = 0,
    /** Admission decision: reject. */
    ROTMOS_ADMISSION_REJECT,
    /** Admission decision: close. */
    ROTMOS_ADMISSION_CLOSE
} rotmos_admission_action;
/** Before-TLS admission using a borrowed real-TCP connection snapshot. Default
 * ALLOW; REJECT/CLOSE closes without HTTP output.
 */
typedef rotmos_status (*rotmos_connection_admit_fn)(
    rotmos_server *, const rotmos_connection_info *, rotmos_admission_action *,
    void *, rotmos_error *);
/** Pre-body request admission. Default ALLOW; REJECT uses staged copied bytes
 * (default 403), CLOSE sends nothing. File/source payloads and bodies beyond
 * the output bound fail.
 */
typedef rotmos_status (*rotmos_request_admit_fn)(rotmos_request *,
                                                 rotmos_reply *,
                                                 rotmos_admission_action *,
                                                 void *, rotmos_error *);
/** Configuration for admission. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_admission_config {
    /** Admission callback before TLS work; rejection closes the TCP connection.
     */
    rotmos_connection_admit_fn on_connection;
    /** Admission callback after metadata resolution, before body/proxy
     * allocation.
     */
    rotmos_request_admit_fn on_request;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_admission_config;
/** Host process/lifecycle and owner-loop request observers. Callback userdata
 * remains caller-owned through server destruction.
 */
typedef struct rotmos_server_callbacks {
    /** Initial callback in each process; use it to invalidate inherited host
     * handles.
     */
    rotmos_process_fn on_process_start;
    /** Parent preparation callback before child startup. */
    rotmos_process_fn on_parent_prepare;
    /** Parent readiness callback after child/listener startup. */
    rotmos_process_fn on_parent_ready;
    /** Parent callback when shutdown begins. */
    rotmos_process_fn on_parent_stopping;
    /** Parent callback after children and native resources stop. */
    rotmos_process_fn on_parent_stopped;
    /** HTTP worker callback after its owner loop is available. */
    rotmos_process_fn on_worker_start;
    /** HTTP worker callback when shutdown begins. */
    rotmos_process_fn on_worker_stopping;
    /** Terminal callback in an exiting child after native teardown; may destroy
     * that fork's host/server state.
     */
    rotmos_worker_exit_fn on_worker_exit;
    /** Parent observation of child start/exit/restart/escalation. */
    rotmos_worker_event_fn on_worker_event;
    /** TLS identity availability/failure observer with a copied public event.
     */
    rotmos_tls_identity_fn on_tls_identity;
    /** Metadata-only callback after identity resolution; may associate host
     * userdata, but cannot begin I/O.
     */
    rotmos_request_start_fn on_request_start;
    /** Once-per-managed-request terminal callback after plan/response cleanup;
     * native acknowledgements may finish later.
     */
    rotmos_request_end_fn on_request_close;
    /** Owner-loop observation callback; cannot perform request I/O or
     * recursively emit events.
     */
    rotmos_observe_fn on_observe;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_server_callbacks;

/** Host control-FD read readiness bit. */
#define ROTMOS_CONTROL_READ 1U
/** Host control-FD write readiness bit. */
#define ROTMOS_CONTROL_WRITE 2U
/** Host control-FD end-of-input bit. */
#define ROTMOS_CONTROL_EOF 4U
/** Host control-FD error bit. */
#define ROTMOS_CONTROL_ERROR 8U

/** Owner-loop callback for the transferred host control FD; events uses
 * ROTMOS_CONTROL_* bits.
 */
typedef rotmos_status (*rotmos_control_fn)(rotmos_server *server,
                                           unsigned int events, void *userdata,
                                           rotmos_error *error);

/** Configuration for supervision. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_supervision_config {
    /** Nonzero enables this optional facility. */
    int enabled;
    /** Host control descriptor; successful supervision_take transfers cleanup
     * ownership.
     */
    int control_fd;
    /** Readiness event mask; zero suspends delivery where supported. */
    unsigned int interest;
    /** Owner-loop readiness/event callback. */
    rotmos_control_fn on_event;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_supervision_config;

/** Maximum resolved correlation identifier length in bytes, excluding any
 * trailing NUL.
 */
#define ROTMOS_CORRELATION_ID_MAX 128U
/** Write into the supplied bounded buffer and return its length (no trailing
 * NUL required). Address resolvers return a numeric IPv4/IPv6 address;
 * correlation IDs may be empty but cannot contain NUL/control bytes. */
typedef rotmos_status (*rotmos_request_resolve_fn)(rotmos_request *, char *,
                                                   size_t capacity,
                                                   size_t *length, void *,
                                                   rotmos_error *);
/** Configuration for identifier. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_identifier_config {
    /** Host resolver callback; its bounded output is copied before return. */
    rotmos_request_resolve_fn resolve;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_identifier_config;
/** Configuration for client ip. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_client_ip_config {
    /** Trusted numeric proxy addresses/CIDRs; arbitrary forwarded addresses
     * remain untrusted.
     */
    const char *const *trusted_proxies;
    /** Number of entries in trusted_proxies. */
    size_t trusted_proxy_count;
    /** Host resolver callback; its bounded output is copied before return. */
    rotmos_request_resolve_fn resolve;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_client_ip_config;

/** Autoblock status rule. */
typedef struct rotmos_autoblock_status_rule {
    /** HTTP status or operation status according to the containing type. */
    unsigned int status;
    /** Number of matching events within the configured window before blocking.
     */
    unsigned int threshold;
} rotmos_autoblock_status_rule;

/** Autoblock event rule. */
typedef struct rotmos_autoblock_event_rule {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Number of matching events within the configured window before blocking.
     */
    unsigned int threshold;
} rotmos_autoblock_event_rule;

/** Configuration for autoblock. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_autoblock_config {
    /** Nonzero enables this optional facility. */
    int enabled;
    /** Autoblock event counting window in seconds. */
    unsigned int window_seconds;
    /** Autoblock ban duration in seconds. */
    unsigned int block_seconds;
    /** Maximum shared autoblock address entries. */
    size_t max_entries;
    /** TCP-stall event threshold; zero disables this rule. */
    unsigned int tcp_stall_threshold;
    /** TLS-failure event threshold; zero disables this rule. */
    unsigned int tls_failure_threshold;
    /** Copied HTTP-status threshold rules. */
    const rotmos_autoblock_status_rule *status_rules;
    /** Number of status_rules entries. */
    size_t status_rule_count;
    /** Copied named-application-event threshold rules. */
    const rotmos_autoblock_event_rule *event_rules;
    /** Number of event_rules entries. */
    size_t event_rule_count;
} rotmos_autoblock_config;

/** Configuration for proxy resource. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct rotmos_proxy_resource_config {
    /** Zero inherits 16 active exchanges and four idle connections per pool. */
    size_t active_exchanges;
    /** Per-upstream idle connection capacity; zero selects four. */
    size_t idle_connections;
} rotmos_proxy_resource_config;

/** Configuration for server. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_server_config {
    /** Copied instance name; independent of http.server_header. */
    const char *name;
    /** Listener declarations copied during inert server construction. */
    const rotmos_listener_config *listeners;
    /** Number of listeners. */
    size_t listener_count;
    /** Worker count, limits and restart/shutdown policy. */
    rotmos_worker_config workers;
    /** Connection, header and request-body admission limits. */
    rotmos_limit_config limits;
    /** Server transport/lifecycle deadline declarations. */
    rotmos_timeout_config timeouts;
    /** HTTP/backend behavior settings. */
    rotmos_http_config http;
    /** Bounded transport buffer settings. */
    rotmos_io_config io;
    /** Shared reverse-proxy active/idle resource settings. */
    rotmos_proxy_resource_config proxy;
    /** Copied downstream TLS/identity/ACME declaration; material opener
     * userdata remains borrowed.
     */
    rotmos_tls_config tls;
    /** Host-owned or library-owned signal handling mode. */
    rotmos_signal_config signals;
    /** Host lifecycle/request observation callbacks; userdata remains borrowed.
     */
    rotmos_server_callbacks callbacks;
    /** Router or raw headers-callback dispatch selection. */
    rotmos_dispatch_config dispatch;
    /** Host control-FD supervision settings. */
    rotmos_supervision_config supervision;
    /** Trusted-proxy and optional address resolution policy. */
    rotmos_client_ip_config client_ip;
    /** Host-resolved request identifier; empty when no resolver is configured.
     */
    rotmos_identifier_config correlation_id;
    /** Connection/request admission callbacks, before body/proxy allocation. */
    rotmos_admission_config admission;
    /** Shared event-driven abuse blocking policy. */
    rotmos_autoblock_config autoblock;
    /** Nonzero holds the serving gate closed after readiness. */
    int start_paused;
} rotmos_server_config;

/** Normalized configuration view with resolved/rounded deadlines, borrowing
 * server-owned strings/arrays. requested_timeouts preserves inheritance/disable
 * declarations.
 */
typedef struct rotmos_server_effective {
    /** Resolved configuration borrowing the owning handle's storage. */
    rotmos_server_config config;
    /** Original zero-inheritance/disabled deadline choices before
     * normalization.
     */
    rotmos_timeout_config requested_timeouts;
    /** Original worker count before automatic resolution. */
    size_t requested_worker_count;
    /** Resolved maximum parsed request-header field count. */
    size_t max_header_fields;
    /** Borrowed name of the server's selected backend. */
    const char *backend_name;
} rotmos_server_effective;

/** Initialize deadline with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_deadline_init(rotmos_deadline *deadline);
/** Initialize listener config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_listener_config_init(rotmos_listener_config *config);
/** Initialize server config with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_server_config_init(rotmos_server_config *config);
/** Apply the production HTTP server profile; overrides should be applied after
 * this initializer.
 */
ROTMOS_API void
rotmos_server_config_init_production_webserver(rotmos_server_config *config);
/** Inert declaration copying; no callbacks, files, sockets, signals or forks.
 */
ROTMOS_API rotmos_status rotmos_server_new(const rotmos_server_config *config,
                                           rotmos_server **out,
                                           rotmos_error *error);
/** Copy normalized settings into a view borrowing server-owned storage;
 * includes requested timeout choices.
 */
ROTMOS_API rotmos_status rotmos_server_effective_config(
    const rotmos_server *server, rotmos_server_effective *out,
    rotmos_error *error);
/** Inspect the declared/run/stop lifecycle state without changing it. */
ROTMOS_API rotmos_server_state
rotmos_server_get_state(const rotmos_server *server);
/** Borrow the server-owned route registry until server destruction. */
ROTMOS_API rotmos_router *rotmos_server_router(rotmos_server *server);
/** Replace the server's inert router; transfer ownership only on success. */
ROTMOS_API rotmos_status rotmos_server_router_take(rotmos_server *server,
                                                   rotmos_router *router,
                                                   rotmos_error *error);
/** Attach host control-FD supervision; successful transfer owns FD cleanup,
 * failure retains caller ownership.
 */
ROTMOS_API rotmos_status rotmos_server_supervision_take(
    rotmos_server *server, const rotmos_supervision_config *config,
    rotmos_error *error);
/** Change control-descriptor interest on its owner loop; zero suspends
 * notifications.
 */
ROTMOS_API rotmos_status rotmos_server_control_interest(rotmos_server *server,
                                                        unsigned int interest,
                                                        rotmos_error *error);
/** Destroy an inactive server and owned declarations; rejects active or
 * reentrant destruction.
 */
ROTMOS_API rotmos_status rotmos_server_destroy(rotmos_server *server,
                                               rotmos_error *error);
/** Validate declarations and TLS material without binding listeners or forking;
 * material callbacks may run.
 */
ROTMOS_API rotmos_status rotmos_server_validate(rotmos_server *server,
                                                rotmos_error *error);
/** Run the server lifecycle until stop/failure; return the run outcome after
 * worker/native cleanup.
 */
ROTMOS_API rotmos_status rotmos_server_run(rotmos_server *server,
                                           rotmos_run_result *out,
                                           rotmos_error *error);
/** Request graceful stop of an active run; callable from another thread while
 * the server remains alive.
 */
ROTMOS_API rotmos_status rotmos_server_request_stop(rotmos_server *server,
                                                    rotmos_error *error);
/** Async-signal-safe during a run; the caller keeps the server alive. */
ROTMOS_API void rotmos_server_notify_signal(rotmos_server *server,
                                            int signal_number);
/** Open or pause the serving gate of an active run; preserve the separate
 * readiness state.
 */
ROTMOS_API rotmos_status rotmos_server_set_serving(rotmos_server *server,
                                                   int serving,
                                                   rotmos_error *error);
/** Borrows addresses acquired by the active run, including ephemeral ports. */
ROTMOS_API rotmos_status rotmos_server_listener_at(const rotmos_server *server,
                                                   size_t index,
                                                   rotmos_bound_listener *out,
                                                   rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
