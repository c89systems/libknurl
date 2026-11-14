#ifndef KNURL_SERVER_H
#define KNURL_SERVER_H

/** @file server.h Public server API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/http.h>
#include <knurl/request.h>
#include <knurl/tls.h>
#include <knurl/dispatch.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for server state. */
typedef enum knurl_server_state {
    /** Server lifecycle phase: declared. */
    KNURL_SERVER_DECLARED = 0,
    /** Server lifecycle phase: starting. */
    KNURL_SERVER_STARTING,
    /** Server lifecycle phase: ready paused. */
    KNURL_SERVER_READY_PAUSED,
    /** Server lifecycle phase: serving. */
    KNURL_SERVER_SERVING,
    /** Server lifecycle phase: stopping. */
    KNURL_SERVER_STOPPING,
    /** Server lifecycle phase: stopped. */
    KNURL_SERVER_STOPPED,
    /** Server lifecycle phase: failed. */
    KNURL_SERVER_FAILED
} knurl_server_state;

/** Values for process role. */
typedef enum knurl_process_role {
    /** Server process role: parent. */
    KNURL_PROCESS_PARENT = 0,
    /** Server process role: http. */
    KNURL_PROCESS_HTTP,
    /** Server process role: key manager. */
    KNURL_PROCESS_KEY_MANAGER,
    /** Server process role: acme. */
    KNURL_PROCESS_ACME
} knurl_process_role;

/** Values for run reason. */
typedef enum knurl_run_reason {
    /** Server run termination reason: requested stop. */
    KNURL_RUN_REQUESTED_STOP = 0,
    /** Server run termination reason: terminating signal. */
    KNURL_RUN_TERMINATING_SIGNAL,
    /** Server run termination reason: startup failure. */
    KNURL_RUN_STARTUP_FAILURE,
    /** Server run termination reason: backend failure. */
    KNURL_RUN_BACKEND_FAILURE
} knurl_run_reason;

/** Active-run address/port view, including OS-selected ephemeral ports. Its
 * address storage is borrowed from the server.
 */
typedef struct knurl_bound_listener {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Numeric address byte view, borrowed from its owner. */
    knurl_bytes address;
    /** TCP port; zero permits an ephemeral bound port where supported. */
    unsigned short port;
    /** Nonzero when this downstream connection uses TLS. */
    int tls;
} knurl_bound_listener;

/** Process role snapshot borrowed during host callbacks; worker_index is zero-
 * based.
 */
struct knurl_process_context {
    /** Parent, HTTP worker, key manager or ACME process role. */
    knurl_process_role role;
    /** Operating-system process identifier. */
    long process_id;
    /** Zero-based HTTP worker index when applicable. */
    unsigned int worker_index;
    /** Nonzero for a restarted child process. */
    int restarted;
};

/** Values for listener kind. */
typedef enum knurl_listener_kind {
    /** Listener transport: http. */
    KNURL_LISTENER_HTTP = 0,
    /** Listener transport: https. */
    KNURL_LISTENER_HTTPS,
    /** Listener transport: redirect. */
    KNURL_LISTENER_REDIRECT
} knurl_listener_kind;

/** Configuration for listener. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_listener_config {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Listener bind address copied with server declarations. */
    const char *bind;
    /** TCP port; zero permits an ephemeral bound port where supported. */
    unsigned short port;
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    knurl_listener_kind kind;
    /** Zero-based TLS identity index for an HTTPS listener. */
    size_t identity_index;
    /** Externally presented listener port for generated URLs/redirects. */
    unsigned short presented_port;
    /** HTTPS destination port of a redirect listener. */
    unsigned short redirect_https_port;
} knurl_listener_config;

/** Milliseconds plus explicit disable. Zero duration inherits the owning
 * operation default, rather than disabling it.
 */
typedef struct knurl_deadline {
    /** Deadline duration; zero inherits the documented default unless disabled.
     */
    long milliseconds;
    /** Nonzero disables this deadline; independent of zero-value inheritance.
     */
    int disabled;
} knurl_deadline;

/** Values for worker death policy. */
typedef enum knurl_worker_death_policy {
    /** Worker lifecycle policy/event: restart. */
    KNURL_WORKER_RESTART = 0,
    /** Worker lifecycle policy/event: terminate. */
    KNURL_WORKER_TERMINATE
} knurl_worker_death_policy;

/** Configuration for worker. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_worker_config {
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
    knurl_worker_death_policy death_policy;
} knurl_worker_config;

/** Configuration for limit. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_limit_config {
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
    knurl_u64 default_body_bytes;
    /** Server-wide body ceiling, including route-specific overrides. */
    knurl_u64 hard_body_bytes;
    /** Transport spool directory; copied declaration, files remain request-
     * scoped.
     */
    const char *body_spool_directory;
    /** Minimum admitted request-body delivery rate in bytes/second. */
    knurl_u64 body_min_rate_bytes_per_second;
    /** Nonzero disables minimum request-body rate enforcement. */
    int body_min_rate_disabled;
    /** Maximum accepted WebSocket frame payload bytes. */
    size_t websocket_max_frame_bytes;
} knurl_limit_config;

