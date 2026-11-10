#include "proxy_headers.h"

#include "proxy_framing.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int knurl_proxy_ascii_equal(const char *a, size_t length, const char *b)
{
    size_t i;
    unsigned char left;
    unsigned char right;

    if (strlen(b) != length) {
        return 0;
    }
    for (i = 0u; i < length; ++i) {
        left = (unsigned char)a[i];
        right = (unsigned char)b[i];
        if (left >= 'A' && left <= 'Z') {
            left = (unsigned char)(left + ('a' - 'A'));
        }
        if (right >= 'A' && right <= 'Z') {
            right = (unsigned char)(right + ('a' - 'A'));
        }
        if (left != right) {
            return 0;
        }
    }
    return 1;
}

static int knurl_proxy_ascii_prefix(const char *value, const char *prefix)
{
    size_t length;

    length = strlen(prefix);
    return strlen(value) >= length &&
           knurl_proxy_ascii_equal(value, length, prefix);
}

static int knurl_proxy_token_byte(unsigned char value)
{
    if (value == 0u) {
        return 0;
    }
    if ((value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z')) {
        return 1;
    }
    return strchr("!#$%&'*+-.^_`|~", value) != NULL;
}

static void knurl_proxy_trim_ows(const char **value, size_t *length)
{
    while (*length != 0u && (**value == ' ' || **value == '\t')) {
        (*value)++;
        (*length)--;
    }
    while (*length != 0u &&
           ((*value)[*length - 1u] == ' ' || (*value)[*length - 1u] == '\t')) {
        (*length)--;
    }
}

/* Iterate a comma-separated token field. A NULL wanted token validates only.
 * Every token is checked even after a match is found. */
static int knurl_proxy_token_list(const char *value, const char *wanted,
                                  int trailers, size_t *count, int *found)
{
    const char *cursor;
    const char *start;
    size_t length;
    size_t i;

    cursor = value;
    if (*cursor == '\0') {
        return 0;
    }
    for (;;) {
        start = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            cursor++;
        }
        length = (size_t)(cursor - start);
        knurl_proxy_trim_ows(&start, &length);
        if (length == 0u || ++(*count) > KNURL_PROXY_HEADER_COUNT_LIMIT) {
            return 0;
        }
        for (i = 0u; i < length; ++i) {
            if (!knurl_proxy_token_byte((unsigned char)start[i])) {
                return 0;
            }
        }
        if (trailers && !knurl_proxy_trailer_field_allowed(start, length)) {
            return 0;
        }
        if (wanted != NULL && knurl_proxy_ascii_equal(start, length, wanted)) {
            *found = 1;
        }
        if (*cursor == '\0') {
            return 1;
        }
        cursor++;
        if (*cursor == '\0') {
            return 0;
        }
    }
}

static int knurl_proxy_has_list_token(const knurl_proxy_headers *headers,
                                      const char *field, const char *wanted,
                                      int trailers, int *found)
{
    size_t i;
    size_t count;

    *found = 0;
    count = 0u;
    for (i = 0u; i < headers->count; ++i) {
        if (knurl_proxy_ascii_equal(headers->fields[i].name,
                                    strlen(headers->fields[i].name), field) &&
            !knurl_proxy_token_list(headers->fields[i].value, wanted, trailers,
                                    &count, found)) {
            return 0;
        }
    }
    return 1;
}

static int knurl_proxy_value_equals(const char *value, const char *expected)
{
    size_t length;

    length = strlen(value);
    knurl_proxy_trim_ows(&value, &length);
    return knurl_proxy_ascii_equal(value, length, expected);
}

static int knurl_proxy_parse_length(const char *value, uint64_t *parsed)
{
    size_t length;
    size_t i;
    uint64_t n;
    unsigned digit;

    length = strlen(value);
    knurl_proxy_trim_ows(&value, &length);
    if (length == 0u) {
        return 0;
    }
    n = 0u;
    for (i = 0u; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return 0;
        }
        digit = (unsigned)(value[i] - '0');
        if (n > (((uint64_t)-1) - digit) / 10u) {
            return 0;
        }
        n = n * 10u + digit;
    }
    *parsed = n;
    return 1;
}

