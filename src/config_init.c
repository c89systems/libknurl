#include "server_internal.h"

void knurl_deadline_init(knurl_deadline *deadline)
{
    if (deadline != NULL) {
        deadline->milliseconds = 0;
        deadline->disabled = 0;
    }
}

void knurl_listener_config_init(knurl_listener_config *config)
{
    if (config == NULL)
        return;
    config->name = NULL;
    config->bind = "0.0.0.0";
    config->port = 0;
    config->kind = KNURL_LISTENER_HTTP;
    config->identity_index = 0;
    config->presented_port = 0;
    config->redirect_https_port = 0;
}

void knurl_tls_material_init(knurl_tls_material *material)
{
    if (material == NULL)
        return;
    material->kind = KNURL_MATERIAL_NONE;
    material->path = NULL;
    material->bytes.data = NULL;
    material->bytes.length = 0;
    material->open = NULL;
    material->userdata = NULL;
}

void knurl_tls_identity_init(knurl_tls_identity *identity)
{
    if (identity == NULL)
        return;
    identity->mode = KNURL_TLS_DISABLED;
    identity->domains = NULL;
    identity->domain_count = 0;
    knurl_tls_material_init(&identity->bundle);
    knurl_tls_material_init(&identity->certificate);
    knurl_tls_material_init(&identity->private_key);
    knurl_tls_material_init(&identity->extra_chain);
    knurl_tls_material_init(&identity->client_ca);
    identity->require_client_certificate = 0;
}

void knurl_tls_config_init(knurl_tls_config *config)
{
    if (config == NULL)
        return;
    config->version = KNURL_TLS_VERSION_DEFAULT;
    config->cipher_list = NULL;
    config->max_material_bytes = 1024UL * 1024UL;
    config->identities = NULL;
    config->identity_count = 0;
    config->acme.email = NULL;
    config->acme.directory_url = NULL;
    config->acme.runtime_directory = NULL;
    config->acme.ca_file = NULL;
    config->acme.ca_directory = NULL;
    config->acme.persist_timeout_ms = 30000;
    config->acme.state = NULL;
    config->acme.userdata = NULL;
}

