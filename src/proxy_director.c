#include "proxy_director.h"

#include "proxy_common.h"
#include "proxy_url.h"

#include <stdlib.h>
#include <string.h>

#define KNURL_PROXY_DIRECTOR_STRING_LIMIT 65536u

static knurl_status knurl_proxy_director_copy(char **slot, const char *value,
                                              knurl_error *error)
{
    char *copy;
    size_t length;

    if (value == NULL) {
        free(*slot);
        *slot = NULL;
        knurl_error_clear(error);
        return KNURL_OK;
    }
    length = strlen(value);
    if (length > KNURL_PROXY_DIRECTOR_STRING_LIMIT) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy rewrite string exceeds 65536 bytes");
        return KNURL_INVALID;
    }
    copy = (char *)malloc(length + 1u);
    if (copy == NULL) {
        knurl_proxy_error(error, KNURL_NOMEM,
                          "failed to copy proxy rewrite string");
        return KNURL_NOMEM;
    }
    memcpy(copy, value, length + 1u);
    free(*slot);
    *slot = copy;
    knurl_error_clear(error);
    return KNURL_OK;
}

static int knurl_proxy_director_host_valid(const char *host)
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

knurl_http_method knurl_proxy_inbound_method(const knurl_proxy_inbound *in)
{
    return in != NULL ? in->method : KNURL_HTTP_GET;
}

const char *knurl_proxy_inbound_path(const knurl_proxy_inbound *in)
{
    return in != NULL ? in->path : NULL;
}

const char *knurl_proxy_inbound_query(const knurl_proxy_inbound *in)
{
    return in != NULL ? in->query : NULL;
}

const char *knurl_proxy_inbound_host(const knurl_proxy_inbound *in)
{
    return in != NULL ? in->host : NULL;
}

int knurl_proxy_inbound_websocket(const knurl_proxy_inbound *in)
{
    return in != NULL && in->websocket;
}

knurl_bytes knurl_proxy_inbound_path_param(const knurl_proxy_inbound *in,
                                           const char *name)
{
    knurl_bytes value;
    value.data = NULL;
    value.length = 0;
    if (in != NULL && in->matched_request != NULL && name != NULL)
        (void)knurl_request_path_param(in->matched_request, name, &value, NULL);
    return value;
}

size_t knurl_proxy_inbound_header_count(const knurl_proxy_inbound *in)
{
    return in != NULL && in->headers != NULL ? in->headers->count : 0u;
}

knurl_status knurl_proxy_inbound_header_at(const knurl_proxy_inbound *in,
                                           size_t index, const char **name,
                                           const char **value)
{
    if (name != NULL)
        *name = NULL;
    if (value != NULL)
        *value = NULL;
    if (in == NULL || in->headers == NULL || name == NULL || value == NULL ||
        index >= in->headers->count)
        return KNURL_INVALID;
    *name = in->headers->fields[index].name;
    *value = in->headers->fields[index].value;
    return KNURL_OK;
}