static int knurl_proxy_valid_host(const char *value)
{
    const char *cursor;
    const char *host_end;
    const char *port;
    size_t label_length;
    size_t host_length;
    unsigned number;
    unsigned digit;
    char address[INET6_ADDRSTRLEN];
    unsigned char ipv6[16];

    if (value == NULL || *value == '\0') {
        return 0;
    }
    port = NULL;
    if (*value == '[') {
        host_end = strchr(value, ']');
        if (host_end == NULL || host_end == value + 1 ||
            (size_t)(host_end - value - 1) >= sizeof(address)) {
            return 0;
        }
        memcpy(address, value + 1, (size_t)(host_end - value - 1));
        address[host_end - value - 1] = '\0';
        if (inet_pton(AF_INET6, address, ipv6) != 1) {
            return 0;
        }
        if (host_end[1] == ':') {
            port = host_end + 2;
        } else if (host_end[1] != '\0') {
            return 0;
        }
    } else {
        host_end = strchr(value, ':');
        if (host_end == NULL) {
            host_end = value + strlen(value);
        } else {
            port = host_end + 1;
        }
        host_length = (size_t)(host_end - value);
        if (host_length == 0u || host_length > 253u) {
            return 0;
        }
        label_length = 0u;
        for (cursor = value; cursor != host_end; ++cursor) {
            unsigned char c = (unsigned char)*cursor;
            if (c == '.') {
                if (label_length == 0u || cursor[-1] == '-') {
                    return 0;
                }
                label_length = 0u;
            } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                       c == '~') {
                if (label_length == 0u && c == '-') {
                    return 0;
                }
                if (++label_length > 63u) {
                    return 0;
                }
            } else {
                return 0;
            }
        }
        if (host_end[-1] == '-' || (host_end[-1] == '.' && host_length == 1u)) {
            return 0;
        }
    }
    if (port != NULL) {
        if (*port == '\0') {
            return 0;
        }
        number = 0u;
        for (; *port != '\0'; ++port) {
            if (*port < '0' || *port > '9') {
                return 0;
            }
            digit = (unsigned)(*port - '0');
            if (number > (65535u - digit) / 10u) {
                return 0;
            }
            number = number * 10u + digit;
        }
        if (number == 0u) {
            return 0;
        }
    }
    return 1;
}

void knurl_proxy_headers_init(knurl_proxy_headers *headers)
{
    if (headers != NULL) {
        memset(headers, 0, sizeof(*headers));
    }
}

void knurl_proxy_headers_cleanup(knurl_proxy_headers *headers)
{
    size_t i;

    if (headers == NULL) {
        return;
    }
    for (i = 0u; i < headers->count; ++i) {
        free(headers->fields[i].name);
    }
    knurl_proxy_headers_init(headers);
}

knurl_proxy_header_status knurl_proxy_headers_add(knurl_proxy_headers *headers,
                                                  const char *name,
                                                  const char *value)
{
    size_t name_length;
    size_t value_length;
    size_t i;
    char *name_copy;
    char *value_copy;

    if (headers == NULL || name == NULL || value == NULL) {
        return KNURL_PROXY_HEADER_INVALID;
    }
    name_length = strlen(name);
    value_length = strlen(value);
    if (name_length == 0u) {
        return KNURL_PROXY_HEADER_INVALID;
    }
    if (headers->count == KNURL_PROXY_HEADER_COUNT_LIMIT ||
        name_length > KNURL_PROXY_HEADER_BLOCK_LIMIT - headers->bytes ||
        value_length >
            KNURL_PROXY_HEADER_BLOCK_LIMIT - headers->bytes - name_length) {
        return KNURL_PROXY_HEADER_LIMIT;
    }
    for (i = 0u; i < name_length; ++i) {
        if (!knurl_proxy_token_byte((unsigned char)name[i])) {
            return KNURL_PROXY_HEADER_INVALID;
        }
    }
    for (i = 0u; i < value_length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if ((c < 0x20u && c != '\t') || c == 0x7fu) {
            return KNURL_PROXY_HEADER_INVALID;
        }
    }
    name_copy = (char *)malloc(name_length + value_length + 2u);
    if (name_copy == NULL) {
        return KNURL_PROXY_HEADER_NOMEM;
    }
    value_copy = name_copy + name_length + 1u;
    memcpy(name_copy, name, name_length + 1u);
    memcpy(value_copy, value, value_length + 1u);
    headers->fields[headers->count].name = name_copy;
    headers->fields[headers->count].value = value_copy;
    headers->count++;
    headers->bytes += name_length + value_length;
    return KNURL_PROXY_HEADER_OK;
}

