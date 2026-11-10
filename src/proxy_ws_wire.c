#include "proxy_ws_wire.h"

#include "proxy_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define KNURL_PROXY_WS_WIRE_LIMIT 131072u

static int knurl_proxy_ws_wire_forbidden(const char *name)
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

knurl_status knurl_proxy_ws_wire_request(const char *request_target,
                                         const char *authority,
                                         const knurl_proxy_headers *sanitized,
                                         char **wire, size_t *wire_length,
                                         knurl_error *error)
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
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid WebSocket upstream request fields");
        return KNURL_INVALID;
    }
    target_length = strlen(request_target);
    authority_length = strlen(authority);
    if (target_length > KNURL_PROXY_WS_WIRE_LIMIT ||
        authority_length > KNURL_PROXY_WS_WIRE_LIMIT ||
        sanitized->count > KNURL_PROXY_HEADER_COUNT_LIMIT) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "WebSocket upstream request head exceeds limit");
        return KNURL_INVALID;
    }
    for (i = 0u; i < target_length; ++i) {
        if ((unsigned char)request_target[i] <= 32u ||
            (unsigned char)request_target[i] >= 127u) {
            knurl_proxy_error(error, KNURL_INVALID,
                              "WebSocket request target has an unsafe byte");
            return KNURL_INVALID;
        }
    }
    for (i = 0u; i < authority_length; ++i) {
        if ((unsigned char)authority[i] <= 32u ||
            (unsigned char)authority[i] >= 127u) {
            knurl_proxy_error(error, KNURL_INVALID,
                              "WebSocket authority has an unsafe byte");
            return KNURL_INVALID;
        }
    }
    length = sizeof(prefix) - 1u + target_length + sizeof(middle) - 1u +
             authority_length + sizeof(upgrade) - 1u + 2u;
    if (length > KNURL_PROXY_WS_WIRE_LIMIT) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "WebSocket upstream request head exceeds limit");
        return KNURL_INVALID;
    }
    count = sanitized->count;
    for (i = 0u; i < count; ++i) {
        const char *name;
        const char *value;

        name = sanitized->fields[i].name;
        value = sanitized->fields[i].value;
        if (name == NULL || value == NULL ||
            knurl_proxy_ws_wire_forbidden(name)) {
            knurl_proxy_error(
                error, KNURL_INVALID,
                "WebSocket request has a forbidden outbound field");
            return KNURL_INVALID;
        }
        n = strlen(name) + strlen(value) + 4u;
        if (n > KNURL_PROXY_WS_WIRE_LIMIT - length) {
            knurl_proxy_error(error, KNURL_INVALID,
                              "WebSocket upstream request head exceeds limit");
            return KNURL_INVALID;
        }
        length += n;
    }
    buffer = (char *)malloc(length + 1u);
    if (buffer == NULL) {
        knurl_proxy_error(error, KNURL_NOMEM,
                          "failed to allocate WebSocket upstream request head");
        return KNURL_NOMEM;
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
    knurl_error_clear(error);
    return KNURL_OK;
}

static int knurl_proxy_ws_wire_status(const char *line, unsigned *status)
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

