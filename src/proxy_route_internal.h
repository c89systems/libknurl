#ifndef KNURL_PROXY_ROUTE_INTERNAL_H
#define KNURL_PROXY_ROUTE_INTERNAL_H

#include <knurl/proxy.h>

typedef struct knurl_proxy_route_data {
    char *path;
    char **targets;
    size_t target_count;
    knurl_proxy_http_version upstream_http_version;
    long connect_timeout_ms;
    long idle_timeout_ms;
    long total_timeout_ms;
    size_t buffer_limit_bytes;
    char *tls_ca_pem;
    size_t tls_ca_pem_length;
    char *tls_client_cert_pem;
    size_t tls_client_cert_pem_length;
    char *tls_client_key_pem;
    size_t tls_client_key_pem_length;
    knurl_proxy_rewrite_fn rewrite;
    void *rewrite_userdata;
    knurl_proxy_preflight_fn preflight;
    void *preflight_userdata;
    knurl_proxy_modify_response_fn modify_response;
    void *modify_response_userdata;
    knurl_proxy_on_error_fn on_error;
    void *on_error_userdata;
    knurl_proxy_exchange_fn on_exchange;
    void *on_exchange_userdata;
} knurl_proxy_route_data;

#endif