void knurl_proxy_headers_remove(knurl_proxy_headers *headers, const char *name)
{
    char *removed[KNURL_PROXY_HEADER_COUNT_LIMIT];
    size_t removed_count;
    size_t kept;
    size_t i;

    if (headers == NULL || name == NULL)
        return;
    removed_count = 0u;
    kept = 0u;
    for (i = 0u; i < headers->count; ++i) {
        if (strcasecmp(headers->fields[i].name, name) == 0) {
            headers->bytes -= strlen(headers->fields[i].name) +
                              strlen(headers->fields[i].value);
            removed[removed_count++] = headers->fields[i].name;
        } else {
            headers->fields[kept++] = headers->fields[i];
        }
    }
    memset(&headers->fields[kept], 0,
           (headers->count - kept) * sizeof(headers->fields[0]));
    headers->count = kept;
    /* The lookup name may live in one of these allocations. */
    for (i = 0u; i < removed_count; ++i)
        free(removed[i]);
}

knurl_proxy_header_status knurl_proxy_headers_set(knurl_proxy_headers *headers,
                                                  const char *name,
                                                  const char *value)
{
    knurl_proxy_headers replacement;
    knurl_proxy_header_status status;
    size_t remaining_count;
    size_t remaining_bytes;
    size_t i;

    if (headers == NULL)
        return KNURL_PROXY_HEADER_INVALID;
    knurl_proxy_headers_init(&replacement);
    status = knurl_proxy_headers_add(&replacement, name, value);
    if (status != KNURL_PROXY_HEADER_OK)
        return status;
    remaining_count = headers->count;
    remaining_bytes = headers->bytes;
    for (i = 0u; i < headers->count; ++i) {
        if (strcasecmp(headers->fields[i].name, replacement.fields[0].name) ==
            0) {
            --remaining_count;
            remaining_bytes -= strlen(headers->fields[i].name) +
                               strlen(headers->fields[i].value);
        }
    }
    if (remaining_count == KNURL_PROXY_HEADER_COUNT_LIMIT ||
        replacement.bytes > KNURL_PROXY_HEADER_BLOCK_LIMIT - remaining_bytes) {
        knurl_proxy_headers_cleanup(&replacement);
        return KNURL_PROXY_HEADER_LIMIT;
    }
    knurl_proxy_headers_remove(headers, replacement.fields[0].name);
    /* Transfer the validated copy without allocating it again. */
    headers->fields[headers->count++] = replacement.fields[0];
    headers->bytes += replacement.bytes;
    return KNURL_PROXY_HEADER_OK;
}

