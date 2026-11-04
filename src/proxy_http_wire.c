#include "proxy_http_wire.h"

#include "proxy_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ROTMOS_PROXY_WIRE_MAX_HEAD_BYTES 131072u
#define ROTMOS_PROXY_WIRE_MAX_TRAILER_BYTES 65536u

static int rotmos_proxy_http_wire_append(char *buffer, size_t capacity,
                                         size_t *used, const char *data,
                                         size_t length)
{
    if (length > capacity - *used)
        return 0;
    memcpy(buffer + *used, data, length);
    *used += length;
    return 1;
}

rotmos_status rotmos_proxy_http_wire_interim_build(
    int status, const rotmos_proxy_headers *headers, char **wire,
    size_t *wire_length, rotmos_error *error)
{
    char status_line[48];
    char *buffer;
    size_t capacity;
    size_t used;
    size_t i;
    int count;

    if (wire != NULL)
        *wire = NULL;
    if (wire_length != NULL)
        *wire_length = 0u;
    if (status < 100 || status >= 200 || status == 101 || headers == NULL ||
        wire == NULL || wire_length == NULL ||
        headers->count > ROTMOS_PROXY_HEADER_COUNT_LIMIT ||
        headers->bytes > ROTMOS_PROXY_HEADER_BLOCK_LIMIT) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid proxy informational response");
        return ROTMOS_INVALID;
    }
    capacity = 48u + headers->bytes + headers->count * 4u + 2u;
    if (capacity > ROTMOS_PROXY_WIRE_MAX_HEAD_BYTES) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy informational response exceeds wire limit");
        return ROTMOS_INVALID;
    }
    buffer = (char *)malloc(capacity);
    if (buffer == NULL) {
        rotmos_proxy_error(error, ROTMOS_NOMEM,
                           "failed to allocate proxy informational response");
        return ROTMOS_NOMEM;
    }
    count =
        snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d \r\n", status);
    used = 0u;
    if (count < 0 || (size_t)count >= sizeof(status_line) ||
        !rotmos_proxy_http_wire_append(buffer, capacity, &used, status_line,
                                       (size_t)count))
        goto invalid;
    for (i = 0u; i < headers->count; ++i) {
        const char *name = headers->fields[i].name;
        const char *value = headers->fields[i].value;
        if (!rotmos_proxy_http_wire_append(buffer, capacity, &used, name,
                                           strlen(name)) ||
            !rotmos_proxy_http_wire_append(buffer, capacity, &used, ": ", 2u) ||
            !rotmos_proxy_http_wire_append(buffer, capacity, &used, value,
                                           strlen(value)) ||
            !rotmos_proxy_http_wire_append(buffer, capacity, &used, "\r\n", 2u))
            goto invalid;
    }
    if (!rotmos_proxy_http_wire_append(buffer, capacity, &used, "\r\n", 2u))
        goto invalid;
    *wire = buffer;
    *wire_length = used;
    rotmos_error_clear(error);
    return ROTMOS_OK;

invalid:
    free(buffer);
    rotmos_proxy_error(error, ROTMOS_INVALID,
                       "failed to format proxy informational response");
    return ROTMOS_INVALID;
}

