#include "proxy_director.h"

#include "proxy_common.h"
#include "proxy_url.h"

#include <stdlib.h>
#include <string.h>

#define ROTMOS_PROXY_DIRECTOR_STRING_LIMIT 65536u

static rotmos_status rotmos_proxy_director_copy(char **slot, const char *value,
                                                rotmos_error *error)
{
    char *copy;
    size_t length;

    if (value == NULL) {
        free(*slot);
        *slot = NULL;
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    length = strlen(value);
    if (length > ROTMOS_PROXY_DIRECTOR_STRING_LIMIT) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy rewrite string exceeds 65536 bytes");
        return ROTMOS_INVALID;
    }
    copy = (char *)malloc(length + 1u);
    if (copy == NULL) {
        rotmos_proxy_error(error, ROTMOS_NOMEM,
                           "failed to copy proxy rewrite string");
        return ROTMOS_NOMEM;
    }
    memcpy(copy, value, length + 1u);
    free(*slot);
    *slot = copy;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static int rotmos_proxy_director_host_valid(const char *host)
{
    const unsigned char *cursor;

    if (host == NULL || host[0] == '\0')
        return 0;
    for (cursor = (const unsigned char *)host; *cursor != '\0'; ++cursor) {
        if (*cursor <= 0x20u || *cursor >= 0x7fu || *cursor == '/' ||
            *cursor == '\\' || *cursor == '@' || *cursor == '?' ||
            *cursor == '#')
            return 0;
    }
    return 1;
}

rotmos_http_method rotmos_proxy_inbound_method(const rotmos_proxy_inbound *in)
{
    return in != NULL ? in->method : ROTMOS_HTTP_GET;
}

const char *rotmos_proxy_inbound_path(const rotmos_proxy_inbound *in)
{
    return in != NULL ? in->path : NULL;
}

const char *rotmos_proxy_inbound_query(const rotmos_proxy_inbound *in)
{
    return in != NULL ? in->query : NULL;
}

const char *rotmos_proxy_inbound_host(const rotmos_proxy_inbound *in)
{
    return in != NULL ? in->host : NULL;
}

int rotmos_proxy_inbound_websocket(const rotmos_proxy_inbound *in)
{
    return in != NULL && in->websocket;
}

rotmos_bytes rotmos_proxy_inbound_path_param(const rotmos_proxy_inbound *in,
                                             const char *name)
{
    rotmos_bytes value;
    value.data = NULL;
    value.length = 0;
    if (in != NULL && in->matched_request != NULL && name != NULL)
        (void)rotmos_request_path_param(in->matched_request, name, &value,
                                        NULL);
    return value;
}

size_t rotmos_proxy_inbound_header_count(const rotmos_proxy_inbound *in)
{
    return in != NULL && in->headers != NULL ? in->headers->count : 0u;
}

rotmos_status rotmos_proxy_inbound_header_at(const rotmos_proxy_inbound *in,
                                             size_t index, const char **name,
                                             const char **value)
{
    if (name != NULL)
        *name = NULL;
    if (value != NULL)
        *value = NULL;
    if (in == NULL || in->headers == NULL || name == NULL || value == NULL ||
        index >= in->headers->count)
        return ROTMOS_INVALID;
    *name = in->headers->fields[index].name;
    *value = in->headers->fields[index].value;
    return ROTMOS_OK;
}

rotmos_status rotmos_proxy_outbound_select_target(rotmos_proxy_outbound *out,
                                                  size_t index,
                                                  rotmos_error *error)
{
    if (out == NULL || out->route == NULL ||
        index >= out->route->target_count) {
        if (out != NULL)
            out->failure = ROTMOS_INVALID;
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy rewrite target is not configured");
        return ROTMOS_INVALID;
    }
    out->target_index = index;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_proxy_outbound_set_method(rotmos_proxy_outbound *out,
                                               rotmos_http_method method,
                                               rotmos_error *error)
{
    if (out == NULL || method < ROTMOS_HTTP_GET || method > ROTMOS_HTTP_MOVE ||
        (out->websocket && method != ROTMOS_HTTP_GET)) {
        if (out != NULL)
            out->failure = ROTMOS_INVALID;
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy rewrite method is invalid for this request");
        return ROTMOS_INVALID;
    }
    out->method = method;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_proxy_outbound_set_path(rotmos_proxy_outbound *out,
                                             const char *raw_path,
                                             rotmos_error *error)
{
    rotmos_status status;

    if (out == NULL || raw_path == NULL ||
        rotmos_proxy_validate_path(raw_path, error) != ROTMOS_OK) {
        if (out != NULL)
            out->failure = ROTMOS_INVALID;
        if (out == NULL || raw_path == NULL)
            rotmos_proxy_error(error, ROTMOS_INVALID,
                               "proxy rewrite path is required");
        return ROTMOS_INVALID;
    }
    status = rotmos_proxy_director_copy(&out->path, raw_path, error);
    if (status != ROTMOS_OK)
        out->failure = status;
    return status;
}

rotmos_status rotmos_proxy_outbound_set_query(rotmos_proxy_outbound *out,
                                              const char *raw_query,
                                              rotmos_error *error)
{
    rotmos_status status;

    if (out == NULL) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy rewrite output is required");
        return ROTMOS_INVALID;
    }
    status = rotmos_proxy_director_copy(&out->query, raw_query, error);
    if (status != ROTMOS_OK)
        out->failure = status;
    return status;
}

rotmos_status rotmos_proxy_outbound_set_host(rotmos_proxy_outbound *out,
                                             const char *host,
                                             rotmos_error *error)
{
    rotmos_status status;

    if (out == NULL ||
        (host != NULL && !rotmos_proxy_director_host_valid(host))) {
        if (out != NULL)
            out->failure = ROTMOS_INVALID;
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy rewrite Host is invalid");
        return ROTMOS_INVALID;
    }
    status = rotmos_proxy_director_copy(&out->host, host, error);
    if (status != ROTMOS_OK)
        out->failure = status;
    return status;
}

static rotmos_status
rotmos_proxy_director_header_status(rotmos_proxy_header_status status,
                                    rotmos_error *error)
{
    if (status == ROTMOS_PROXY_HEADER_OK) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (status == ROTMOS_PROXY_HEADER_NOMEM) {
        rotmos_proxy_error(error, ROTMOS_NOMEM,
                           "failed to allocate proxy rewrite header");
        return ROTMOS_NOMEM;
    }
    rotmos_proxy_error(error, ROTMOS_INVALID,
                       "proxy rewrite header is invalid or exceeds limits");
    return ROTMOS_INVALID;
}

rotmos_status rotmos_proxy_outbound_add_header(rotmos_proxy_outbound *out,
                                               const char *name,
                                               const char *value,
                                               rotmos_error *error)
{
    rotmos_status status;

    if (out == NULL || !rotmos_proxy_request_header_editable(name)) {
        if (out != NULL)
            out->failure = ROTMOS_INVALID;
        rotmos_proxy_error(
            error, ROTMOS_INVALID,
            "proxy rewrite cannot edit transport or handshake headers");
        return ROTMOS_INVALID;
    }
    status = rotmos_proxy_director_header_status(
        rotmos_proxy_headers_add(&out->headers, name, value), error);
    if (status != ROTMOS_OK)
        out->failure = status;
    return status;
}

rotmos_status rotmos_proxy_outbound_remove_header(rotmos_proxy_outbound *out,
                                                  const char *name,
                                                  rotmos_error *error)
{
    if (out == NULL || !rotmos_proxy_request_header_editable(name)) {
        if (out != NULL)
            out->failure = ROTMOS_INVALID;
        rotmos_proxy_error(
            error, ROTMOS_INVALID,
            "proxy rewrite cannot edit transport or handshake headers");
        return ROTMOS_INVALID;
    }
    rotmos_proxy_headers_remove(&out->headers, name);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_proxy_outbound_set_header(rotmos_proxy_outbound *out,
                                               const char *name,
                                               const char *value,
                                               rotmos_error *error)
{
    rotmos_status status;

    if (out == NULL || !rotmos_proxy_request_header_editable(name)) {
        if (out != NULL)
            out->failure = ROTMOS_INVALID;
        rotmos_proxy_error(
            error, ROTMOS_INVALID,
            "proxy rewrite cannot edit transport or handshake headers");
        return ROTMOS_INVALID;
    }
    status = rotmos_proxy_director_header_status(
        rotmos_proxy_headers_set(&out->headers, name, value), error);
    if (status != ROTMOS_OK)
        out->failure = status;
    return status;
}

void rotmos_proxy_director_cleanup(rotmos_proxy_outbound *out)
{
    if (out == NULL)
        return;
    rotmos_proxy_headers_cleanup(&out->headers);
    free(out->path);
    free(out->query);
    free(out->host);
    memset(out, 0, sizeof(*out));
}

rotmos_status rotmos_proxy_director_prepare(
    const rotmos_proxy_route_data *route, rotmos_http_method method,
    const char *path, const char *query, const char *host, int websocket,
    rotmos_request *matched_request, const rotmos_proxy_headers *inbound,
    const rotmos_proxy_headers *sanitized, rotmos_proxy_outbound *out,
    char **target, char **authority, rotmos_error *error)
{
    rotmos_proxy_inbound input;
    rotmos_status status;
    char *initial_target;
    char *initial_authority;
    char *default_authority;
    size_t i;

    if (out == NULL || target == NULL || authority == NULL || route == NULL ||
        route->target_count == 0u || sanitized == NULL || inbound == NULL ||
        path == NULL || host == NULL) {
        rotmos_proxy_error(
            error, ROTMOS_INVALID,
            "proxy director requires request metadata and route");
        return ROTMOS_INVALID;
    }
    *target = NULL;
    *authority = NULL;
    memset(out, 0, sizeof(*out));
    initial_target = NULL;
    initial_authority = NULL;
    status =
        rotmos_proxy_target_build(route->targets[0], path, query,
                                  &initial_target, &initial_authority, error);
    free(initial_target);
    free(initial_authority);
    if (status != ROTMOS_OK)
        return status;
    out->route = route;
    out->method = method;
    out->websocket = websocket;
    for (i = 0u; i < sanitized->count; ++i) {
        if (rotmos_proxy_headers_add(&out->headers, sanitized->fields[i].name,
                                     sanitized->fields[i].value) !=
            ROTMOS_PROXY_HEADER_OK) {
            rotmos_proxy_error(error, ROTMOS_NOMEM,
                               "failed to copy proxy outbound headers");
            return ROTMOS_NOMEM;
        }
    }
    status = rotmos_proxy_director_copy(&out->path, path, error);
    if (status == ROTMOS_OK)
        status = rotmos_proxy_director_copy(&out->query, query, error);
    if (status != ROTMOS_OK)
        return status;
    input.method = method;
    input.path = path;
    input.query = query;
    input.host = host;
    input.headers = inbound;
    input.matched_request = matched_request;
    input.websocket = websocket;
    if (route->rewrite != NULL) {
        status = route->rewrite(&input, out, route->rewrite_userdata, error);
        if (status != ROTMOS_OK)
            return status;
        if (out->failure != ROTMOS_OK) {
            rotmos_proxy_error(error, out->failure,
                               "proxy rewrite attempted an invalid edit");
            return out->failure;
        }
    }
    status = rotmos_proxy_target_build(route->targets[out->target_index],
                                       out->path, out->query, target,
                                       &default_authority, error);
    if (status != ROTMOS_OK)
        return status;
    if (out->websocket && out->method != ROTMOS_HTTP_GET) {
        free(*target);
        *target = NULL;
        free(default_authority);
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy WebSocket rewrite must retain GET");
        return ROTMOS_INVALID;
    }
    if (out->host != NULL) {
        free(default_authority);
        status = rotmos_proxy_director_copy(authority, out->host, error);
        if (status != ROTMOS_OK) {
            free(*target);
            *target = NULL;
            return status;
        }
    } else {
        *authority = default_authority;
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