knurl_proxy_header_status
knurl_proxy_request_head_parse(const knurl_proxy_headers *headers,
                               knurl_proxy_request_head *head,
                               const char **reason)
{
    size_t i;
    uint64_t length;
    int connection_upgrade;
    int upgrade_websocket;
    int found;
    int upgrade_seen;
    int expect_seen;
    int transfer_seen;
    int host_seen;

    if (reason != NULL) {
        *reason = NULL;
    }
    if (headers == NULL || head == NULL) {
        if (reason != NULL)
            *reason = "request headers are required";
        return KNURL_PROXY_HEADER_INVALID;
    }
    memset(head, 0, sizeof(*head));
    transfer_seen = 0;
    expect_seen = 0;
    upgrade_seen = 0;
    host_seen = 0;
    for (i = 0u; i < headers->count; ++i) {
        const char *name = headers->fields[i].name;
        const char *value = headers->fields[i].value;
        size_t name_length = strlen(name);
        if (knurl_proxy_ascii_equal(name, name_length, "host")) {
            if (++host_seen != 1 || !knurl_proxy_valid_host(value)) {
                if (reason != NULL)
                    *reason = "invalid or repeated Host";
                return KNURL_PROXY_HEADER_INVALID;
            }
            head->host = value;
        } else if (knurl_proxy_ascii_equal(name, name_length,
                                           "content-length")) {
            if (!knurl_proxy_parse_length(value, &length) ||
                (head->has_content_length && length != head->content_length)) {
                if (reason != NULL)
                    *reason = "invalid or conflicting Content-Length";
                return KNURL_PROXY_HEADER_INVALID;
            }
            head->has_content_length = 1;
            head->content_length = length;
        } else if (knurl_proxy_ascii_equal(name, name_length,
                                           "transfer-encoding")) {
            if (++transfer_seen != 1 ||
                !knurl_proxy_value_equals(value, "chunked")) {
                if (reason != NULL)
                    *reason = "unsupported Transfer-Encoding";
                return KNURL_PROXY_HEADER_INVALID;
            }
            head->chunked = 1;
        } else if (knurl_proxy_ascii_equal(name, name_length, "expect")) {
            if (++expect_seen != 1 ||
                !knurl_proxy_value_equals(value, "100-continue")) {
                if (reason != NULL)
                    *reason = "unsupported Expect header";
                return KNURL_PROXY_HEADER_INVALID;
            }
            head->expect_continue = 1;
        } else if (knurl_proxy_ascii_equal(name, name_length, "upgrade")) {
            if (++upgrade_seen != 1 ||
                !knurl_proxy_value_equals(value, "websocket")) {
                if (reason != NULL)
                    *reason = "unsupported Upgrade protocol";
                return KNURL_PROXY_HEADER_UNSUPPORTED_UPGRADE;
            }
        } else if (knurl_proxy_ascii_equal(name, name_length, "trailer")) {
            head->has_trailers = 1;
        }
    }
    if (host_seen != 1 || (head->chunked && head->has_content_length)) {
        if (reason != NULL)
            *reason = "missing Host or conflicting body framing";
        return KNURL_PROXY_HEADER_INVALID;
    }
    if (head->has_trailers && !head->chunked) {
        if (reason != NULL)
            *reason = "Trailer requires chunked body framing";
        return KNURL_PROXY_HEADER_INVALID;
    }
    if (head->expect_continue &&
        !(head->chunked ||
          (head->has_content_length && head->content_length != 0u))) {
        if (reason != NULL)
            *reason = "Expect requires a request body";
        return KNURL_PROXY_HEADER_INVALID;
    }
    if (!knurl_proxy_has_list_token(headers, "connection", "upgrade", 0,
                                    &connection_upgrade) ||
        !knurl_proxy_has_list_token(headers, "upgrade", "websocket", 0,
                                    &upgrade_websocket) ||
        !knurl_proxy_has_list_token(headers, "trailer", NULL, 1, &found)) {
        if (reason != NULL)
            *reason = "invalid connection, upgrade, or Trailer tokens";
        return KNURL_PROXY_HEADER_INVALID;
    }
    if (head->has_trailers &&
        knurl_proxy_has_list_token(headers, "connection", "trailer", 0,
                                   &found) &&
        found) {
        if (reason != NULL)
            *reason = "Connection may not nominate Trailer";
        return KNURL_PROXY_HEADER_INVALID;
    }
    if (upgrade_seen != 0 || connection_upgrade) {
        if (upgrade_seen != 1 || !connection_upgrade || !upgrade_websocket) {
            if (reason != NULL)
                *reason = "unsupported or incomplete Upgrade";
            return KNURL_PROXY_HEADER_UNSUPPORTED_UPGRADE;
        }
        head->websocket_upgrade = 1;
        if (head->chunked || head->has_content_length ||
            head->expect_continue) {
            if (reason != NULL)
                *reason = "WebSocket upgrade may not have a framed body";
            return KNURL_PROXY_HEADER_INVALID;
        }
    }
    if (!knurl_proxy_has_list_token(headers, "connection", "host", 0, &found) ||
        found ||
        !knurl_proxy_has_list_token(headers, "connection", "content-length", 0,
                                    &found) ||
        found ||
        !knurl_proxy_has_list_token(headers, "connection", "transfer-encoding",
                                    0, &found) ||
        found) {
        if (reason != NULL)
            *reason = "Connection nominates request framing";
        return KNURL_PROXY_HEADER_INVALID;
    }
    return KNURL_PROXY_HEADER_OK;
}

