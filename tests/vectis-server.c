#include "vectis-server.h"
#include <string.h>
/* Matches vectis_server_config_init, not an alternative library default set. */
void reference_server_config_init(struct reference_server_config *config)
{
    memset(config, 0, sizeof(*config));
    config->max_connections = 1024;
    config->worker_accept_threshold = 16;
    config->worker_rlimit_nofiles = 768;
    config->worker_shutdown_timeout_ms = 5000;
    config->max_request_header_bytes = 65536UL;
    config->request_header_timeout_ms = 5000;
    config->request_body_idle_timeout_ms = 15000;
    config->response_write_idle_timeout_ms = 15000;
    config->request_body_min_rate_bytes_per_sec = 1024;
    config->request_body_min_rate_grace_ms = 10000;
    config->idle_timeout_ms = 30000;
    config->keepalive_timeout_ms = 5000;
    config->keepalive_max_requests = 100;
    config->socket_backlog = 5000;
    config->request_process_budget_ms = 100;
    config->websocket_max_frame_bytes = 16384;
    config->websocket_timeout_ms = 120000;
    config->server_header = "vectis";
    config->access_log.success_level = PSLOG_LEVEL_TRACE;
    config->access_log.client_error_level = PSLOG_LEVEL_WARN;
    config->access_log.server_error_level = PSLOG_LEVEL_ERROR;
    config->autoblock.window_seconds = 1800;
    config->autoblock.block_seconds = 3600;
    config->autoblock.max_entries = 8192;
}
static rotmos_status invalid(rotmos_error *error)
{
    rotmos_error_clear(error);
    if (error != NULL) {
        error->status = ROTMOS_INVALID;
        strcpy(error->operation, "reference_server");
        strcpy(error->message, "provide output and bounded rule arrays");
    }
    return ROTMOS_INVALID;
}
rotmos_status
reference_server_translate(const struct reference_server_config *in,
                           rotmos_server_config *out, rotmos_error *error)
{
    struct reference_server_config defaults;
    rotmos_status status;
    if (in == NULL) {
        reference_server_config_init(&defaults);
        in = &defaults;
    }
    if (out == NULL || in->autoblock.status_rule_count > 16 ||
        in->autoblock.event_rule_count > 16 ||
        in->access_log.status_level_rule_count > 32)
        return invalid(error);
    rotmos_server_config_init(out);
    out->name = "vectis";
    out->limits.max_connections = in->max_connections;
    out->limits.request_limit = in->request_limit;
    out->workers.count = in->worker_count;
    out->workers.accept_threshold = in->worker_accept_threshold;
    out->workers.rlimit_nofiles = in->worker_rlimit_nofiles;
    out->workers.affinity_disabled = in->worker_set_affinity_disabled;
    out->workers.shutdown_timeout_ms = in->worker_shutdown_timeout_ms;
    out->workers.death_policy = in->worker_death_policy;
    out->limits.max_request_header_bytes = in->max_request_header_bytes;
    out->limits.body_spool_directory = in->request_body_spool_dir;
    status = rotmos_u64_from_size(in->max_request_body_bytes,
                                  &out->limits.hard_body_bytes, error);
    if (status != ROTMOS_OK)
        return status;
    if (in->max_request_body_bytes != 0)
        out->limits.default_body_bytes = out->limits.hard_body_bytes;
    status = rotmos_u64_from_size(in->request_body_min_rate_bytes_per_sec,
                                  &out->limits.body_min_rate_bytes_per_second,
                                  error);
    if (status != ROTMOS_OK)
        return status;
    out->limits.websocket_max_frame_bytes = in->websocket_max_frame_bytes;
    out->timeouts.request_header.milliseconds = in->request_header_timeout_ms;
    out->timeouts.request_body_idle.milliseconds =
        in->request_body_idle_timeout_ms;
    out->timeouts.response_write_idle.milliseconds =
        in->response_write_idle_timeout_ms;
    out->timeouts.body_min_rate_grace.milliseconds =
        in->request_body_min_rate_grace_ms;
    out->timeouts.connection_idle.milliseconds = in->idle_timeout_ms;
    /* Kore does not inspect this deadline when keepalive is disabled. */
    out->timeouts.keepalive.milliseconds =
        in->keepalive_disabled ? 0 : in->keepalive_timeout_ms;
    out->timeouts.websocket_idle.milliseconds = in->websocket_timeout_ms;
    out->http.keepalive_disabled = in->keepalive_disabled;
    out->http.keepalive_max_requests = in->keepalive_max_requests;
    out->http.backend_curl_timeout_seconds = in->kore_curl_timeout_seconds;
    status =
        rotmos_u64_from_size(in->kore_curl_recv_max_bytes,
                             &out->http.backend_curl_recv_max_bytes, error);
    if (status != ROTMOS_OK)
        return status;
    out->http.backend_quiet = in->kore_quiet;
    out->http.socket_backlog = in->socket_backlog;
    out->http.request_process_budget_ms = in->request_process_budget_ms;
    out->http.hsts_max_age_seconds = in->hsts_max_age_seconds;
    out->http.server_header =
        in->server_header != NULL ? in->server_header : "vectis";
    out->http.access_log_path = in->access_log_path;
    out->http.pretty_error_pages = in->pretty_error_pages;
    out->client_ip.trusted_proxies = in->client_ip.trusted_proxies;
    out->client_ip.trusted_proxy_count = in->client_ip.trusted_proxy_count;
    out->autoblock.enabled = in->autoblock.enabled;
    out->autoblock.window_seconds = in->autoblock.window_seconds;
    out->autoblock.block_seconds = in->autoblock.block_seconds;
    out->autoblock.max_entries = in->autoblock.max_entries;
    out->autoblock.tcp_stall_threshold = in->autoblock.tcp_stall_threshold;
    out->autoblock.tls_failure_threshold = in->autoblock.tls_failure_threshold;
    out->autoblock.status_rules =
        in->autoblock.status_rule_count ? in->autoblock.status_rules : NULL;
    out->autoblock.status_rule_count = in->autoblock.status_rule_count;
    out->autoblock.event_rules =
        in->autoblock.event_rule_count ? in->autoblock.event_rules : NULL;
    out->autoblock.event_rule_count = in->autoblock.event_rule_count;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
rotmos_status reference_server_new(const struct reference_server_config *input,
                                   const rotmos_listener_config *listeners,
                                   size_t count,
                                   const rotmos_server_callbacks *callbacks,
                                   pslog_logger *logger, rotmos_server **out,
                                   rotmos_error *error)
{
    struct reference_server_config defaults;
    rotmos_server_config config;
    rotmos_access_log_config access;
    rotmos_status status;
    if (out == NULL)
        return invalid(error);
    *out = NULL;
    if (input == NULL) {
        reference_server_config_init(&defaults);
        input = &defaults;
    }
    status = reference_server_translate(input, &config, error);
    if (status != ROTMOS_OK)
        return status;
    config.listeners = listeners;
    config.listener_count = count;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    if (callbacks != NULL)
        config.callbacks = *callbacks;
    status = rotmos_server_new(&config, out, error);
    if (status != ROTMOS_OK)
        return status;
    rotmos_access_log_config_init(&access);
    access.logger = input->access_log.logger;
    access.logger_disabled = input->access_log.logger_disabled;
    access.success_level = input->access_log.success_level;
    access.client_error_level = input->access_log.client_error_level;
    access.server_error_level = input->access_log.server_error_level;
    access.status_level_rules = input->access_log.status_level_rules;
    access.status_level_rule_count = input->access_log.status_level_rule_count;
    status = rotmos_server_set_pslog(*out, logger, &access, error);
    if (status != ROTMOS_OK) {
        rotmos_server_destroy(*out, NULL);
        *out = NULL;
    }
    return status;
}
