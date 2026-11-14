#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "server_internal.h"
#include "file_internal.h"
#include "acme_internal.h"
#include "ip.h"

#include <arpa/inet.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

static knurl_status invalid(knurl_error *error, const char *message)
{
    return knurl_fail(error, KNURL_INVALID, "server_config", message);
}

static int text(const char *value)
{
    return value != NULL && *value != '\0' && knurl_header_value_valid(value);
}

static int optional_text(const char *value)
{
    return value == NULL || text(value);
}

static knurl_status deadline(knurl_deadline *value, long fallback, int seconds,
                             knurl_error *error)
{
    long rounded;
    if (value->milliseconds < 0)
        return invalid(error, "deadline milliseconds must be nonnegative; use "
                              "disabled to turn off a deadline");
    if (value->disabled) {
        value->milliseconds = 0;
        value->disabled = 1;
        return KNURL_OK;
    }
    if (value->milliseconds == 0)
        value->milliseconds = fallback;
    if (seconds) {
        rounded = value->milliseconds / 1000;
        if (value->milliseconds % 1000 != 0)
            ++rounded;
        if (rounded > 65535)
            rounded = 65535;
        value->milliseconds = rounded * 1000;
    }
    return KNURL_OK;
}

static int material_valid(const knurl_tls_material *material, size_t maximum)
{
    switch (material->kind) {
    case KNURL_MATERIAL_NONE:
        return material->path == NULL && material->bytes.data == NULL &&
               material->bytes.length == 0 && material->open == NULL;
    case KNURL_MATERIAL_FILE:
        return text(material->path) && material->bytes.data == NULL &&
               material->bytes.length == 0 && material->open == NULL;
    case KNURL_MATERIAL_MEMORY:
        return material->path == NULL && material->bytes.data != NULL &&
               material->bytes.length != 0 &&
               material->bytes.length <= INT_MAX - 1UL &&
               material->bytes.length <= maximum && material->open == NULL;
    case KNURL_MATERIAL_CALLBACK:
        return material->path == NULL && material->bytes.data == NULL &&
               material->bytes.length == 0 && material->open != NULL;
    }
    return 0;
}

static knurl_status tls_validate(knurl_tls_config *config, knurl_error *error)
{
    const knurl_tls_identity *identity;
    const char *domain;
    size_t i;
    size_t j;
    size_t k;
    int bundled;
    int split;
    if (config->version < KNURL_TLS_VERSION_DEFAULT ||
        config->version > KNURL_TLS_VERSION_1_3)
        return invalid(error, "unknown TLS protocol version selection");
    if (config->max_material_bytes == 0)
        config->max_material_bytes = (size_t)-1;
    if (!optional_text(config->cipher_list) ||
        !optional_text(config->acme.email) ||
        !optional_text(config->acme.directory_url) ||
        !optional_text(config->acme.runtime_directory) ||
        !optional_text(config->acme.ca_file) ||
        !optional_text(config->acme.ca_directory))
        return invalid(error, "TLS configuration text must be nonempty and "
                              "contain no control characters");
    if (config->acme.persist_timeout_ms < 0 ||
        (unsigned long)config->acme.persist_timeout_ms > 0x7fffffffUL)
        return invalid(
            error,
            "ACME persistence timeout must be 0 or 1..2147483647 milliseconds");
    if (config->acme.persist_timeout_ms == 0)
        config->acme.persist_timeout_ms = 30000;
    if (config->identity_count > (size_t)-1 / sizeof(*identity) ||
        (config->identity_count != 0 && config->identities == NULL))
        return invalid(error, "invalid TLS identity array");
    for (i = 0; i < config->identity_count; ++i) {
        identity = &config->identities[i];
        if (identity->mode != KNURL_TLS_MANUAL &&
            identity->mode != KNURL_TLS_ACME)
            return invalid(
                error, "configured TLS identities require MANUAL or ACME mode");
        if (identity->domain_count == 0 || identity->domains == NULL ||
            identity->domain_count > (size_t)-1 / sizeof(*identity->domains))
            return invalid(error,
                           "TLS identities require an explicit domain array");
        for (j = 0; j < identity->domain_count; ++j) {
            domain = identity->domains[j];
            if (!text(domain) || strlen(domain) > 253 ||
                strpbrk(domain, "/\\\t ") != NULL ||
                (identity->mode == KNURL_TLS_ACME &&
                 !knurl_acme_domain_valid(domain)))
                return invalid(error, "invalid TLS domain; ACME wildcard "
                                      "identities are unsupported");
            for (k = 0; k < j; ++k) {
                if (knurl_text_equal_case(domain, identity->domains[k]))
                    return invalid(error,
                                   "duplicate TLS domain in an identity");
            }
        }
        if (!material_valid(&identity->bundle, config->max_material_bytes) ||
            !material_valid(&identity->certificate,
                            config->max_material_bytes) ||
            !material_valid(&identity->private_key,
                            config->max_material_bytes) ||
            !material_valid(&identity->extra_chain,
                            config->max_material_bytes) ||
            !material_valid(&identity->client_ca, config->max_material_bytes))
            return invalid(error, "TLS material requires one matching source "
                                  "kind within max_material_bytes");
        bundled = identity->bundle.kind != KNURL_MATERIAL_NONE;
        split = identity->certificate.kind != KNURL_MATERIAL_NONE ||
                identity->private_key.kind != KNURL_MATERIAL_NONE;
        if (identity->mode == KNURL_TLS_MANUAL &&
            ((bundled && split) ||
             (!bundled && (identity->certificate.kind == KNURL_MATERIAL_NONE ||
                           identity->private_key.kind == KNURL_MATERIAL_NONE))))
            return invalid(error, "manual TLS requires a bundle or both "
                                  "certificate and private key");
        if (identity->mode == KNURL_TLS_ACME &&
            (bundled || split || config->acme.state == NULL ||
             !text(config->acme.email) || !text(config->acme.directory_url) ||
             !text(config->acme.runtime_directory)))
            return invalid(error, "ACME requires explicit account, directory, "
                                  "runtime directory and state callback");
        if (identity->require_client_certificate &&
            identity->client_ca.kind == KNURL_MATERIAL_NONE)
            return invalid(
                error, "required client certificates need a client CA source");
    }
    return KNURL_OK;
}

