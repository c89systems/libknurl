#include "proxy_ws_wire.h"

#include "proxy_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ROTMOS_PROXY_WS_WIRE_LIMIT 131072u

static int rotmos_proxy_ws_wire_forbidden(const char *name)
{
    static const char *const forbidden[] = {"host",
                                            "connection",
                                            "upgrade",
                                            "content-length",
                                            "transfer-encoding",
                                            "expect",
                                            "proxy-connection"};
    size_t i;

    for (i = 0u; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        if (strcasecmp(name, forbidden[i]) == 0)
            return 1;
    }
    return 0;
}

rotmos_status
rotmos_proxy_ws_wire_request(const char *request_target, const char *authority,
                             const rotmos_proxy_headers *sanitized, char **wire,
                             size_t *wire_length, rotmos_error *error)
{
    static const char prefix[] = "GET ";
    static const char middle[] = " HTTP/1.1\r\nHost: ";
    static const char upgrade[] =
        "\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n";
    size_t length;
    size_t target_length;
    size_t authority_length;
    size_t count;
    size_t i;
    size_t n;
    char *buffer;

    if (wire != NULL)
        *wire = NULL;
    if (wire_length != NULL)
        *wire_length = 0u;
    if (request_target == NULL || request_target[0] != '/' ||
        authority == NULL || authority[0] == '\0' || sanitized == NULL ||
        wire == NULL || wire_length == NULL) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid WebSocket upstream request fields");
        return ROTMOS_INVALID;
    }
    target_length = strlen(request_target);
    authority_length = strlen(authority);
    if (target_length > ROTMOS_PROXY_WS_WIRE_LIMIT ||
        authority_length > ROTMOS_PROXY_WS_WIRE_LIMIT ||
        sanitized->count > ROTMOS_PROXY_HEADER_COUNT_LIMIT) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "WebSocket upstream request head exceeds limit");
        return ROTMOS_INVALID;
    }
    for (i = 0u; i < target_length; ++i) {
        if ((unsigned char)request_target[i] <= 32u ||
            (unsigned char)request_target[i] >= 127u) {
            rotmos_proxy_error(error, ROTMOS_INVALID,
                               "WebSocket request target has an unsafe byte");
            return ROTMOS_INVALID;
        }
    }
    for (i = 0u; i < authority_length; ++i) {
        if ((unsigned char)authority[i] <= 32u ||
            (unsigned char)authority[i] >= 127u) {
            rotmos_proxy_error(error, ROTMOS_INVALID,
                               "WebSocket authority has an unsafe byte");
            return ROTMOS_INVALID;
        }
    }
    length = sizeof(prefix) - 1u + target_length + sizeof(middle) - 1u +
             authority_length + sizeof(upgrade) - 1u + 2u;
    if (length > ROTMOS_PROXY_WS_WIRE_LIMIT) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "WebSocket upstream request head exceeds limit");
        return ROTMOS_INVALID;
    }
    count = sanitized->count;
    for (i = 0u; i < count; ++i) {
        const char *name;
        const char *value;

        name = sanitized->fields[i].name;
        value = sanitized->fields[i].value;
        if (name == NULL || value == NULL ||
            rotmos_proxy_ws_wire_forbidden(name)) {
            rotmos_proxy_error(
                error, ROTMOS_INVALID,
                "WebSocket request has a forbidden outbound field");
            return ROTMOS_INVALID;
        }
        n = strlen(name) + strlen(value) + 4u;
        if (n > ROTMOS_PROXY_WS_WIRE_LIMIT - length) {
            rotmos_proxy_error(error, ROTMOS_INVALID,
                               "WebSocket upstream request head exceeds limit");
            return ROTMOS_INVALID;
        }
        length += n;
    }
    buffer = (char *)malloc(length + 1u);
    if (buffer == NULL) {
        rotmos_proxy_error(
            error, ROTMOS_NOMEM,
            "failed to allocate WebSocket upstream request head");
        return ROTMOS_NOMEM;
    }
    n = 0u;
    memcpy(buffer + n, prefix, sizeof(prefix) - 1u);
    n += sizeof(prefix) - 1u;
    memcpy(buffer + n, request_target, target_length);
    n += target_length;
    memcpy(buffer + n, middle, sizeof(middle) - 1u);
    n += sizeof(middle) - 1u;
    memcpy(buffer + n, authority, authority_length);
    n += authority_length;
    memcpy(buffer + n, upgrade, sizeof(upgrade) - 1u);
    n += sizeof(upgrade) - 1u;
    for (i = 0u; i < count; ++i) {
        size_t name_length;
        size_t value_length;

        name_length = strlen(sanitized->fields[i].name);
        value_length = strlen(sanitized->fields[i].value);
        memcpy(buffer + n, sanitized->fields[i].name, name_length);
        n += name_length;
        memcpy(buffer + n, ": ", 2u);
        n += 2u;
        memcpy(buffer + n, sanitized->fields[i].value, value_length);
        n += value_length;
        memcpy(buffer + n, "\r\n", 2u);
        n += 2u;
    }
    memcpy(buffer + n, "\r\n", 2u);
    n += 2u;
    buffer[n] = '\0';
    *wire = buffer;
    *wire_length = n;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static int rotmos_proxy_ws_wire_status(const char *line, unsigned *status)
{
    const char *number;
    const char *cursor;
    int http10;

    http10 = strncmp(line, "HTTP/1.0 ", 9u) == 0;
    if (!http10 && strncmp(line, "HTTP/1.1 ", 9u) != 0)
        return 0;
    number = line + 9u;
    if (number[0] < '1' || number[0] > '5' || number[1] < '0' ||
        number[1] > '9' || number[2] < '0' || number[2] > '9' ||
        (number[3] != '\0' && number[3] != ' '))
        return 0;
    for (cursor = number + 3u; *cursor != '\0'; ++cursor) {
        if ((unsigned char)*cursor < 32u || (unsigned char)*cursor > 126u)
            return 0;
    }
    *status = (unsigned)(number[0] - '0') * 100u +
              (unsigned)(number[1] - '0') * 10u + (unsigned)(number[2] - '0');
    return !http10 || *status != 101u;
}

