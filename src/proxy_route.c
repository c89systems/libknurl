#include "proxy_route_internal.h"

#include "proxy_common.h"
#include "router_internal.h"
#include "proxy_runtime.h"

#include <curl/curl.h>
#include <curl/urlapi.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ROTMOS_PROXY_DEFAULT_CONNECT_TIMEOUT_MS 10000L
#define ROTMOS_PROXY_DEFAULT_IDLE_TIMEOUT_MS 60000L
#define ROTMOS_PROXY_DEFAULT_BUFFER_LIMIT_BYTES 16384u
#define ROTMOS_PROXY_MIN_BUFFER_LIMIT_BYTES 8192u
#define ROTMOS_PROXY_MAX_BUFFER_LIMIT_BYTES 1048576u
#define ROTMOS_PROXY_MAX_TARGETS 16u
#define ROTMOS_PROXY_MAX_CA_PEM_BYTES 262144u
#define ROTMOS_PROXY_MAX_CLIENT_PEM_BYTES 262144u

static char *rotmos_proxy_copy_string(const char *value)
{
    char *copy;
    size_t length;

    length = strlen(value);
    copy = (char *)malloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, value, length + 1u);
    }
    return copy;
}

static void rotmos_proxy_route_data_free(void *userdata)
{
    rotmos_proxy_route_data *data;
    size_t i;

    data = (rotmos_proxy_route_data *)userdata;
    if (data == NULL) {
        return;
    }
    for (i = 0u; i < data->target_count; ++i) {
        free(data->targets[i]);
    }
    free(data->path);
    free(data->targets);
    free(data->tls_ca_pem);
    free(data->tls_client_cert_pem);
    if (data->tls_client_key_pem != NULL) {
        OPENSSL_cleanse(data->tls_client_key_pem,
                        data->tls_client_key_pem_length);
        free(data->tls_client_key_pem);
    }
    free(data);
}

static int rotmos_proxy_target_valid(const char *target)
{
    CURLU *url;
    CURLUcode code;
    char *scheme;
    char *host;
    char *part;
    int valid;
    size_t authority_offset;
    const unsigned char *cursor;
    const char *base_path;

    if (target == NULL || target[0] == '\0' || strchr(target, '\\') != NULL ||
        strchr(target, '?') != NULL || strchr(target, '#') != NULL) {
        return 0;
    }
    for (cursor = (const unsigned char *)target; *cursor != '\0'; ++cursor) {
        if (*cursor <= 0x20u || *cursor == 0x7fu) {
            return 0;
        }
    }
    if (strncasecmp(target, "http://", 7u) == 0) {
        authority_offset = 7u;
    } else if (strncasecmp(target, "https://", 8u) == 0) {
        authority_offset = 8u;
    } else {
        return 0;
    }
    if (target[authority_offset] == '\0' || target[authority_offset] == '/' ||
        target[authority_offset] == '?' || target[authority_offset] == '#') {
        return 0;
    }
    base_path = strchr(target + authority_offset, '/');
    if (base_path != NULL &&
        rotmos_proxy_validate_path(base_path, NULL) != ROTMOS_OK) {
        return 0;
    }
    url = curl_url();
    if (url == NULL) {
        return 0;
    }
    valid = 0;
    scheme = NULL;
    host = NULL;
    code = curl_url_set(url, CURLUPART_URL, target, 0u);
    if (code != CURLUE_OK ||
        curl_url_get(url, CURLUPART_SCHEME, &scheme, 0u) != CURLUE_OK ||
        curl_url_get(url, CURLUPART_HOST, &host, 0u) != CURLUE_OK ||
        (strcasecmp(scheme, "http") != 0 && strcasecmp(scheme, "https") != 0) ||
        host[0] == '\0') {
        goto done;
    }
    code = curl_url_get(url, CURLUPART_USER, &part, 0u);
    if (code != CURLUE_NO_USER) {
        if (code == CURLUE_OK)
            curl_free(part);
        goto done;
    }
    code = curl_url_get(url, CURLUPART_PASSWORD, &part, 0u);
    if (code != CURLUE_NO_PASSWORD) {
        if (code == CURLUE_OK)
            curl_free(part);
        goto done;
    }
    code = curl_url_get(url, CURLUPART_OPTIONS, &part, 0u);
    if (code != CURLUE_NO_OPTIONS) {
        if (code == CURLUE_OK)
            curl_free(part);
        goto done;
    }
    code = curl_url_get(url, CURLUPART_QUERY, &part, 0u);
    if (code != CURLUE_NO_QUERY) {
        if (code == CURLUE_OK)
            curl_free(part);
        goto done;
    }
    code = curl_url_get(url, CURLUPART_FRAGMENT, &part, 0u);
    if (code != CURLUE_NO_FRAGMENT) {
        if (code == CURLUE_OK)
            curl_free(part);
        goto done;
    }
    valid = 1;
done:
    curl_free(scheme);
    curl_free(host);
    curl_url_cleanup(url);
    return valid;
}