/** Configuration for timeout. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_timeout_config {
    /** Server startup/readiness deadline. */
    knurl_deadline startup;
    /** Downstream request-head receive deadline. */
    knurl_deadline request_header;
    /** Request-body no-progress deadline. */
    knurl_deadline request_body_idle;
    /** Downstream output no-progress deadline. */
    knurl_deadline response_write_idle;
    /** Grace period before minimum-body-rate enforcement. */
    knurl_deadline body_min_rate_grace;
    /** Idle downstream connection deadline. */
    knurl_deadline connection_idle;
    /** HTTP keepalive idle deadline. */
    knurl_deadline keepalive;
    /** Application WebSocket idle deadline. */
    knurl_deadline websocket_idle;
    /** Deferred application response deadline; Vectis adapters and the Lua
     * facade explicitly disable the extra deadline by default.
     */
    knurl_deadline deferred_response;
} knurl_timeout_config;

/** Configuration for http. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_http_config {
    /** Copied extension overrides for every static file/directory/asset mount;
     * route overrides win. Zero count inherits built-in MIME inference.
     */
    const knurl_mime_type *mime_types;
    /** Number of entries in mime_types; nonzero requires a non-NULL array. */
    size_t mime_type_count;
    /** Copied Server response-header value; defaults to "knurl", independently
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
    knurl_u64 access_log_max_bytes;
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
    knurl_u64 backend_curl_recv_max_bytes;
    /** Nonzero selects quiet backend logging. */
    int backend_quiet;
    /** Nonzero selects formatted backend error pages. */
    int pretty_error_pages;
} knurl_http_config;

/** Infer a filename's Content-Type using this instance's overrides, then
 * built-ins, then application/octet-stream. NULL server uses only built-ins.
 * No filesystem access. Result is borrowed until server destruction (or has
 * static lifetime for built-ins); path is a filename/path, not a URL.
 */
KNURL_API const char *knurl_server_content_type(const knurl_server *,
                                                const char *path);

/** Configuration for io. Use the owning operation's initializer before applying
 * overrides.
 */
typedef struct knurl_io_config {
    /** Bounded transport/generation chunk size in bytes. */
    size_t chunk_bytes;
    /** Bounded downstream input queue capacity in bytes. */
    size_t input_queue_bytes;
    /** HTTP output queue capacity in bytes; zero inherits 256 KiB. Resolved
     * capacity must be at least 33 bytes and must hold chunk_bytes.
     */
    size_t output_queue_bytes;
} knurl_io_config;

/** Values for signal mode. */
typedef enum knurl_signal_mode {
    /** The embedding host owns signal delivery and calls the public notifier.
     */
    KNURL_SIGNALS_HOST = 0,
    /** Install server-owned signal handling for the active run. */
    KNURL_SIGNALS_LIBRARY
} knurl_signal_mode;

/** Configuration for signal. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_signal_config {
    /** Selected policy mode; use the enclosing initializer for defaults. */
    knurl_signal_mode mode;
} knurl_signal_config;

/** Synchronous host lifecycle callback in the indicated process. Return a
 * portable status with bounded error detail; process context is borrowed for
 * the call.
 */
typedef knurl_status (*knurl_process_fn)(knurl_server *server,
                                         const knurl_process_context *process,
                                         void *userdata, knurl_error *error);
/** Terminal callback after native teardown, in the exiting fork only. The
 * context is a borrowed snapshot. It may destroy this fork's server/host state;
 * Core does not access either after return. Nonzero becomes the exit status. */
typedef int (*knurl_worker_exit_fn)(knurl_server *server,
                                    const knurl_process_context *process,
                                    void *userdata);

/** Values for worker event kind. */
typedef enum knurl_worker_event_kind {
    /** Worker lifecycle policy/event: started. */
    KNURL_WORKER_STARTED = 0,
    /** Worker lifecycle policy/event: exited. */
    KNURL_WORKER_EXITED,
    /** Worker lifecycle policy/event: restarted. */
    KNURL_WORKER_RESTARTED,
    /** Worker lifecycle policy/event: escalated. */
    KNURL_WORKER_ESCALATED
} knurl_worker_event_kind;

/** Copied child lifecycle/failure snapshot supplied to parent observations. */
typedef struct knurl_worker_event {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    knurl_worker_event_kind kind;
    /** Copied process-role snapshot; contains no backend-owned process
     * pointers.
     */
    knurl_process_context process;
    /** Observed child exit code when applicable. */
    int exit_status;
    /** Observed/requested operating-system signal number. */
    int signal_number;
    /** Nonzero when shutdown could not await normal child completion. */
    int abandoned;
    /** Nonzero for an expected child exit during shutdown. */
    int expected;
    /** Copied bounded diagnostic; OK/NONE when no error is attached. */
    knurl_error error;
} knurl_worker_event;

/** Terminal run result after process/native cleanup, including recovered
 * failures and the last child failure when present.
 */
