#include "server_internal.h"

void rotmos_deadline_init(rotmos_deadline *deadline)
{
    if (deadline != NULL) {
        deadline->milliseconds = 0;
        deadline->disabled = 0;
    }
}

void rotmos_listener_config_init(rotmos_listener_config *config)
{
    if (config == NULL)
        return;
    config->name = NULL;
    config->bind = "0.0.0.0";
    config->port = 0;
    config->kind = ROTMOS_LISTENER_HTTP;
    config->identity_index = 0;
    config->presented_port = 0;
    config->redirect_https_port = 0;
}

void rotmos_tls_material_init(rotmos_tls_material *material)
{
    if (material == NULL)
        return;
    material->kind = ROTMOS_MATERIAL_NONE;
    material->path = NULL;
    material->bytes.data = NULL;
    material->bytes.length = 0;
    material->open = NULL;
    material->userdata = NULL;
}

void rotmos_tls_identity_init(rotmos_tls_identity *identity)
{
    if (identity == NULL)
        return;
    identity->mode = ROTMOS_TLS_DISABLED;
    identity->domains = NULL;
    identity->domain_count = 0;
    rotmos_tls_material_init(&identity->bundle);
    rotmos_tls_material_init(&identity->certificate);
    rotmos_tls_material_init(&identity->private_key);
    rotmos_tls_material_init(&identity->extra_chain);
    rotmos_tls_material_init(&identity->client_ca);
    identity->require_client_certificate = 0;
}

void rotmos_tls_config_init(rotmos_tls_config *config)
{
    if (config == NULL)
        return;
    config->version = ROTMOS_TLS_VERSION_DEFAULT;
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

void rotmos_server_config_init(rotmos_server_config *config)
{
    if (config == NULL)
        return;
    config->name = "rotmos";
    config->listeners = NULL;
    config->listener_count = 0;
    config->workers.count = 0;
    config->workers.accept_threshold = 16;
    config->workers.rlimit_nofiles = 768;
    config->workers.affinity_disabled = 0;
    config->workers.shutdown_timeout_ms = 5000;
    config->workers.death_policy = ROTMOS_WORKER_RESTART;
    config->limits.max_connections = 1024;
    config->limits.request_limit = 0;
    config->limits.max_request_header_bytes = 65536UL;
    config->limits.max_response_header_bytes = 65536UL;
    config->limits.default_body_bytes.high = 0;
    config->limits.default_body_bytes.low = 10UL * 1024UL * 1024UL;
    config->limits.hard_body_bytes.high = config->limits.hard_body_bytes.low =
        0;
    config->limits.body_spool_directory = NULL;
    config->limits.body_min_rate_bytes_per_second.high = 0;
    config->limits.body_min_rate_bytes_per_second.low = 1024;
    config->limits.body_min_rate_disabled = 0;
    config->limits.websocket_max_frame_bytes = 16384;
    rotmos_deadline_init(&config->timeouts.startup);
    rotmos_deadline_init(&config->timeouts.request_header);
    rotmos_deadline_init(&config->timeouts.request_body_idle);
    rotmos_deadline_init(&config->timeouts.response_write_idle);
    rotmos_deadline_init(&config->timeouts.body_min_rate_grace);
    rotmos_deadline_init(&config->timeouts.connection_idle);
    rotmos_deadline_init(&config->timeouts.keepalive);
    rotmos_deadline_init(&config->timeouts.websocket_idle);
    rotmos_deadline_init(&config->timeouts.deferred_response);
    config->http.server_header = "rotmos";
    config->http.access_log_path = NULL;
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
    config->io.input_queue_bytes = config->io.output_queue_bytes = 262144UL;
    rotmos_tls_config_init(&config->tls);
    config->signals.mode = ROTMOS_SIGNALS_HOST;
    rotmos_dispatch_config_init(&config->dispatch);
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
    config->callbacks.userdata = NULL;
    config->supervision.enabled = 0;
    config->supervision.control_fd = -1;
    config->supervision.interest = ROTMOS_CONTROL_READ;
    config->supervision.on_event = NULL;
    config->supervision.userdata = NULL;
    config->client_ip.trusted_proxies = NULL;
    config->client_ip.trusted_proxy_count = 0;
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

void rotmos_server_config_init_production_webserver(
    rotmos_server_config *config)
{
    static const rotmos_autoblock_status_rule rules[] = {
        {401, 10}, {403, 10}, {404, 50}, {429, 5}};
    if (config == NULL)
        return;
    rotmos_server_config_init(config);
    config->limits.hard_body_bytes = config->limits.default_body_bytes;
    config->autoblock.enabled = 1;
    config->autoblock.tcp_stall_threshold = 20;
    config->autoblock.tls_failure_threshold = 10;
    config->autoblock.status_rules = rules;
    config->autoblock.status_rule_count = sizeof(rules) / sizeof(rules[0]);
}