rotmos_proxy_ws_head_result rotmos_proxy_ws_wire_response_head(
    const unsigned char *data, size_t length, size_t *head_length,
    unsigned *status, rotmos_proxy_headers *headers, const char **reason)
{
    rotmos_proxy_header_status field_status;
    size_t boundary;
    size_t pos;
    size_t end;
    size_t i;
    char *copy;
    char *colon;
    char *value;

    if (head_length != NULL)
        *head_length = 0u;
    if (status != NULL)
        *status = 0u;
    if (reason != NULL)
        *reason = NULL;
    if (data == NULL || head_length == NULL || status == NULL ||
        headers == NULL || headers->count != 0u) {
        if (reason != NULL)
            *reason = "invalid WebSocket response parser input";
        return ROTMOS_PROXY_WS_HEAD_INVALID;
    }
    boundary = 0u;
    for (i = 0u; i + 3u < length && i + 4u <= ROTMOS_PROXY_HEADER_BLOCK_LIMIT;
         ++i) {
        if (data[i] == '\r' && data[i + 1u] == '\n' && data[i + 2u] == '\r' &&
            data[i + 3u] == '\n') {
            boundary = i + 4u;
            break;
        }
    }
    if (boundary == 0u) {
        if (length >= ROTMOS_PROXY_HEADER_BLOCK_LIMIT) {
            if (reason != NULL)
                *reason = "WebSocket upstream response head exceeds limit";
            return ROTMOS_PROXY_WS_HEAD_LIMIT;
        }
        return ROTMOS_PROXY_WS_HEAD_MORE;
    }
    for (i = 0u; i < boundary; ++i) {
        unsigned char byte;

        byte = data[i];
        if (byte == 0u || byte == 127u ||
            (byte < 32u && byte != '\r' && byte != '\n' && byte != '\t') ||
            (byte == '\r' && (i + 1u >= boundary || data[i + 1u] != '\n')) ||
            (byte == '\n' && (i == 0u || data[i - 1u] != '\r'))) {
            if (reason != NULL)
                *reason = "WebSocket upstream response has a control byte";
            return ROTMOS_PROXY_WS_HEAD_INVALID;
        }
    }
    copy = (char *)malloc(boundary + 1u);
    if (copy == NULL)
        return ROTMOS_PROXY_WS_HEAD_NOMEM;
    memcpy(copy, data, boundary);
    copy[boundary] = '\0';
    pos = 0u;
    end = 0u;
    while (end + 1u < boundary &&
           !(copy[end] == '\r' && copy[end + 1u] == '\n'))
        ++end;
    if (end + 1u >= boundary || end < 12u) {
        if (reason != NULL)
            *reason = "invalid WebSocket upstream status line";
        goto invalid;
    }
    copy[end] = '\0';
    if (!rotmos_proxy_ws_wire_status(copy, status)) {
        if (reason != NULL)
            *reason = "invalid WebSocket upstream status line";
        goto invalid;
    }
    pos = end + 2u;
    while (pos + 1u < boundary) {
        end = pos;
        while (end + 1u < boundary &&
               !(copy[end] == '\r' && copy[end + 1u] == '\n'))
            ++end;
        if (end + 1u >= boundary) {
            if (reason != NULL)
                *reason = "invalid WebSocket upstream header line";
            goto invalid;
        }
        if (end == pos)
            break;
        copy[end] = '\0';
        colon = strchr(copy + pos, ':');
        if (colon == NULL || colon == copy + pos) {
            if (reason != NULL)
                *reason = "invalid WebSocket upstream header field";
            goto invalid;
        }
        *colon = '\0';
        value = colon + 1u;
        while (*value == ' ' || *value == '\t')
            ++value;
        for (i = strlen(value);
             i != 0u && (value[i - 1u] == ' ' || value[i - 1u] == '\t'); --i)
            value[i - 1u] = '\0';
        field_status = rotmos_proxy_headers_add(headers, copy + pos, value);
        if (field_status != ROTMOS_PROXY_HEADER_OK) {
            if (reason != NULL)
                *reason = "invalid or oversized WebSocket upstream headers";
            free(copy);
            rotmos_proxy_headers_cleanup(headers);
            return field_status == ROTMOS_PROXY_HEADER_NOMEM
                       ? ROTMOS_PROXY_WS_HEAD_NOMEM
                   : field_status == ROTMOS_PROXY_HEADER_LIMIT
                       ? ROTMOS_PROXY_WS_HEAD_LIMIT
                       : ROTMOS_PROXY_WS_HEAD_INVALID;
        }
        pos = end + 2u;
    }
    free(copy);
    *head_length = boundary;
    return ROTMOS_PROXY_WS_HEAD_COMPLETE;
invalid:
    free(copy);
    rotmos_proxy_headers_cleanup(headers);
    *status = 0u;
    return ROTMOS_PROXY_WS_HEAD_INVALID;
}