typedef struct knurl_run_result {
    /** Terminal reason for leaving the server run. */
    knurl_run_reason reason;
    /** Observed/requested operating-system signal number. */
    int signal_number;
    /** Nonzero when shutdown required escalation. */
    int escalated;
    /** Nonzero when last_failure contains an observed child failure. */
    int has_failure;
    /** Most recent observed worker failure when has_failure is set. */
    knurl_worker_event last_failure;
    /** Exact count of worker failures recovered by restart. */
    knurl_u64 recovered_failures;
} knurl_run_result;

/** Parent-loop observation of child start/exit/restart/escalation. The event
 * snapshot is borrowed only for this call.
 */
typedef void (*knurl_worker_event_fn)(knurl_server *server,
                                      const knurl_worker_event *event,
                                      void *userdata);

/** Connection info. */
typedef struct knurl_connection_info {
    /** Real downstream TCP peer endpoint, independent of forwarded-address
     * policy.
     */
    knurl_endpoint peer;
    /** Accepted local endpoint. */
    knurl_endpoint local;
    /** Zero-based configured listener index. */
    size_t listener_index;
    /** Copied process-role snapshot; contains no backend-owned process
     * pointers.
     */
    knurl_process_context process;
    /** Nonzero when this downstream connection uses TLS. */
    int tls;
} knurl_connection_info;
/** Values for observation kind. */
typedef enum knurl_observation_kind {
    /** Owner-loop observation: connection accepted. */
    KNURL_OBSERVE_CONNECTION_ACCEPTED = 0,
    /** Owner-loop observation: tcp stall. */
    KNURL_OBSERVE_TCP_STALL,
    /** Owner-loop observation: tls failure. */
    KNURL_OBSERVE_TLS_FAILURE,
    /** Owner-loop observation: http status. */
    KNURL_OBSERVE_HTTP_STATUS,
    /** Owner-loop observation: request started. */
    KNURL_OBSERVE_REQUEST_STARTED,
    /** Owner-loop observation: request rejected. */
    KNURL_OBSERVE_REQUEST_REJECTED,
    /** Owner-loop observation: response committed. */
    KNURL_OBSERVE_RESPONSE_COMMITTED,
    /** Owner-loop observation: request complete. */
    KNURL_OBSERVE_REQUEST_COMPLETE,
    /** Owner-loop observation: application. */
    KNURL_OBSERVE_APPLICATION
} knurl_observation_kind;
/** Values for disconnect side. */
typedef enum knurl_disconnect_side {
    /** Disconnect attribution: unknown. */
    KNURL_DISCONNECT_UNKNOWN = 0,
    /** Disconnect attribution: downstream. */
    KNURL_DISCONNECT_DOWNSTREAM,
    /** Disconnect attribution: upstream. */
    KNURL_DISCONNECT_UPSTREAM,
    /** Disconnect attribution: host. */
    KNURL_DISCONNECT_HOST
} knurl_disconnect_side;
/** Borrowed owner-loop observation. Transport events have request NULL. Final
 * counters exist only with has_outcome; early rejection/takeover may leave
 * input_counts_known false.
 */
typedef struct knurl_observation {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    knurl_observation_kind kind;
    /** Copied accepted-connection metadata for this observation. */
    knurl_connection_info connection;
    /** Borrowed request handle; NULL for transport-only observations. */
    knurl_request *request;
    /** Resolved client address; does not replace the real TCP peer. */
    knurl_bytes effective_address;
    /** Host-resolved request identifier; empty when no resolver is configured.
     */
    knurl_bytes correlation_id;
    /** Name of this declaration, field or listed resource. */
    knurl_bytes name;
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
    knurl_status cause;
    /** Downstream/upstream/host attribution when known. */
    knurl_disconnect_side disconnect_side;
    /** Borrowed or copied terminal accounting according to the containing
     * callback/result.
     */
    knurl_request_outcome outcome;
} knurl_observation;
/** Borrowed snapshots, owner-loop only. Transport observations have
 * request=NULL and always identify the real TCP peer. Final counts/duration are
 * available only when has_outcome is true; input_counts_known distinguishes
 * admitted managed input from early rejection/takeover. Unknown status is zero.
 * Observers cannot perform request/response I/O or recursively emit events. */
typedef void (*knurl_observe_fn)(knurl_server *, const knurl_observation *,
                                 void *);
/** Owner-loop metadata callback after identity resolution. May associate
 * application userdata; cannot begin request/response I/O.
 */
typedef knurl_status (*knurl_request_start_fn)(knurl_server *, knurl_request *,
                                               void *, knurl_error *);
/** Once-per-initialized-managed-request callback after plan/response cleanup
 * and final observation, including failed admission/start. Metadata is valid
 * during the call; native source acknowledgements may finish later.
 */
typedef void (*knurl_request_end_fn)(knurl_server *, knurl_request *,
                                     const knurl_request_outcome *, void *);
/** Pre-body admission decision; ALLOW is the initialized default. Connection
 * rejection closes before TLS, request rejection uses the staged reply.
 */