void knurl_server_config_init(knurl_server_config *config)
{
    if (config == NULL)
        return;
    config->name = "knurl";
    config->listeners = NULL;
    config->listener_count = 0;
    config->workers.count = 0;
    config->workers.accept_threshold = 16;
    config->workers.rlimit_nofiles = 768;
    config->workers.affinity_disabled = 0;
    config->workers.shutdown_timeout_ms = 5000;
    config->workers.death_policy = KNURL_WORKER_RESTART;
    config->limits.max_connections = 1024;
    config->limits.request_limit = 0;
    config->limits.max_request_header_bytes = 65536UL;
    config->limits.max_response_header_bytes = 0;
    config->limits.default_body_bytes.high = 0;
    config->limits.default_body_bytes.low = 10UL * 1024UL * 1024UL;
    config->limits.hard_body_bytes.high = config->limits.hard_body_bytes.low =
        0;
    config->limits.body_spool_directory = NULL;
    config->limits.body_min_rate_bytes_per_second.high = 0;
    config->limits.body_min_rate_bytes_per_second.low = 1024;
    config->limits.body_min_rate_disabled = 0;
    config->limits.websocket_max_frame_bytes = 16384;
    knurl_deadline_init(&config->timeouts.startup);
    knurl_deadline_init(&config->timeouts.request_header);
    knurl_deadline_init(&config->timeouts.request_body_idle);
    knurl_deadline_init(&config->timeouts.response_write_idle);
    knurl_deadline_init(&config->timeouts.body_min_rate_grace);
    knurl_deadline_init(&config->timeouts.connection_idle);
    knurl_deadline_init(&config->timeouts.keepalive);
    knurl_deadline_init(&config->timeouts.websocket_idle);
    knurl_deadline_init(&config->timeouts.deferred_response);
    config->http.mime_types = NULL;
    config->http.mime_type_count = 0;
    config->http.server_header = "knurl";
    config->http.access_log_path = NULL;
    config->http.access_log_max_bytes.high = 0;
    config->http.access_log_max_bytes.low = 10UL * 1024UL * 1024UL;
    config->http.access_log_file_count = 3;
    config->http.access_log_rotation_disabled = 0;
    config->http.keepalive_disabled = 0;
    config->http.keepalive_max_requests = 100;
    config->http.socket_backlog = 5000;
    config->http.request_process_budget_ms = 100;
    config->http.hsts_max_age_seconds = 0;
    config->http.backend_curl_timeout_seconds = 0;
    config->http.backend_curl_recv_max_bytes.high = 0;
    config->http.backend_curl_recv_max_bytes.low = 0;
    config->http.backend_quiet = config->http.pretty_error_pages = 0;
    config->io.chunk_bytes = 8192;
    config->proxy.active_exchanges = 0;
    config->proxy.idle_connections = 0;
    config->io.input_queue_bytes = config->io.output_queue_bytes = 262144UL;
    knurl_tls_config_init(&config->tls);
    config->signals.mode = KNURL_SIGNALS_HOST;
    knurl_dispatch_config_init(&config->dispatch);
    config->callbacks.on_process_start = NULL;
    config->callbacks.on_parent_prepare = NULL;
    config->callbacks.on_parent_ready = NULL;
    config->callbacks.on_parent_stopping = NULL;
    config->callbacks.on_parent_stopped = NULL;
    config->callbacks.on_worker_start = NULL;
    config->callbacks.on_worker_stopping = NULL;
    config->callbacks.on_worker_exit = NULL;
    config->callbacks.on_worker_event = NULL;
    config->callbacks.on_tls_identity = NULL;
    config->callbacks.on_request_start = NULL;
    config->callbacks.on_request_close = NULL;
    config->callbacks.on_observe = NULL;
    config->callbacks.userdata = NULL;
    config->supervision.enabled = 0;
    config->supervision.control_fd = -1;
    config->supervision.interest = KNURL_CONTROL_READ;
    config->supervision.on_event = NULL;
    config->supervision.userdata = NULL;
    config->client_ip.trusted_proxies = NULL;
    config->client_ip.trusted_proxy_count = 0;
    config->client_ip.forwarded_header = NULL;
    config->client_ip.real_ip_header = NULL;
    config->client_ip.forwarded_format = KNURL_FORWARDED_COMMA;
    config->client_ip.max_hops = 0;
    config->client_ip.resolve = NULL;
    config->client_ip.userdata = NULL;
    config->correlation_id.resolve = NULL;
    config->correlation_id.userdata = NULL;
    config->admission.on_connection = NULL;
    config->admission.on_request = NULL;
    config->admission.userdata = NULL;
    config->autoblock.enabled = 0;
    config->autoblock.window_seconds = 1800;
    config->autoblock.block_seconds = 3600;
    config->autoblock.max_entries = 8192;
    config->autoblock.tcp_stall_threshold = 0;
    config->autoblock.tls_failure_threshold = 0;
    config->autoblock.status_rules = NULL;
    config->autoblock.status_rule_count = 0;
    config->autoblock.event_rules = NULL;
    config->autoblock.event_rule_count = 0;
    config->start_paused = 0;
}

void knurl_server_config_init_production_webserver(knurl_server_config *config)
{
    static const knurl_autoblock_status_rule rules[] = {
        {401, 10}, {403, 10}, {404, 50}, {429, 5}};
    if (config == NULL)
        return;
    knurl_server_config_init(config);
    config->limits.hard_body_bytes = config->limits.default_body_bytes;
    config->autoblock.enabled = 1;
    config->autoblock.tcp_stall_threshold = 20;
    config->autoblock.tls_failure_threshold = 10;
    config->autoblock.status_rules = rules;
    config->autoblock.status_rule_count = sizeof(rules) / sizeof(rules[0]);
}