knurl_status knurl_config_normalize(knurl_server_config *config,
                                    knurl_error *error)
{
    knurl_status status;
    long cpus;
    size_t i;
    struct knurl_ip_network network;
    off_t log_limit;
    const char *proxy;
    if (config->name == NULL)
        config->name = "knurl";
    if (config->http.server_header == NULL)
        config->http.server_header = "knurl";
    if (config->http.access_log_max_bytes.high == 0 &&
        config->http.access_log_max_bytes.low == 0)
        config->http.access_log_max_bytes.low = 10UL * 1024UL * 1024UL;
    if (config->http.access_log_file_count == 0)
        config->http.access_log_file_count = 3;
    if (!knurl_u64_valid(config->http.access_log_max_bytes) ||
        !knurl_file_offset(config->http.access_log_max_bytes, &log_limit))
        return knurl_fail(
            error, KNURL_INVALID, "server_new",
            "access_log_max_bytes must fit a positive file offset");
    if (!text(config->name) || !text(config->http.server_header) ||
        strlen(config->http.server_header) > 53 ||
        !optional_text(config->limits.body_spool_directory) ||
        !optional_text(config->http.access_log_path))
        return invalid(
            error,
            "invalid server/path text; Server header must contain 1..53 bytes");
    if (config->workers.count > 253)
        return invalid(error,
                       "HTTP worker count must be 0 (automatic) or 1..253");
    if (config->workers.count == 0) {
        cpus = sysconf(_SC_NPROCESSORS_ONLN);
        config->workers.count = cpus > 0 ? (size_t)cpus : 1;
        if (config->workers.count > 253)
            config->workers.count = 253;
    }
#define DEFAULT_FIELD(field, value)                                            \
    do {                                                                       \
        if (config->field == 0)                                                \
            config->field = value;                                             \
    } while (0)
    DEFAULT_FIELD(workers.accept_threshold, 16);
    DEFAULT_FIELD(workers.rlimit_nofiles, 768);
    DEFAULT_FIELD(workers.shutdown_timeout_ms, 5000);
    DEFAULT_FIELD(limits.max_connections, 1024);
    DEFAULT_FIELD(limits.request_limit, config->limits.max_connections);
    DEFAULT_FIELD(limits.max_request_header_bytes, 65536UL);
    DEFAULT_FIELD(limits.websocket_max_frame_bytes, 16384);
    DEFAULT_FIELD(http.keepalive_max_requests, 100);
    DEFAULT_FIELD(http.socket_backlog, 5000);
    DEFAULT_FIELD(http.request_process_budget_ms, 100);
    DEFAULT_FIELD(http.backend_curl_timeout_seconds, 60);
    DEFAULT_FIELD(io.chunk_bytes, 8192);
    DEFAULT_FIELD(io.input_queue_bytes, 262144UL);
    DEFAULT_FIELD(io.output_queue_bytes, 262144UL);
    DEFAULT_FIELD(autoblock.window_seconds, 1800);
    DEFAULT_FIELD(autoblock.block_seconds, 3600);
    DEFAULT_FIELD(autoblock.max_entries, 8192);
#undef DEFAULT_FIELD
    if (config->limits.default_body_bytes.high == 0 &&
        config->limits.default_body_bytes.low == 0)
        config->limits.default_body_bytes.low = 10UL * 1024UL * 1024UL;
    if (config->limits.body_min_rate_bytes_per_second.high == 0 &&
        config->limits.body_min_rate_bytes_per_second.low == 0)
        config->limits.body_min_rate_bytes_per_second.low = 1024;
    if (config->http.backend_curl_recv_max_bytes.high == 0 &&
        config->http.backend_curl_recv_max_bytes.low == 0)
        config->http.backend_curl_recv_max_bytes.low = 2UL * 1024UL * 1024UL;
    if (!knurl_u64_valid(config->limits.default_body_bytes) ||
        !knurl_u64_valid(config->limits.hard_body_bytes) ||
        !knurl_u64_valid(config->limits.body_min_rate_bytes_per_second) ||
        !knurl_u64_valid(config->http.backend_curl_recv_max_bytes))
        return invalid(
            error,
            "wide configuration values require two unsigned 32-bit words");
    if (config->workers.accept_threshold > 0xffffffffUL ||
        config->workers.rlimit_nofiles > 0xffffffffUL ||
        config->limits.max_connections > 0xffffffffUL ||
        config->limits.request_limit > 0xffffffffUL ||
        config->workers.shutdown_timeout_ms < 0 ||
        (unsigned long)config->workers.shutdown_timeout_ms > 0xffffffffUL ||
        config->http.backend_curl_timeout_seconds > 65535 ||
        config->http.socket_backlog > INT_MAX ||
        config->limits.max_request_header_bytes < 1024 ||
        (config->limits.max_response_header_bytes != 0 &&
         config->limits.max_response_header_bytes < 1024))
        return invalid(error, "connection, worker, backlog, header or curl "
                              "limit exceeds backend range");
    if ((config->dispatch.mode != KNURL_DISPATCH_ROUTER &&
         config->dispatch.mode != KNURL_DISPATCH_CALLBACK) ||
        (config->dispatch.mode == KNURL_DISPATCH_CALLBACK &&
         config->dispatch.on_headers == NULL) ||
        (config->dispatch.mode == KNURL_DISPATCH_ROUTER &&
         config->dispatch.on_headers != NULL))
        return invalid(error, "select ROUTER or CALLBACK with its required "
                              "on_headers dispatcher");
    if ((config->workers.death_policy != KNURL_WORKER_RESTART &&
         config->workers.death_policy != KNURL_WORKER_TERMINATE) ||
        (config->signals.mode != KNURL_SIGNALS_HOST &&
         config->signals.mode != KNURL_SIGNALS_LIBRARY))
        return invalid(error,
                       "unknown worker death policy or signal ownership mode");
    if (config->io.output_queue_bytes <= KNURL_RESPONSE_CHUNK_OVERHEAD)
        return invalid(error, "output queue must hold at least 33 bytes for "
                              "response framing and payload progress");
    if (config->io.chunk_bytes > config->io.input_queue_bytes ||
        config->io.chunk_bytes > config->io.output_queue_bytes)
        return invalid(error, "I/O chunk must fit each transport queue");
#define DEADLINE(field, fallback, seconds)                                     \
    do {                                                                       \
        status = deadline(&config->timeouts.field, fallback, seconds, error);  \
        if (status != KNURL_OK)                                                \
            return status;                                                     \
    } while (0)
    DEADLINE(startup, 30000, 0);
    DEADLINE(request_header, 5000, 1);
    DEADLINE(request_body_idle, 15000, 1);
    DEADLINE(response_write_idle, 15000, 0);
    DEADLINE(body_min_rate_grace, 10000, 0);
    DEADLINE(connection_idle, 30000, 0);
    DEADLINE(keepalive, 5000, 1);
    DEADLINE(websocket_idle, 120000, 0);
    DEADLINE(deferred_response, 30000, 0);
#undef DEADLINE
    if ((unsigned long)config->timeouts.response_write_idle.milliseconds >
        0xffffffffUL)
        return invalid(
            error,
            "response write-idle timeout exceeds backend milliseconds range");
    if (config->proxy.active_exchanges > LONG_MAX ||
        config->proxy.idle_connections > LONG_MAX)
        return invalid(
            error, "proxy resource limits exceed the transport representation");
    if (config->proxy.active_exchanges == 0)
        config->proxy.active_exchanges = 16;
    if (config->proxy.idle_connections == 0)
        config->proxy.idle_connections = 4;
    status = tls_validate(&config->tls, error);
    if (status != KNURL_OK)
        return status;
    if (config->listener_count > 255 ||
        (config->listener_count != 0 && config->listeners == NULL))
        return invalid(
            error, "listener array must contain at most 255 explicit records");
    for (i = 0; i < config->listener_count; ++i) {
        if (!text(config->listeners[i].bind) ||
            !optional_text(config->listeners[i].name) ||
            config->listeners[i].kind < KNURL_LISTENER_HTTP ||
            config->listeners[i].kind > KNURL_LISTENER_REDIRECT)
            return invalid(error,
                           "listener requires a bind address and known kind");
        if (config->listeners[i].kind != KNURL_LISTENER_HTTP &&
            config->listeners[i].identity_index >= config->tls.identity_count)
            return invalid(
                error,
                "HTTPS/redirect listener references an absent TLS identity");
        if (config->listeners[i].kind == KNURL_LISTENER_REDIRECT &&
            config->listeners[i].redirect_https_port == 0)
            return invalid(error,
                           "redirect listener requires a presented HTTPS port");
    }
    if (config->client_ip.forwarded_header == NULL)
        config->client_ip.forwarded_header = "x-forwarded-for";
    if (config->client_ip.real_ip_header == NULL)
        config->client_ip.real_ip_header = "x-real-ip";
    if (config->client_ip.max_hops == 0)
        config->client_ip.max_hops = 16;
    if (config->client_ip.max_hops > 64 ||
        config->client_ip.forwarded_format < KNURL_FORWARDED_COMMA ||
        config->client_ip.forwarded_format > KNURL_FORWARDED_RFC7239 ||
        (config->client_ip.forwarded_header[0] &&
         !knurl_header_name_valid(config->client_ip.forwarded_header)) ||
        (config->client_ip.real_ip_header[0] &&
         !knurl_header_name_valid(config->client_ip.real_ip_header)))
        return invalid(error, "valid forwarding header names, format and at "
                              "most 64 hops required");
    if (config->client_ip.trusted_proxy_count > 16 ||
        (config->client_ip.trusted_proxy_count != 0 &&
         config->client_ip.trusted_proxies == NULL))
        return invalid(error, "trusted proxy array must contain at most 16 "
                              "addresses/CIDRs or * entries");
    for (i = 0; i < config->client_ip.trusted_proxy_count; ++i) {
        proxy = config->client_ip.trusted_proxies[i];
        if (proxy == NULL || (strcmp(proxy, "*") != 0 &&
                              !knurl_ip_network_parse(proxy, &network)))
            return invalid(
                error,
                "trusted proxy must be a numeric IPv4/IPv6 address, CIDR or *");
    }
    if (config->autoblock.status_rule_count > 16 ||
        config->autoblock.event_rule_count > 16 ||
        (config->autoblock.status_rule_count != 0 &&
         config->autoblock.status_rules == NULL) ||
        (config->autoblock.event_rule_count != 0 &&
         config->autoblock.event_rules == NULL) ||
        (config->autoblock.enabled && config->autoblock.max_entries > 65536UL))
        return invalid(
            error,
            "autoblock rule arrays or entry limit exceed backend bounds");
    if (config->autoblock.enabled) {
        for (i = 0; i < config->autoblock.status_rule_count; ++i) {
            if (config->autoblock.status_rules[i].status < 100 ||
                config->autoblock.status_rules[i].status > 599 ||
                config->autoblock.status_rules[i].threshold == 0)
                return invalid(error, "enabled autoblock status rules require "
                                      "HTTP 100..599 and a positive threshold");
        }
        for (i = 0; i < config->autoblock.event_rule_count; ++i) {
            if (!text(config->autoblock.event_rules[i].name) ||
                config->autoblock.event_rules[i].threshold == 0)
                return invalid(error, "enabled autoblock event rules require a "
                                      "name and positive threshold");
        }
    }
    knurl_error_clear(error);
    return KNURL_OK;
}