typedef enum knurl_admission_action {
    /** Admission decision: allow. */
    KNURL_ADMISSION_ALLOW = 0,
    /** Admission decision: reject. */
    KNURL_ADMISSION_REJECT,
    /** Admission decision: close. */
    KNURL_ADMISSION_CLOSE
} knurl_admission_action;
/** Before-TLS admission using a borrowed real-TCP connection snapshot. Default
 * ALLOW; REJECT/CLOSE closes without HTTP output.
 */
typedef knurl_status (*knurl_connection_admit_fn)(knurl_server *,
                                                  const knurl_connection_info *,
                                                  knurl_admission_action *,
                                                  void *, knurl_error *);
/** Pre-body request admission. Default ALLOW; REJECT uses staged copied bytes
 * (default 403), CLOSE sends nothing. File/source payloads and bodies beyond
 * the output bound fail.
 */
typedef knurl_status (*knurl_request_admit_fn)(knurl_request *, knurl_reply *,
                                               knurl_admission_action *, void *,
                                               knurl_error *);
/** Configuration for admission. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_admission_config {
    /** Admission callback before TLS work; rejection closes the TCP connection.
     */
    knurl_connection_admit_fn on_connection;
    /** Admission callback after metadata resolution, before body/proxy
     * allocation.
     */
    knurl_request_admit_fn on_request;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_admission_config;
/** Host process/lifecycle and owner-loop request observers. Callback userdata
 * remains caller-owned through server destruction.
 */