static int knurl_proxy_hop_field(const char *name)
{
    static const char *const hop[] = {"connection",
                                      "keep-alive",
                                      "proxy-connection",
                                      "te",
                                      "trailer",
                                      "transfer-encoding",
                                      "upgrade",
                                      "proxy-authenticate",
                                      "proxy-authorization",
                                      "host",
                                      "content-length",
                                      "expect",
                                      "forwarded",
                                      "x-real-ip"};
    size_t i;
    size_t length;

    length = strlen(name);
    if (knurl_proxy_ascii_prefix(name, "x-forwarded-")) {
        return 1;
    }
    for (i = 0u; i < sizeof(hop) / sizeof(hop[0]); ++i) {
        if (knurl_proxy_ascii_equal(name, length, hop[i])) {
            return 1;
        }
    }
    return 0;
}

int knurl_proxy_request_header_editable(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;
    return !knurl_proxy_hop_field(name) &&
           !knurl_proxy_ascii_prefix(name, "sec-websocket-");
}

int knurl_proxy_response_header_editable(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;
    return !knurl_proxy_hop_field(name) &&
           !knurl_proxy_ascii_prefix(name, "sec-websocket-");
}

knurl_proxy_header_status
knurl_proxy_headers_sanitize_request(const knurl_proxy_headers *source,
                                     knurl_proxy_headers *destination)
{
    knurl_proxy_request_head head;
    size_t i;
    int nominated;
    knurl_proxy_header_status status;

    if (source == NULL || destination == NULL || source == destination ||
        destination->count != 0u) {
        return KNURL_PROXY_HEADER_INVALID;
    }
    if (knurl_proxy_request_head_parse(source, &head, NULL) !=
        KNURL_PROXY_HEADER_OK) {
        return KNURL_PROXY_HEADER_INVALID;
    }
    for (i = 0u; i < source->count; ++i) {
        const char *name = source->fields[i].name;
        if (knurl_proxy_hop_field(name)) {
            continue;
        }
        if (!knurl_proxy_has_list_token(source, "connection", name, 0,
                                        &nominated)) {
            status = KNURL_PROXY_HEADER_INVALID;
            goto failed;
        }
        if (nominated) {
            continue;
        }
        status =
            knurl_proxy_headers_add(destination, name, source->fields[i].value);
        if (status != KNURL_PROXY_HEADER_OK) {
            goto failed;
        }
    }
    return KNURL_PROXY_HEADER_OK;
failed:
    knurl_proxy_headers_cleanup(destination);
    return status;
}

knurl_proxy_header_status
knurl_proxy_headers_sanitize_response(const knurl_proxy_headers *source,
                                      knurl_proxy_headers *destination)
{
    knurl_proxy_header_status status;
    const char *name;
    size_t i;
    int found;

    if (source == NULL || destination == NULL || source == destination ||
        destination->count != 0u ||
        !knurl_proxy_has_list_token(source, "connection", NULL, 0, &found)) {
        return KNURL_PROXY_HEADER_INVALID;
    }
    if (!knurl_proxy_has_list_token(source, "connection", "content-length", 0,
                                    &found) ||
        found ||
        !knurl_proxy_has_list_token(source, "connection", "transfer-encoding",
                                    0, &found) ||
        found ||
        !knurl_proxy_has_list_token(source, "connection", "trailer", 0,
                                    &found) ||
        found) {
        return KNURL_PROXY_HEADER_INVALID;
    }
    for (i = 0u; i < source->count; ++i) {
        name = source->fields[i].name;
        if (knurl_proxy_hop_field(name)) {
            continue;
        }
        if (!knurl_proxy_has_list_token(source, "connection", name, 0,
                                        &found)) {
            knurl_proxy_headers_cleanup(destination);
            return KNURL_PROXY_HEADER_INVALID;
        }
        if (found) {
            continue;
        }
        status =
            knurl_proxy_headers_add(destination, name, source->fields[i].value);
        if (status != KNURL_PROXY_HEADER_OK) {
            knurl_proxy_headers_cleanup(destination);
            return status;
        }
    }
    return KNURL_PROXY_HEADER_OK;
}

int knurl_proxy_request_trailer_declared(const knurl_proxy_headers *headers,
                                         const char *name)
{
    int found;

    if (headers == NULL || name == NULL ||
        !knurl_proxy_trailer_field_allowed(name, strlen(name))) {
        return 0;
    }
    return knurl_proxy_has_list_token(headers, "trailer", name, 1, &found) &&
           found;
}