knurl_proxy_ws_head_result knurl_proxy_ws_wire_response_head(
    const unsigned char *data, size_t length, size_t *head_length,
    unsigned *status, knurl_proxy_headers *headers, const char **reason)
{
    knurl_proxy_header_status field_status;
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
        return KNURL_PROXY_WS_HEAD_INVALID;
    }
    boundary = 0u;
    for (i = 0u; i + 3u < length && i + 4u <= KNURL_PROXY_HEADER_BLOCK_LIMIT;
         ++i) {
        if (data[i] == '\r' && data[i + 1u] == '\n' && data[i + 2u] == '\r' &&
            data[i + 3u] == '\n') {
            boundary = i + 4u;
            break;
        }
    }
    if (boundary == 0u) {
        if (length >= KNURL_PROXY_HEADER_BLOCK_LIMIT) {
            if (reason != NULL)
                *reason = "WebSocket upstream response head exceeds limit";
            return KNURL_PROXY_WS_HEAD_LIMIT;
        }
        return KNURL_PROXY_WS_HEAD_MORE;
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
            return KNURL_PROXY_WS_HEAD_INVALID;
        }
    }
    copy = (char *)malloc(boundary + 1u);
    if (copy == NULL)
        return KNURL_PROXY_WS_HEAD_NOMEM;
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
    if (!knurl_proxy_ws_wire_status(copy, status)) {
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
        field_status = knurl_proxy_headers_add(headers, copy + pos, value);
        if (field_status != KNURL_PROXY_HEADER_OK) {
            if (reason != NULL)
                *reason = "invalid or oversized WebSocket upstream headers";
            free(copy);
            knurl_proxy_headers_cleanup(headers);
            return field_status == KNURL_PROXY_HEADER_NOMEM
                       ? KNURL_PROXY_WS_HEAD_NOMEM
                   : field_status == KNURL_PROXY_HEADER_LIMIT
                       ? KNURL_PROXY_WS_HEAD_LIMIT
                       : KNURL_PROXY_WS_HEAD_INVALID;
        }
        pos = end + 2u;
    }
    free(copy);
    *head_length = boundary;
    return KNURL_PROXY_WS_HEAD_COMPLETE;
invalid:
    free(copy);
    knurl_proxy_headers_cleanup(headers);
    *status = 0u;
    return KNURL_PROXY_WS_HEAD_INVALID;
}

static size_t
knurl_proxy_ws_wire_field_count(const knurl_proxy_headers *headers,
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

knurl_status
knurl_proxy_ws_wire_upgrade_response(const knurl_proxy_headers *validated,
                                     char **wire, size_t *wire_length,
                                     knurl_error *error)
{
    static const char prefix[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n";
    static const char *const selected[] = {"Sec-WebSocket-Accept",
                                           "Sec-WebSocket-Protocol",
                                           "Sec-WebSocket-Extensions"};
    knurl_proxy_headers sanitized;
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
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid WebSocket response writer input");
        return KNURL_INVALID;
    }
    knurl_proxy_headers_init(&sanitized);
    if (knurl_proxy_headers_sanitize_response(validated, &sanitized) !=
        KNURL_PROXY_HEADER_OK) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid WebSocket response hop-by-hop fields");
        return KNURL_INVALID;
    }
    for (i = 0u; i < sizeof(selected) / sizeof(selected[0]); ++i) {
        if (knurl_proxy_ws_wire_field_count(validated, selected[i]) !=
            knurl_proxy_ws_wire_field_count(&sanitized, selected[i])) {
            knurl_proxy_headers_cleanup(&sanitized);
            knurl_proxy_error(
                error, KNURL_INVALID,
                "WebSocket negotiation field was nominated hop-by-hop");
            return KNURL_INVALID;
        }
    }
    capacity = sizeof(prefix) + 2u;
    for (i = 0u; i < sanitized.count; ++i) {
        n = strlen(sanitized.fields[i].name) +
            strlen(sanitized.fields[i].value) + 4u;
        if (n > KNURL_PROXY_WS_WIRE_LIMIT - capacity) {
            knurl_proxy_headers_cleanup(&sanitized);
            knurl_proxy_error(error, KNURL_INVALID,
                              "WebSocket response head exceeds limit");
            return KNURL_INVALID;
        }
        capacity += n;
    }
    buffer = (char *)malloc(capacity);
    if (buffer == NULL) {
        knurl_proxy_headers_cleanup(&sanitized);
        knurl_proxy_error(error, KNURL_NOMEM,
                          "failed to allocate WebSocket response head");
        return KNURL_NOMEM;
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
    knurl_proxy_headers_cleanup(&sanitized);
    knurl_error_clear(error);
    return KNURL_OK;
}