typedef struct knurl_server_callbacks {
    /** Initial callback in each process; use it to invalidate inherited host
     * handles.
     */
    knurl_process_fn on_process_start;
    /** Parent preparation callback before child startup. */
    knurl_process_fn on_parent_prepare;
    /** Parent callback after required child/listener startup. The callback
     * enters with the server in READY_PAUSED. Open the gate with
     * knurl_server_set_serving before starting clients that must receive
     * application responses before this callback returns. On successful return,
     * an unselected gate follows start_paused.
     */
    knurl_process_fn on_parent_ready;
    /** Parent callback when shutdown begins. */
    knurl_process_fn on_parent_stopping;
    /** Parent callback after children and native resources stop. */
    knurl_process_fn on_parent_stopped;
    /** HTTP worker callback after its owner loop is available. */
    knurl_process_fn on_worker_start;
    /** HTTP worker callback when shutdown begins. */
    knurl_process_fn on_worker_stopping;
    /** Terminal callback in an exiting child after native teardown; may destroy
     * that fork's host/server state.
     */
    knurl_worker_exit_fn on_worker_exit;
    /** Parent observation of child start/exit/restart/escalation. */
    knurl_worker_event_fn on_worker_event;
    /** TLS identity availability/failure observer with a copied public event.
     */
    knurl_tls_identity_fn on_tls_identity;
    /** Metadata-only callback after identity resolution; may associate host
     * userdata, but cannot begin I/O.
     */
    knurl_request_start_fn on_request_start;
    /** Once-per-managed-request terminal callback after plan/response cleanup;
     * native acknowledgements may finish later.
     */
    knurl_request_end_fn on_request_close;
    /** Owner-loop observation callback; cannot perform request I/O or
     * recursively emit events.
     */
    knurl_observe_fn on_observe;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_server_callbacks;

/** Host control-FD read readiness bit. */
#define KNURL_CONTROL_READ 1U
/** Host control-FD write readiness bit. */
#define KNURL_CONTROL_WRITE 2U
/** Host control-FD end-of-input bit. */
#define KNURL_CONTROL_EOF 4U
/** Host control-FD error bit. */
#define KNURL_CONTROL_ERROR 8U

/** Owner-loop callback for the transferred host control FD; events uses
 * KNURL_CONTROL_* bits.
 */
typedef knurl_status (*knurl_control_fn)(knurl_server *server,
                                         unsigned int events, void *userdata,
                                         knurl_error *error);

/** Configuration for supervision. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_supervision_config {
    /** Nonzero enables this optional facility. */
    int enabled;
    /** Host control descriptor; successful supervision_take transfers cleanup
     * ownership.
     */
    int control_fd;
    /** Readiness event mask; zero suspends delivery where supported. */
    unsigned int interest;
    /** Owner-loop readiness/event callback. */
    knurl_control_fn on_event;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_supervision_config;

/** Maximum resolved correlation identifier length in bytes, excluding any
 * trailing NUL.
 */
#define KNURL_CORRELATION_ID_MAX 128U
/** Write into the supplied bounded buffer and return its length (no trailing
 * NUL required). Address resolvers return a numeric IPv4/IPv6 address, or zero
 * length to preserve the default identity and its trust validity;
 * correlation IDs may be empty but cannot contain NUL/control bytes. */
typedef knurl_status (*knurl_request_resolve_fn)(knurl_request *, char *,
                                                 size_t capacity,
                                                 size_t *length, void *,
                                                 knurl_error *);
/** Configuration for identifier. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_identifier_config {
    /** Host resolver callback; its bounded output is copied before return. */
    knurl_request_resolve_fn resolve;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_identifier_config;
/** Configuration for client ip. Use the owning operation's initializer before
 * applying overrides.
 */
typedef enum knurl_forwarded_format {
    KNURL_FORWARDED_COMMA = 0,
    KNURL_FORWARDED_RFC7239 = 1
} knurl_forwarded_format;
typedef struct knurl_client_ip_config {
    /** Trusted numeric proxy addresses/CIDRs; arbitrary forwarded addresses
     * remain untrusted.
     */
    const char *const *trusted_proxies;
    /** Number of entries in trusted_proxies. */
    size_t trusted_proxy_count;
    /** NULL selects X-Forwarded-For; empty disables this header. */
    const char *forwarded_header;
    /** NULL selects X-Real-IP; empty disables this fallback header. */
    const char *real_ip_header;
    /** Comma-separated numeric addresses or RFC 7239 for parameters. */
    knurl_forwarded_format forwarded_format;
    /** Zero selects 16 hops; maximum 64. Oversized chains use the peer. */
    size_t max_hops;
    /** Host resolver callback; its bounded output is copied before return. */
    knurl_request_resolve_fn resolve;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_client_ip_config;

/** Autoblock status rule. */
typedef struct knurl_autoblock_status_rule {
    /** HTTP status or operation status according to the containing type. */
    unsigned int status;
    /** Number of matching events within the configured window before blocking.
     */
    unsigned int threshold;
} knurl_autoblock_status_rule;

/** Autoblock event rule. */
typedef struct knurl_autoblock_event_rule {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Number of matching events within the configured window before blocking.
     */
    unsigned int threshold;
} knurl_autoblock_event_rule;

/** Configuration for autoblock. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_autoblock_config {
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
    const knurl_autoblock_status_rule *status_rules;
    /** Number of status_rules entries. */
    size_t status_rule_count;
    /** Copied named-application-event threshold rules. */
    const knurl_autoblock_event_rule *event_rules;
    /** Number of event_rules entries. */
    size_t event_rule_count;
} knurl_autoblock_config;

/** Maximum copied trusted-proxy hops in one autoblock notification. */
#define KNURL_AUTOBLOCK_MAX_VIA 16U
/** Maximum copied application-event name bytes in a notification. */
#define KNURL_AUTOBLOCK_NAME_MAX 256U
/** Address provenance used by the request identity policy. */
typedef enum knurl_client_address_source {
    KNURL_CLIENT_ADDRESS_PEER = 0,
    KNURL_CLIENT_ADDRESS_FORWARDED_FOR,
    KNURL_CLIENT_ADDRESS_REAL_IP,
    KNURL_CLIENT_ADDRESS_RESOLVER
} knurl_client_address_source;
/** The rule that produced an autoblock decision. */
typedef enum knurl_autoblock_reason {
    KNURL_AUTOBLOCK_HTTP_STATUS = 0,
    KNURL_AUTOBLOCK_TCP_STALL,
    KNURL_AUTOBLOCK_TLS_FAILURE,
    KNURL_AUTOBLOCK_APPLICATION
} knurl_autoblock_reason;
/** A newly applied block or a change to an existing block's deadline. */
typedef enum knurl_autoblock_change {
    KNURL_AUTOBLOCK_BLOCKED = 0,
    KNURL_AUTOBLOCK_EXTENDED
} knurl_autoblock_change;
/** Self-contained copied decision. Address is the blocked numeric client key;
 * peer is the actual TCP peer. Via lists known trusted proxies from client
 * towards server. Resolver overrides report the known peer only. Transport
 * events use peer identity. A truncated via list retains the nearest-server
 * suffix; a truncated name retains its prefix. Times are monotonic
 * milliseconds, suitable for duration arithmetic rather than wall-clock
 * display. Expiry is the engine's scheduled deadline.
 */
typedef struct knurl_autoblock_event {
    knurl_autoblock_change change;
    knurl_autoblock_reason reason;
    knurl_client_address_source address_source;
    char address[46];
    char peer_address[46];
    char local_address[46];
    unsigned short peer_port, local_port;
    /** Zero-based listener index, or (size_t)-1 when unavailable. */
    size_t listener_index;
    knurl_process_context process;
    int tls;
    char via[KNURL_AUTOBLOCK_MAX_VIA][46];
    size_t via_count;
    int via_truncated;
    char name[KNURL_AUTOBLOCK_NAME_MAX + 1];
    int name_truncated;
    /** Triggering HTTP status, or zero for another reason. */
    int status;
    /** Zero-based status/event rule index, or (size_t)-1 for transport rules.
     */
    size_t rule_index;
    unsigned int threshold, count, window_seconds;
    knurl_u64 occurred_at_ms, expires_at_ms, duration_ms;
} knurl_autoblock_event;
/** Native notification in the process that changed the block, after releasing
 * the shared lock. Event is borrowed during the call. Keep work bounded; do not
 * enter a Lua VM or recursively emit events. Querying notifications or
 * requesting server stop is allowed. Callback failure cannot undo the block.
 */
typedef void (*knurl_autoblock_fn)(knurl_server *,
                                   const knurl_autoblock_event *, void *);
/** Inert notification declaration. Zero queue_capacity disables polling;
 * callback and queue may be selected independently. The queue overwrites its
 * oldest event when full and reports that loss on the next poll. The
 * declaration is copied; callback userdata is borrowed until replaced or server
 * destruction.
 */
typedef struct knurl_autoblock_notifications_config {
    size_t queue_capacity;
    knurl_autoblock_fn callback;
    void *userdata;
} knurl_autoblock_notifications_config;
/** Initialize notifications with queue and callback disabled. */
KNURL_API void knurl_autoblock_notifications_config_init(
    knurl_autoblock_notifications_config *);

/** Configuration for proxy resource. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_proxy_resource_config {
    /** Zero inherits 16 active exchanges and four idle connections per pool. */
    size_t active_exchanges;
    /** Per-upstream idle connection capacity; zero selects four. */
    size_t idle_connections;
} knurl_proxy_resource_config;