rotmos_status rotmos_proxy_http_wire_plan_build(
    const rotmos_proxy_http_response *response,
    const rotmos_proxy_headers *outbound_headers, int auto_version,
    rotmos_proxy_http_wire_plan *plan, rotmos_error *error)
{
    char status_line[48];
    char length_line[64];
    size_t capacity;
    size_t used;
    size_t i;
    int count;
    int chunked;
    int body_allowed;
    int declared_trailers;

    if (response == NULL || outbound_headers == NULL || plan == NULL ||
        response->status < 200 || response->status > 599 ||
        (auto_version != 0 && auto_version != 1)) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy response writer requires final metadata");
        return ROTMOS_INVALID;
    }
    memset(plan, 0, sizeof(*plan));
    body_allowed = response->body_allowed;
    declared_trailers = 0;
    for (i = 0u; i < response->headers.count; ++i) {
        if (strcasecmp(response->headers.fields[i].name, "trailer") == 0)
            declared_trailers = 1;
    }
    chunked =
        body_allowed && (auto_version || response->chunked ||
                         !response->has_content_length || declared_trailers);
    capacity = 128u;
    for (i = 0u; i < outbound_headers->count; ++i) {
        size_t name_length = strlen(outbound_headers->fields[i].name);
        size_t value_length = strlen(outbound_headers->fields[i].value);
        if (capacity > ROTMOS_PROXY_WIRE_MAX_HEAD_BYTES - 4u ||
            name_length > ROTMOS_PROXY_WIRE_MAX_HEAD_BYTES - capacity - 4u ||
            value_length > ROTMOS_PROXY_WIRE_MAX_HEAD_BYTES - capacity - 4u -
                               name_length) {
            rotmos_proxy_error(error, ROTMOS_INVALID,
                               "proxy response headers exceed wire limit");
            return ROTMOS_INVALID;
        }
        capacity += name_length + value_length + 4u;
    }
    if (chunked) {
        for (i = 0u; i < response->headers.count; ++i) {
            size_t value_length;
            if (strcasecmp(response->headers.fields[i].name, "trailer") != 0)
                continue;
            value_length = strlen(response->headers.fields[i].value);
            if (capacity > ROTMOS_PROXY_WIRE_MAX_HEAD_BYTES - 11u ||
                value_length >
                    ROTMOS_PROXY_WIRE_MAX_HEAD_BYTES - capacity - 11u) {
                rotmos_proxy_error(
                    error, ROTMOS_INVALID,
                    "proxy Trailer declaration exceeds wire limit");
                return ROTMOS_INVALID;
            }
            capacity += value_length + 11u;
        }
    }
    plan->head = (char *)malloc(capacity);
    if (plan->head == NULL) {
        rotmos_proxy_error(error, ROTMOS_NOMEM,
                           "failed to allocate proxy response headers");
        return ROTMOS_NOMEM;
    }
    used = 0u;
    count = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d \r\n",
                     response->status);
    if (count < 0 || (size_t)count >= sizeof(status_line) ||
        !rotmos_proxy_http_wire_append(plan->head, capacity, &used, status_line,
                                       (size_t)count))
        goto invalid;
    for (i = 0u; i < outbound_headers->count; ++i) {
        const char *name = outbound_headers->fields[i].name;
        const char *value = outbound_headers->fields[i].value;
        if (!rotmos_proxy_http_wire_append(plan->head, capacity, &used, name,
                                           strlen(name)) ||
            !rotmos_proxy_http_wire_append(plan->head, capacity, &used, ": ",
                                           2u) ||
            !rotmos_proxy_http_wire_append(plan->head, capacity, &used, value,
                                           strlen(value)) ||
            !rotmos_proxy_http_wire_append(plan->head, capacity, &used, "\r\n",
                                           2u))
            goto invalid;
    }
    if (chunked) {
        for (i = 0u; i < response->headers.count; ++i) {
            const char *value;
            if (strcasecmp(response->headers.fields[i].name, "trailer") != 0)
                continue;
            value = response->headers.fields[i].value;
            if (!rotmos_proxy_http_wire_append(plan->head, capacity, &used,
                                               "Trailer: ", 9u) ||
                !rotmos_proxy_http_wire_append(plan->head, capacity, &used,
                                               value, strlen(value)) ||
                !rotmos_proxy_http_wire_append(plan->head, capacity, &used,
                                               "\r\n", 2u))
                goto invalid;
        }
    }
    if (chunked) {
        if (!rotmos_proxy_http_wire_append(plan->head, capacity, &used,
                                           "Transfer-Encoding: chunked\r\n",
                                           28u))
            goto invalid;
    } else if (response->has_content_length && response->status != 204 &&
               response->status != 205 &&
               (body_allowed || response->head_request ||
                response->status == 304)) {
        char decimal[21];
        if (!rotmos_proxy_format_length(response->content_length, decimal))
            goto invalid;
        count = snprintf(length_line, sizeof(length_line),
                         "Content-Length: %s\r\n", decimal);
        if (count < 0 || (size_t)count >= sizeof(length_line) ||
            !rotmos_proxy_http_wire_append(plan->head, capacity, &used,
                                           length_line, (size_t)count))
            goto invalid;
    }
    if (!rotmos_proxy_http_wire_append(plan->head, capacity, &used,
                                       "Connection: close\r\n\r\n", 21u))
        goto invalid;
    if (used == capacity)
        goto invalid;
    plan->head[used] = '\0';
    plan->head_length = used;
    plan->chunked = chunked;
    plan->body_allowed = body_allowed;
    rotmos_error_clear(error);
    return ROTMOS_OK;

invalid:
    rotmos_proxy_http_wire_plan_cleanup(plan);
    rotmos_proxy_error(error, ROTMOS_INVALID,
                       "failed to format bounded proxy response headers");
    return ROTMOS_INVALID;
}

void rotmos_proxy_http_wire_plan_cleanup(rotmos_proxy_http_wire_plan *plan)
{
    if (plan == NULL)
        return;
    free(plan->head);
    memset(plan, 0, sizeof(*plan));
}

rotmos_status
rotmos_proxy_http_wire_chunk(const rotmos_proxy_http_wire_plan *plan,
                             const unsigned char *body, size_t body_length,
                             char *buffer, size_t capacity, size_t *written,
                             rotmos_error *error)
{
    char prefix[32];
    size_t used;
    int count;

    if (written != NULL)
        *written = 0u;
    if (plan == NULL || !plan->body_allowed || body == NULL ||
        body_length == 0u || buffer == NULL || written == NULL) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid proxy response body chunk");
        return ROTMOS_INVALID;
    }
    used = 0u;
    if (plan->chunked) {
        count = snprintf(prefix, sizeof(prefix), "%lx\r\n",
                         (unsigned long)body_length);
        if (count < 0 || (size_t)count >= sizeof(prefix) ||
            !rotmos_proxy_http_wire_append(buffer, capacity, &used, prefix,
                                           (size_t)count))
            goto invalid_chunk;
    }
    if (!rotmos_proxy_http_wire_append(buffer, capacity, &used,
                                       (const char *)body, body_length))
        goto invalid_chunk;
    if (plan->chunked &&
        !rotmos_proxy_http_wire_append(buffer, capacity, &used, "\r\n", 2u))
        goto invalid_chunk;
    *written = used;
    rotmos_error_clear(error);
    return ROTMOS_OK;

