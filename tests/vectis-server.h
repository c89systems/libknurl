#ifndef KNURL_REFERENCE_VECTIS_SERVER_H
#define KNURL_REFERENCE_VECTIS_SERVER_H
#include <knurl/server.h>
#include <knurl/pslog.h>
/* Test-only transcription of the 34 baseline server fields. Aggregate rule
 * values use public, layout-compatible knurl value types; arrays retain the
 * baseline inline ownership. No Vectis checkout or application types are build
 * inputs. Source: vectis.h at 6b1e263e0436687f68a7768ca55f4361a6e59e19. */
struct reference_access_log {
    pslog_logger *logger;
    int logger_disabled;
    pslog_level success_level, client_error_level, server_error_level;
    knurl_access_log_status_level_rule status_level_rules[32];
    size_t status_level_rule_count;
};
struct reference_client_ip {
    const char *const *trusted_proxies;
    size_t trusted_proxy_count;
};
struct reference_autoblock {
    int enabled;
    unsigned int window_seconds, block_seconds, max_entries;
    unsigned int tcp_stall_threshold, tls_failure_threshold;
    knurl_autoblock_status_rule status_rules[16];
    size_t status_rule_count;
    knurl_autoblock_event_rule event_rules[16];
    size_t event_rule_count;
};
struct reference_server_config {
    size_t max_connections, request_limit, worker_count;
    size_t worker_accept_threshold, worker_rlimit_nofiles;
    int worker_set_affinity_disabled;
    long worker_shutdown_timeout_ms;
    size_t max_request_header_bytes, max_request_body_bytes;
    const char *request_body_spool_dir;
    long request_header_timeout_ms, request_body_idle_timeout_ms;
    long response_write_idle_timeout_ms;
    size_t request_body_min_rate_bytes_per_sec;
    long request_body_min_rate_grace_ms, idle_timeout_ms;
    int keepalive_disabled;
    long keepalive_timeout_ms;
    unsigned int keepalive_max_requests, kore_curl_timeout_seconds;
    size_t kore_curl_recv_max_bytes;
    int kore_quiet;
    unsigned int socket_backlog, request_process_budget_ms,
        hsts_max_age_seconds;
    size_t websocket_max_frame_bytes;
    long websocket_timeout_ms;
    const char *server_header, *access_log_path;
    struct reference_access_log access_log;
    int pretty_error_pages;
    knurl_worker_death_policy worker_death_policy;
    struct reference_client_ip client_ip;
    struct reference_autoblock autoblock;
};
void reference_server_config_init(struct reference_server_config *);
knurl_status reference_server_translate(const struct reference_server_config *,
                                        knurl_server_config *, knurl_error *);
/* Foreground factory. A supervised host uses translate and supplies its own
 * control-descriptor/signal policy through public server configuration. */
knurl_status reference_server_new(const struct reference_server_config *,
                                  const knurl_listener_config *, size_t,
                                  const knurl_server_callbacks *,
                                  pslog_logger *, knurl_server **,
                                  knurl_error *);
#endif