/** Configuration for server. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_server_config {
    /** Copied instance name; independent of http.server_header. */
    const char *name;
    /** Listener declarations copied during inert server construction. */
    const knurl_listener_config *listeners;
    /** Number of listeners. */
    size_t listener_count;
    /** Worker count, limits and restart/shutdown policy. */
    knurl_worker_config workers;
    /** Connection, header and request-body admission limits. */
    knurl_limit_config limits;
    /** Server transport/lifecycle deadline declarations. */
    knurl_timeout_config timeouts;
    /** HTTP/backend behavior settings. */
    knurl_http_config http;
    /** Bounded transport buffer settings. */
    knurl_io_config io;
    /** Shared reverse-proxy active/idle resource settings. */
    knurl_proxy_resource_config proxy;
    /** Copied downstream TLS/identity/ACME declaration; material opener
     * userdata remains borrowed.
     */
    knurl_tls_config tls;
    /** Host-owned or library-owned signal handling mode. */
    knurl_signal_config signals;
    /** Host lifecycle/request observation callbacks; userdata remains borrowed.
     */
    knurl_server_callbacks callbacks;
    /** Router or raw headers-callback dispatch selection. */
    knurl_dispatch_config dispatch;
    /** Host control-FD supervision settings. */
    knurl_supervision_config supervision;
    /** Trusted-proxy and optional address resolution policy. */
    knurl_client_ip_config client_ip;
    /** Host-resolved request identifier; empty when no resolver is configured.
     */
    knurl_identifier_config correlation_id;
    /** Connection/request admission callbacks, before body/proxy allocation. */
    knurl_admission_config admission;
    /** Shared event-driven abuse blocking policy. */
    knurl_autoblock_config autoblock;
    /** Default 0 opens serving after on_parent_ready returns successfully;
     * nonzero keeps it paused. An explicit gate selection in that callback
     * takes precedence.
     */
    int start_paused;
} knurl_server_config;

/** Normalized configuration view with resolved/rounded deadlines, borrowing
 * server-owned strings/arrays. requested_timeouts preserves inheritance/disable
 * declarations.
 */
typedef struct knurl_server_effective {
    /** Resolved configuration borrowing the owning handle's storage. */
    knurl_server_config config;
    /** Original zero-inheritance/disabled deadline choices before
     * normalization.
     */
    knurl_timeout_config requested_timeouts;
    /** Original worker count before automatic resolution. */
    size_t requested_worker_count;
    /** Resolved maximum parsed request-header field count. */
    size_t max_header_fields;
    /** Borrowed name of the server's selected backend. */
    const char *backend_name;
} knurl_server_effective;

/** Initialize deadline with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_deadline_init(knurl_deadline *deadline);
/** Initialize listener config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_listener_config_init(knurl_listener_config *config);
/** Initialize server config with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_server_config_init(knurl_server_config *config);
/** Apply the production HTTP server profile; overrides should be applied after
 * this initializer.
 */
KNURL_API void
knurl_server_config_init_production_webserver(knurl_server_config *config);
/** Inert declaration copying; no callbacks, files, sockets, signals or forks.
 */
KNURL_API knurl_status knurl_server_new(const knurl_server_config *config,
                                        knurl_server **out, knurl_error *error);
/** Copy normalized settings into a view borrowing server-owned storage;
 * includes requested timeout choices.
 */
KNURL_API knurl_status
knurl_server_effective_config(const knurl_server *server,
                              knurl_server_effective *out, knurl_error *error);
/** Inspect the declared/run/stop lifecycle state without changing it. */
KNURL_API knurl_server_state knurl_server_get_state(const knurl_server *server);
/** Borrow the server-owned route registry until server destruction. */
KNURL_API knurl_router *knurl_server_router(knurl_server *server);
/** Replace the server's inert router; transfer ownership only on success. */
KNURL_API knurl_status knurl_server_router_take(knurl_server *server,
                                                knurl_router *router,
                                                knurl_error *error);
/** Attach host control-FD supervision; successful transfer owns FD cleanup,
 * failure retains caller ownership.
 */