knurl_status knurl_proxy_outbound_select_target(knurl_proxy_outbound *out,
                                                size_t index,
                                                knurl_error *error)
{
    if (out == NULL || out->route == NULL ||
        index >= out->route->target_count) {
        if (out != NULL)
            out->failure = KNURL_INVALID;
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy rewrite target is not configured");
        return KNURL_INVALID;
    }
    out->target_index = index;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_proxy_outbound_set_method(knurl_proxy_outbound *out,
                                             knurl_http_method method,
                                             knurl_error *error)
{
    if (out == NULL || method < KNURL_HTTP_GET || method > KNURL_HTTP_MOVE ||
        (out->websocket && method != KNURL_HTTP_GET)) {
        if (out != NULL)
            out->failure = KNURL_INVALID;
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy rewrite method is invalid for this request");
        return KNURL_INVALID;
    }
    out->method = method;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_proxy_outbound_set_path(knurl_proxy_outbound *out,
                                           const char *raw_path,
                                           knurl_error *error)
{
    knurl_status status;

    if (out == NULL || raw_path == NULL ||
        knurl_proxy_validate_path(raw_path, error) != KNURL_OK) {
        if (out != NULL)
            out->failure = KNURL_INVALID;
        if (out == NULL || raw_path == NULL)
            knurl_proxy_error(error, KNURL_INVALID,
                              "proxy rewrite path is required");
        return KNURL_INVALID;
    }
    status = knurl_proxy_director_copy(&out->path, raw_path, error);
    if (status != KNURL_OK)
        out->failure = status;
    return status;
}

knurl_status knurl_proxy_outbound_set_query(knurl_proxy_outbound *out,
                                            const char *raw_query,
                                            knurl_error *error)
{
    knurl_status status;

    if (out == NULL) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy rewrite output is required");
        return KNURL_INVALID;
    }
    status = knurl_proxy_director_copy(&out->query, raw_query, error);
    if (status != KNURL_OK)
        out->failure = status;
    return status;
}

knurl_status knurl_proxy_outbound_set_host(knurl_proxy_outbound *out,
                                           const char *host, knurl_error *error)
{
    knurl_status status;

    if (out == NULL ||
        (host != NULL && !knurl_proxy_director_host_valid(host))) {
        if (out != NULL)
            out->failure = KNURL_INVALID;
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy rewrite Host is invalid");
        return KNURL_INVALID;
    }
    status = knurl_proxy_director_copy(&out->host, host, error);
    if (status != KNURL_OK)
        out->failure = status;
    return status;
}

static knurl_status
knurl_proxy_director_header_status(knurl_proxy_header_status status,
                                   knurl_error *error)
{
    if (status == KNURL_PROXY_HEADER_OK) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (status == KNURL_PROXY_HEADER_NOMEM) {
        knurl_proxy_error(error, KNURL_NOMEM,
                          "failed to allocate proxy rewrite header");
        return KNURL_NOMEM;
    }
    knurl_proxy_error(error, KNURL_INVALID,
                      "proxy rewrite header is invalid or exceeds limits");
    return KNURL_INVALID;
}

knurl_status knurl_proxy_outbound_add_header(knurl_proxy_outbound *out,
                                             const char *name,
                                             const char *value,
                                             knurl_error *error)
{
    knurl_status status;

    if (out == NULL || !knurl_proxy_request_header_editable(name)) {
        if (out != NULL)
            out->failure = KNURL_INVALID;
        knurl_proxy_error(
            error, KNURL_INVALID,
            "proxy rewrite cannot edit transport or handshake headers");
        return KNURL_INVALID;
    }
    status = knurl_proxy_director_header_status(
        knurl_proxy_headers_add(&out->headers, name, value), error);
    if (status != KNURL_OK)
        out->failure = status;
    return status;
}

knurl_status knurl_proxy_outbound_remove_header(knurl_proxy_outbound *out,
                                                const char *name,
                                                knurl_error *error)
{
    if (out == NULL || !knurl_proxy_request_header_editable(name)) {
        if (out != NULL)
            out->failure = KNURL_INVALID;
        knurl_proxy_error(
            error, KNURL_INVALID,
            "proxy rewrite cannot edit transport or handshake headers");
        return KNURL_INVALID;
    }
    knurl_proxy_headers_remove(&out->headers, name);
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_proxy_outbound_set_header(knurl_proxy_outbound *out,
                                             const char *name,
                                             const char *value,
                                             knurl_error *error)
{
    knurl_status status;

    if (out == NULL || !knurl_proxy_request_header_editable(name)) {
        if (out != NULL)
            out->failure = KNURL_INVALID;
        knurl_proxy_error(
            error, KNURL_INVALID,
            "proxy rewrite cannot edit transport or handshake headers");
        return KNURL_INVALID;
    }
    status = knurl_proxy_director_header_status(
        knurl_proxy_headers_set(&out->headers, name, value), error);
    if (status != KNURL_OK)
        out->failure = status;
    return status;
}

void knurl_proxy_director_cleanup(knurl_proxy_outbound *out)
{
    if (out == NULL)
        return;
    knurl_proxy_headers_cleanup(&out->headers);
    free(out->path);
    free(out->query);
    free(out->host);
    memset(out, 0, sizeof(*out));
}

knurl_status knurl_proxy_director_prepare(
    const knurl_proxy_route_data *route, knurl_http_method method,
    const char *path, const char *query, const char *host, int websocket,
    knurl_request *matched_request, const knurl_proxy_headers *inbound,
    const knurl_proxy_headers *sanitized, knurl_proxy_outbound *out,
    char **target, char **authority, knurl_error *error)
{
    knurl_proxy_inbound input;
    knurl_status status;
    char *initial_target;
    char *initial_authority;
    char *default_authority;
    size_t i;

    if (out == NULL || target == NULL || authority == NULL || route == NULL ||
        route->target_count == 0u || sanitized == NULL || inbound == NULL ||
        path == NULL || host == NULL) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy director requires request metadata and route");
        return KNURL_INVALID;
    }
    *target = NULL;
    *authority = NULL;
    memset(out, 0, sizeof(*out));
    initial_target = NULL;
    initial_authority = NULL;
    status =
        knurl_proxy_target_build(route->targets[0], path, query,
                                 &initial_target, &initial_authority, error);
    free(initial_target);
    free(initial_authority);
    if (status != KNURL_OK)
        return status;
    out->route = route;
    out->method = method;
    out->websocket = websocket;
    for (i = 0u; i < sanitized->count; ++i) {
        if (knurl_proxy_headers_add(&out->headers, sanitized->fields[i].name,
                                    sanitized->fields[i].value) !=
            KNURL_PROXY_HEADER_OK) {
            knurl_proxy_error(error, KNURL_NOMEM,
                              "failed to copy proxy outbound headers");
            return KNURL_NOMEM;
        }
    }
    status = knurl_proxy_director_copy(&out->path, path, error);
    if (status == KNURL_OK)
        status = knurl_proxy_director_copy(&out->query, query, error);
    if (status != KNURL_OK)
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
        if (status != KNURL_OK)
            return status;
        if (out->failure != KNURL_OK) {
            knurl_proxy_error(error, out->failure,
                              "proxy rewrite attempted an invalid edit");
            return out->failure;
        }
    }
    status =
        knurl_proxy_target_build(route->targets[out->target_index], out->path,
                                 out->query, target, &default_authority, error);
    if (status != KNURL_OK)
        return status;
    if (out->websocket && out->method != KNURL_HTTP_GET) {
        free(*target);
        *target = NULL;
        free(default_authority);
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy WebSocket rewrite must retain GET");
        return KNURL_INVALID;
    }
    if (out->host != NULL) {
        free(default_authority);
        status = knurl_proxy_director_copy(authority, out->host, error);
        if (status != KNURL_OK) {
            free(*target);
            *target = NULL;
            return status;
        }
    } else {
        *authority = default_authority;
    }
    knurl_error_clear(error);
    return KNURL_OK;
}