static size_t
rotmos_proxy_ws_wire_field_count(const rotmos_proxy_headers *headers,
                                 const char *name)
{
    size_t count;
    size_t i;

    count = 0u;
    for (i = 0u; i < headers->count; ++i) {
        if (strcasecmp(headers->fields[i].name, name) == 0)
            ++count;
    }
    return count;
}

rotmos_status
rotmos_proxy_ws_wire_upgrade_response(const rotmos_proxy_headers *validated,
                                      char **wire, size_t *wire_length,
                                      rotmos_error *error)
{
    static const char prefix[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n";
    static const char *const selected[] = {"Sec-WebSocket-Accept",
                                           "Sec-WebSocket-Protocol",
                                           "Sec-WebSocket-Extensions"};
    rotmos_proxy_headers sanitized;
    size_t capacity;
    size_t used;
    size_t i;
    size_t n;
    char *buffer;

    if (wire != NULL)
        *wire = NULL;
    if (wire_length != NULL)
        *wire_length = 0u;
    if (validated == NULL || wire == NULL || wire_length == NULL) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid WebSocket response writer input");
        return ROTMOS_INVALID;
    }
    rotmos_proxy_headers_init(&sanitized);
    if (rotmos_proxy_headers_sanitize_response(validated, &sanitized) !=
        ROTMOS_PROXY_HEADER_OK) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid WebSocket response hop-by-hop fields");
        return ROTMOS_INVALID;
    }
    for (i = 0u; i < sizeof(selected) / sizeof(selected[0]); ++i) {
        if (rotmos_proxy_ws_wire_field_count(validated, selected[i]) !=
            rotmos_proxy_ws_wire_field_count(&sanitized, selected[i])) {
            rotmos_proxy_headers_cleanup(&sanitized);
            rotmos_proxy_error(
                error, ROTMOS_INVALID,
                "WebSocket negotiation field was nominated hop-by-hop");
            return ROTMOS_INVALID;
        }
    }
    capacity = sizeof(prefix) + 2u;
    for (i = 0u; i < sanitized.count; ++i) {
        n = strlen(sanitized.fields[i].name) +
            strlen(sanitized.fields[i].value) + 4u;
        if (n > ROTMOS_PROXY_WS_WIRE_LIMIT - capacity) {
            rotmos_proxy_headers_cleanup(&sanitized);
            rotmos_proxy_error(error, ROTMOS_INVALID,
                               "WebSocket response head exceeds limit");
            return ROTMOS_INVALID;
        }
        capacity += n;
    }
    buffer = (char *)malloc(capacity);
    if (buffer == NULL) {
        rotmos_proxy_headers_cleanup(&sanitized);
        rotmos_proxy_error(error, ROTMOS_NOMEM,
                           "failed to allocate WebSocket response head");
        return ROTMOS_NOMEM;
    }
    used = sizeof(prefix) - 1u;
    memcpy(buffer, prefix, used);
    for (i = 0u; i < sanitized.count; ++i) {
        n = strlen(sanitized.fields[i].name);
        memcpy(buffer + used, sanitized.fields[i].name, n);
        used += n;
        memcpy(buffer + used, ": ", 2u);
        used += 2u;
        n = strlen(sanitized.fields[i].value);
        memcpy(buffer + used, sanitized.fields[i].value, n);
        used += n;
        memcpy(buffer + used, "\r\n", 2u);
        used += 2u;
    }
    memcpy(buffer + used, "\r\n", 2u);
    used += 2u;
    buffer[used] = '\0';
    *wire = buffer;
    *wire_length = used;
    rotmos_proxy_headers_cleanup(&sanitized);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