KNURL_API knurl_status knurl_server_supervision_take(
    knurl_server *server, const knurl_supervision_config *config,
    knurl_error *error);
/** Change control-descriptor interest on its owner loop; zero suspends
 * notifications.
 */
KNURL_API knurl_status knurl_server_control_interest(knurl_server *server,
                                                     unsigned int interest,
                                                     knurl_error *error);
/** Destroy an inactive server and owned declarations; rejects active or
 * reentrant destruction.
 */
KNURL_API knurl_status knurl_server_destroy(knurl_server *server,
                                            knurl_error *error);
/** Validate declarations and TLS material without binding listeners or forking;
 * material callbacks may run.
 */
KNURL_API knurl_status knurl_server_validate(knurl_server *server,
                                             knurl_error *error);
/** Validate an inactive server and probe its startup resources in the caller.
 * Opens all configured listeners together, then closes them. Probes an explicit
 * body spool directory when a declared ordinary handler can spill and opens the
 * configured access log with the runtime's file rules. The log may be created.
 * TLS material callbacks may run. Request-time header callbacks choose their
 * body policy at dispatch. The call leaves the server inactive and releases
 * probe resources. Resource availability may change before the subsequent run.
 */
KNURL_API knurl_status knurl_server_preflight(knurl_server *server,
                                              knurl_error *error);
/** Run the server lifecycle until stop/failure; return the run outcome after
 * worker/native cleanup.
 */
KNURL_API knurl_status knurl_server_run(knurl_server *server,
                                        knurl_run_result *out,
                                        knurl_error *error);
/** Request graceful stop of an active run; callable from another thread while
 * the server remains alive.
 */
KNURL_API knurl_status knurl_server_request_stop(knurl_server *server,
                                                 knurl_error *error);
/** Async-signal-safe during a run; the caller keeps the server alive. */
KNURL_API void knurl_server_notify_signal(knurl_server *server,
                                          int signal_number);
/** Open or pause serving on the ready parent owner loop, including from
 * on_parent_ready. Readiness remains separate. Nonzero opens the gate; a stop
 * already requested prevents opening it. Other processes/threads or a server
 * outside READY_PAUSED/SERVING return STATE.
 */
KNURL_API knurl_status knurl_server_set_serving(knurl_server *server,
                                                int serving,
                                                knurl_error *error);
/** Borrows addresses acquired by the active run, including ephemeral ports. */
KNURL_API knurl_status knurl_server_listener_at(const knurl_server *server,
                                                size_t index,
                                                knurl_bound_listener *out,
                                                knurl_error *error);
/** Copy an inactive server's notification declaration. NULL disables delivery.
 * Registration clears retained history. A run starts a fresh queue, shared by
 * all workers. Polling history remains available after run returns. */
KNURL_API knurl_status knurl_server_set_autoblock_notifications(
    knurl_server *, const knurl_autoblock_notifications_config *,
    knurl_error *);
/** Consume one copied notification from the shared FIFO, or return EOF.
 * Coordinate one logical polling consumer across processes. dropped reports
 * overwrite/recovery losses since the last poll requesting that count,
 * including on EOF. Active polling requires the current process owner loop.
 * Inactive polling is serialized with lifecycle operations. No callback is
 * invoked by polling. Queue contents last until the next run, notification
 * registration or server destruction. */
KNURL_API knurl_status knurl_server_autoblock_next(knurl_server *,
                                                   knurl_autoblock_event *,
                                                   knurl_u64 *dropped,
                                                   knurl_error *);