rotmos_status rotmos_router_proxy(rotmos_router *router,
                                  const rotmos_proxy_config *config,
                                  rotmos_route **out, rotmos_error *error)
{
    rotmos_proxy_route_data *data;
    rotmos_route_config route;
    rotmos_route *registered;
    rotmos_status status;
    size_t i;
    size_t count;
    size_t ca_pem_length;
    size_t client_cert_pem_length;
    size_t client_key_pem_length;
    const char *target;

    if (out != NULL)
        *out = NULL;
    if (router == NULL || config == NULL) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy route requires a router and config");
        return ROTMOS_INVALID;
    }
    if (config->path == NULL || config->target == NULL ||
        config->alternate_target_count >= ROTMOS_PROXY_MAX_TARGETS ||
        (config->alternate_target_count != 0u &&
         config->alternate_targets == NULL)) {
        rotmos_proxy_error(
            error, ROTMOS_INVALID,
            "proxy route requires a path and configured target URLs");
        return ROTMOS_INVALID;
    }
    if (config->upstream_http_version != ROTMOS_PROXY_HTTP_AUTO &&
        config->upstream_http_version != ROTMOS_PROXY_HTTP_1_1) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy upstream_http_version is invalid");
        return ROTMOS_INVALID;
    }
    if (config->connect_timeout_ms < 0L || config->idle_timeout_ms < 0L ||
        config->total_timeout_ms < 0L ||
        (config->buffer_limit_bytes != 0u &&
         (config->buffer_limit_bytes < ROTMOS_PROXY_MIN_BUFFER_LIMIT_BYTES ||
          config->buffer_limit_bytes > ROTMOS_PROXY_MAX_BUFFER_LIMIT_BYTES))) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy timeouts or buffer_limit_bytes are invalid");
        return ROTMOS_INVALID;
    }
    ca_pem_length = 0u;
    if (config->tls_ca_pem != NULL) {
        ca_pem_length = strlen(config->tls_ca_pem);
        if (ca_pem_length == 0u ||
            ca_pem_length > ROTMOS_PROXY_MAX_CA_PEM_BYTES) {
            rotmos_proxy_error(
                error, ROTMOS_INVALID,
                "proxy tls_ca_pem must contain 1 to 262144 bytes");
            return ROTMOS_INVALID;
        }
    }
    if ((config->tls_client_cert_pem == NULL) !=
        (config->tls_client_key_pem == NULL)) {
        rotmos_proxy_error(
            error, ROTMOS_INVALID,
            "proxy TLS client certificate and key must be set together");
        return ROTMOS_INVALID;
    }
    client_cert_pem_length = 0u;
    client_key_pem_length = 0u;
    if (config->tls_client_cert_pem != NULL) {
        client_cert_pem_length = strlen(config->tls_client_cert_pem);
        client_key_pem_length = strlen(config->tls_client_key_pem);
        if (client_cert_pem_length == 0u ||
            client_cert_pem_length > ROTMOS_PROXY_MAX_CLIENT_PEM_BYTES ||
            client_key_pem_length == 0u ||
            client_key_pem_length > ROTMOS_PROXY_MAX_CLIENT_PEM_BYTES) {
            rotmos_proxy_error(
                error, ROTMOS_INVALID,
                "proxy TLS client certificate and key must each contain "
                "1 to 262144 bytes");
            return ROTMOS_INVALID;
        }
    }
    count = config->alternate_target_count + 1u;
    data = (rotmos_proxy_route_data *)calloc(1u, sizeof(*data));
    if (data == NULL) {
        rotmos_proxy_error(error, ROTMOS_NOMEM,
                           "failed to allocate proxy route configuration");
        return ROTMOS_NOMEM;
    }
    data->path = rotmos_proxy_copy_string(config->path);
    if (data->path == NULL) {
        rotmos_proxy_route_data_free(data);
        rotmos_proxy_error(error, ROTMOS_NOMEM,
                           "failed to copy proxy route path");
        return ROTMOS_NOMEM;
    }
    data->targets = (char **)calloc(count, sizeof(*data->targets));
    if (data->targets == NULL) {
        rotmos_proxy_route_data_free(data);
        rotmos_proxy_error(error, ROTMOS_NOMEM,
                           "failed to allocate proxy route targets");
        return ROTMOS_NOMEM;
    }
    data->target_count = count;
    for (i = 0u; i < count; ++i) {
        target = i == 0u ? config->target : config->alternate_targets[i - 1u];
        if (!rotmos_proxy_target_valid(target)) {
            rotmos_proxy_route_data_free(data);
            rotmos_proxy_error(
                error, ROTMOS_INVALID,
                "proxy target must be an HTTP or HTTPS base URL without "
                "credentials, query, or fragment");
            return ROTMOS_INVALID;
        }
        data->targets[i] = rotmos_proxy_copy_string(target);
        if (data->targets[i] == NULL) {
            rotmos_proxy_route_data_free(data);
            rotmos_proxy_error(error, ROTMOS_NOMEM,
                               "failed to copy proxy target URL");
            return ROTMOS_NOMEM;
        }
    }
    data->upstream_http_version = config->upstream_http_version;
    data->connect_timeout_ms = config->connect_timeout_ms != 0L
                                   ? config->connect_timeout_ms
                                   : ROTMOS_PROXY_DEFAULT_CONNECT_TIMEOUT_MS;
    data->idle_timeout_ms = config->idle_timeout_ms != 0L
                                ? config->idle_timeout_ms
                                : ROTMOS_PROXY_DEFAULT_IDLE_TIMEOUT_MS;
    data->total_timeout_ms = config->total_timeout_ms;
    data->rewrite = config->rewrite;
    data->rewrite_userdata = config->rewrite_userdata;
    data->preflight = config->preflight;
    data->preflight_userdata = config->preflight_userdata;
    data->modify_response = config->modify_response;
    data->modify_response_userdata = config->modify_response_userdata;
    data->on_error = config->on_error;
    data->on_error_userdata = config->on_error_userdata;
    data->on_exchange = config->on_exchange;
    data->on_exchange_userdata = config->on_exchange_userdata;
    data->buffer_limit_bytes = config->buffer_limit_bytes != 0u
                                   ? config->buffer_limit_bytes
                                   : ROTMOS_PROXY_DEFAULT_BUFFER_LIMIT_BYTES;
    if (config->tls_ca_pem != NULL) {
        data->tls_ca_pem_length = ca_pem_length;
        data->tls_ca_pem = rotmos_proxy_copy_string(config->tls_ca_pem);
        if (data->tls_ca_pem == NULL) {
            rotmos_proxy_route_data_free(data);
            rotmos_proxy_error(error, ROTMOS_NOMEM,
                               "failed to copy proxy TLS CA bundle");
            return ROTMOS_NOMEM;
        }
    }
    if (config->tls_client_cert_pem != NULL) {
        data->tls_client_cert_pem_length = client_cert_pem_length;
        data->tls_client_cert_pem =
            rotmos_proxy_copy_string(config->tls_client_cert_pem);
        data->tls_client_key_pem_length = client_key_pem_length;
        data->tls_client_key_pem =
            rotmos_proxy_copy_string(config->tls_client_key_pem);
        if (data->tls_client_cert_pem == NULL ||
            data->tls_client_key_pem == NULL) {
            rotmos_proxy_route_data_free(data);
            rotmos_proxy_error(error, ROTMOS_NOMEM,
                               "failed to copy proxy TLS client identity");
            return ROTMOS_NOMEM;
        }
    }
    rotmos_route_config_init(&route);
    route.method = ROTMOS_HTTP_ANY;
    route.path = config->path;
    route.methods = config->methods;
    route.path_kind = config->path_kind;
    route.on_headers = rotmos_proxy_dispatch;
    route.userdata = data;
    status = rotmos_router_route(router, &route, &registered, error);
    if (status != ROTMOS_OK)
        rotmos_proxy_route_data_free(data);
    else {
        registered->release_userdata = rotmos_proxy_route_data_free;
        registered->raw_path = 1;
        if (out != NULL)
            *out = registered;
    }
    return status;
}