invalid_chunk:
    rotmos_proxy_error(error, ROTMOS_INVALID,
                       "proxy response chunk exceeds bounded output buffer");
    return ROTMOS_INVALID;
}

rotmos_status rotmos_proxy_http_wire_chunk_in_place(
    const rotmos_proxy_http_wire_plan *plan, unsigned char *body,
    size_t body_length, size_t headroom, size_t tailroom,
    const unsigned char **wire, size_t *written, rotmos_error *error)
{
    char prefix[32];
    size_t prefix_length;
    int count;

    if (wire != NULL)
        *wire = NULL;
    if (written != NULL)
        *written = 0u;
    if (plan == NULL || !plan->body_allowed || body == NULL ||
        body_length == 0u || wire == NULL || written == NULL) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid in-place proxy response chunk");
        return ROTMOS_INVALID;
    }
    if (!plan->chunked) {
        *wire = body;
        *written = body_length;
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    count =
        snprintf(prefix, sizeof(prefix), "%lx\r\n", (unsigned long)body_length);
    if (count < 0 || (size_t)count >= sizeof(prefix)) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy response chunk length is invalid");
        return ROTMOS_INVALID;
    }
    prefix_length = (size_t)count;
    if (headroom < prefix_length || tailroom < 2u ||
        body_length > (size_t)-1 - prefix_length - 2u) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy response chunk exceeds framing space");
        return ROTMOS_INVALID;
    }
    memcpy(body - prefix_length, prefix, prefix_length);
    memcpy(body + body_length, "\r\n", 2u);
    *wire = body - prefix_length;
    *written = body_length + prefix_length + 2u;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status
rotmos_proxy_http_wire_finish(const rotmos_proxy_http_wire_plan *plan,
                              const rotmos_proxy_headers *trailers, char **out,
                              size_t *out_length, rotmos_error *error)
{
    size_t capacity;
    size_t used;
    size_t i;
    char *buffer;

    if (out != NULL)
        *out = NULL;
    if (out_length != NULL)
        *out_length = 0u;
    if (plan == NULL || trailers == NULL || out == NULL || out_length == NULL ||
        (!plan->chunked && trailers->count != 0u)) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy trailers require chunked downstream framing");
        return ROTMOS_INVALID;
    }
    if (!plan->chunked) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    capacity = 7u;
    for (i = 0u; i < trailers->count; ++i) {
        size_t name_length = strlen(trailers->fields[i].name);
        size_t value_length = strlen(trailers->fields[i].value);
        if (capacity > ROTMOS_PROXY_WIRE_MAX_TRAILER_BYTES - 4u ||
            name_length > ROTMOS_PROXY_WIRE_MAX_TRAILER_BYTES - capacity - 4u ||
            value_length > ROTMOS_PROXY_WIRE_MAX_TRAILER_BYTES - capacity - 4u -
                               name_length) {
            rotmos_proxy_error(error, ROTMOS_INVALID,
                               "proxy response trailers exceed wire limit");
            return ROTMOS_INVALID;
        }
        capacity += name_length + value_length + 4u;
    }
    buffer = (char *)malloc(capacity);
    if (buffer == NULL) {
        rotmos_proxy_error(error, ROTMOS_NOMEM,
                           "failed to allocate proxy response trailers");
        return ROTMOS_NOMEM;
    }
    used = 0u;
    if (!rotmos_proxy_http_wire_append(buffer, capacity, &used, "0\r\n", 3u))
        goto invalid_final;
    for (i = 0u; i < trailers->count; ++i) {
        const char *name = trailers->fields[i].name;
        const char *value = trailers->fields[i].value;
        if (!rotmos_proxy_http_wire_append(buffer, capacity, &used, name,
                                           strlen(name)) ||
            !rotmos_proxy_http_wire_append(buffer, capacity, &used, ": ", 2u) ||
            !rotmos_proxy_http_wire_append(buffer, capacity, &used, value,
                                           strlen(value)) ||
            !rotmos_proxy_http_wire_append(buffer, capacity, &used, "\r\n", 2u))
            goto invalid_final;
    }
    if (!rotmos_proxy_http_wire_append(buffer, capacity, &used, "\r\n", 2u))
        goto invalid_final;
    *out = buffer;
    *out_length = used;
    rotmos_error_clear(error);
    return ROTMOS_OK;

invalid_final:
    free(buffer);
    rotmos_proxy_error(error, ROTMOS_INVALID,
                       "failed to format bounded proxy response trailers");
    return ROTMOS_INVALID;
}