struct knurl_access_log_config;
struct knurl_bound_listener;
struct knurl_error;
struct knurl_loop;
struct knurl_router;
struct knurl_run_result;
struct knurl_server_effective;
struct knurl_storage_filesystem_config;
struct knurl_storage_provider;
struct knurl_supervision_config;
struct pslog_logger;
/** Library-owned server receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_server {
    /** Return the packaged Kore commit string in static storage. */
    const char *(*kore_revision)(const struct knurl_server *server);
    /** Active-run view; outside a run returns STATE. An identity with no HTTPS
     * listener remains pending. The output borrows no backend structures. */
    knurl_status (*tls_identity_state)(const struct knurl_server *server,
                                       size_t index,
                                       knurl_tls_identity_state *out,
                                       struct knurl_error *error);
    /** Inert outside-run registration. Both logger pointers are borrowed for
     * every run; their sinks must support native process callbacks. Rules are
     * copied. NULL configuration uses trace/warn/error; NULL root uses a
     * library-owned default logger during run. Without an override, access
     * derives sub=http. logger_disabled affects structured access only,
     * independently of file logs.
     */
    knurl_status (*set_pslog)(struct knurl_server *, struct pslog_logger *,
                              const struct knurl_access_log_config *,
                              struct knurl_error *);
    /** Borrow the current process's loop during an active server run. Captured
     * handles cannot cross a fork or run boundary. All operations require the
     * owner thread except waker_signal and server_request_stop.
     */
    struct knurl_loop *(*loop)(struct knurl_server *server);
    /** Infer a filename's Content-Type using this instance's overrides, then
     * built-ins, then application/octet-stream. NULL server uses only
     * built-ins. No filesystem access. Result is borrowed until server
     * destruction (or has static lifetime for built-ins); path is a
     * filename/path, not a URL.
     */
    const char *(*content_type)(const struct knurl_server *, const char *path);
    /** Copy normalized settings into a view borrowing server-owned storage;
     * includes requested timeout choices.
     */
    knurl_status (*effective_config)(const struct knurl_server *server,
                                     struct knurl_server_effective *out,
                                     struct knurl_error *error);
    /** Inspect the declared/run/stop lifecycle state without changing it. */
    knurl_server_state (*get_state)(const struct knurl_server *server);
    /** Borrow the server-owned route registry until server destruction. */
    struct knurl_router *(*router)(struct knurl_server *server);
    /** Replace the server's inert router; transfer ownership only on success.
     */
    knurl_status (*router_take)(struct knurl_server *server,
                                struct knurl_router *router,
                                struct knurl_error *error);
    /** Attach host control-FD supervision; successful transfer owns FD cleanup,
     * failure retains caller ownership.
     */
    knurl_status (*supervision_take)(
        struct knurl_server *server,
        const struct knurl_supervision_config *config,
        struct knurl_error *error);
    /** Change control-descriptor interest on its owner loop; zero suspends
     * notifications.
     */
    knurl_status (*control_interest)(struct knurl_server *server,
                                     unsigned int interest,
                                     struct knurl_error *error);
    /** Destroy an inactive server and owned declarations; rejects active or
     * reentrant destruction.
     */
    knurl_status (*destroy)(struct knurl_server *server,
                            struct knurl_error *error);
    /** Validate declarations and TLS material without binding listeners or
     * forking; material callbacks may run.
     */
    knurl_status (*validate)(struct knurl_server *server,
                             struct knurl_error *error);
    /** Validate an inactive server and probe its startup resources in the
     * caller. Opens all configured listeners together, then closes them. Probes
     * an explicit body spool directory when a declared ordinary handler can
     * spill and opens the configured access log with the runtime's file rules.
     * The log may be created. TLS material callbacks may run. Request-time
     * header callbacks choose their body policy at dispatch. The call leaves
     * the server inactive and releases probe resources. Resource availability
     * may change before the subsequent run.
     */
    knurl_status (*preflight)(struct knurl_server *server,
                              struct knurl_error *error);
    /** Run the server lifecycle until stop/failure; return the run outcome
     * after worker/native cleanup.
     */
    knurl_status (*run)(struct knurl_server *server,
                        struct knurl_run_result *out,
                        struct knurl_error *error);
    /** Request graceful stop of an active run; callable from another thread
     * while the server remains alive.
     */
    knurl_status (*request_stop)(struct knurl_server *server,
                                 struct knurl_error *error);
    /** Async-signal-safe during a run; the caller keeps the server alive. */
    void (*notify_signal)(struct knurl_server *server, int signal_number);
    /** Open or pause serving on the ready parent owner loop, including from
     * on_parent_ready. Readiness remains separate. Nonzero opens the gate; a
     * stop already requested prevents opening it. Other processes/threads or a
     * server outside READY_PAUSED/SERVING return STATE.
     */
    knurl_status (*set_serving)(struct knurl_server *server, int serving,
                                struct knurl_error *error);
    /** Borrows addresses acquired by the active run, including ephemeral ports.
     */
    knurl_status (*listener_at)(const struct knurl_server *server, size_t index,
                                struct knurl_bound_listener *out,
                                struct knurl_error *error);
    /** Inert declaration over a borrowed server. Filesystem providers share one
     * bounded executor in that HTTP worker, created on its first operation.
     * Absolute cache/root paths; NULL root
     * selects cache_dir/webdav/site_id/content and the existing tombstone tree.
     * Zero quotas select 16 MiB/file, 128 MiB total and 4096 resources.
     * PUT transactions are capped by max_file_bytes and admitted by server
     * limits; native execution uses the worker's bounded queue with
     * backpressure. Conditions are rechecked with quota accounting under the
     * mutation lock at finish. No filesystem I/O or threads are started by
     * construction. Server outlives the provider; bound providers are not
     * reusable across runs. */
    knurl_status (*storage_filesystem)(
        struct knurl_server *, const struct knurl_storage_filesystem_config *,
        struct knurl_storage_provider **, struct knurl_error *);
    /** Configure native callbacks and the shared polling queue while inactive.
     */
    knurl_status (*set_autoblock_notifications)(
        knurl_server *, const knurl_autoblock_notifications_config *,
        knurl_error *);
    /** Consume one copied event; EOF still reports any queue losses. */
    knurl_status (*autoblock_next)(knurl_server *, knurl_autoblock_event *,
                                   knurl_u64 *, knurl_error *);
    /** Private library state; applications must not access it. */
    struct knurl_server_impl *impl;
};

#ifdef __cplusplus
}
#endif
#endif
