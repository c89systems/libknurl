#include "proxy_http.h"

#include "proxy_framing.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ROTMOS_PROXY_HTTP_EXPECT_STATUS 0
#define ROTMOS_PROXY_HTTP_READING_HEADERS 1
#define ROTMOS_PROXY_HTTP_READING_BODY 2
#define ROTMOS_PROXY_HTTP_READING_TRAILERS 3
#define ROTMOS_PROXY_HTTP_TRAILERS_DONE 4

static int rotmos_proxy_http_length(const char *value, uint64_t *length)
{
    uint64_t parsed;
    unsigned digit;

    if (*value == '\0') {
        return 0;
    }
    parsed = 0u;
    for (; *value != '\0'; ++value) {
        if (*value < '0' || *value > '9') {
            return 0;
        }
        digit = (unsigned)(*value - '0');
        if (parsed > (((uint64_t)-1) - digit) / 10u) {
            return 0;
        }
        parsed = parsed * 10u + digit;
    }
    *length = parsed;
    return 1;
}

static int rotmos_proxy_http_status_line(const char *line, int *status,
                                         int *http2)
{
    const char *number;
    size_t length;

    length = strlen(line);
    if (length >= 9u && (strncasecmp(line, "HTTP/1.1 ", 9u) == 0 ||
                         strncasecmp(line, "HTTP/1.0 ", 9u) == 0)) {
        number = line + 9u;
        *http2 = 0;
    } else if (length >= 7u && strncasecmp(line, "HTTP/2 ", 7u) == 0) {
        number = line + 7u;
        *http2 = 1;
    } else {
        return 0;
    }
    length = strlen(number);
    if (length < 3u) {
        return 0;
    }
    if (number[0] < '1' || number[0] > '5' || number[1] < '0' ||
        number[1] > '9' || number[2] < '0' || number[2] > '9' ||
        (number[3] != '\0' && number[3] != ' ')) {
        return 0;
    }
    *status =
        (number[0] - '0') * 100 + (number[1] - '0') * 10 + (number[2] - '0');
    return 1;
}

static int rotmos_proxy_http_token(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int rotmos_proxy_http_trailer_declaration(const char *value)
{
    const char *cursor;
    const char *start;
    size_t length;
    size_t count;
    size_t i;

    cursor = value;
    count = 0u;
    if (*cursor == '\0') {
        return 0;
    }
    for (;;) {
        start = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            cursor++;
        }
        length = (size_t)(cursor - start);
        while (length != 0u && (*start == ' ' || *start == '\t')) {
            start++;
            length--;
        }
        while (length != 0u &&
               (start[length - 1u] == ' ' || start[length - 1u] == '\t')) {
            length--;
        }
        if (length == 0u || ++count > ROTMOS_PROXY_HEADER_COUNT_LIMIT ||
            !rotmos_proxy_trailer_field_allowed(start, length)) {
            return 0;
        }
        for (i = 0u; i < length; ++i) {
            if (!rotmos_proxy_http_token((unsigned char)start[i])) {
                return 0;
            }
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

static rotmos_proxy_header_status
rotmos_proxy_http_parse_framing(rotmos_proxy_http_response *response,
                                const char **reason)
{
    const rotmos_proxy_header *field;
    uint64_t length;
    size_t i;
    int transfer_seen;

    transfer_seen = 0;
    response->has_content_length = 0;
    response->chunked = 0;
    for (i = 0u; i < response->headers.count; ++i) {
        field = &response->headers.fields[i];
        if (strcasecmp(field->name, "content-length") == 0) {
            if (!rotmos_proxy_http_length(field->value, &length) ||
                (response->has_content_length &&
                 response->content_length != length)) {
                if (reason != NULL)
                    *reason = "invalid or conflicting upstream Content-Length";
                return ROTMOS_PROXY_HEADER_INVALID;
            }
            response->content_length = length;
            response->has_content_length = 1;
        } else if (strcasecmp(field->name, "transfer-encoding") == 0) {
            if (++transfer_seen != 1 ||
                strcasecmp(field->value, "chunked") != 0 || response->http2) {
                if (reason != NULL)
                    *reason = "unsupported upstream Transfer-Encoding";
                return ROTMOS_PROXY_HEADER_INVALID;
            }
            response->chunked = 1;
        } else if (strcasecmp(field->name, "trailer") == 0 &&
                   !rotmos_proxy_http_trailer_declaration(field->value)) {
            if (reason != NULL)
                *reason = "invalid upstream Trailer declaration";
            return ROTMOS_PROXY_HEADER_INVALID;
        }
    }
    if (response->chunked && response->has_content_length) {
        if (reason != NULL)
            *reason = "upstream response has conflicting body framing";
        return ROTMOS_PROXY_HEADER_INVALID;
    }
    if (response->status < 200 &&
        (response->has_content_length || response->chunked)) {
        if (reason != NULL)
            *reason = "upstream informational response has forbidden framing";
        return ROTMOS_PROXY_HEADER_INVALID;
    }
    return ROTMOS_PROXY_HEADER_OK;
}

void rotmos_proxy_http_response_init(rotmos_proxy_http_response *response,
                                     int head_request)
{
    if (response != NULL) {
        memset(response, 0, sizeof(*response));
        response->head_request = head_request != 0;
    }
}

void rotmos_proxy_http_response_cleanup(rotmos_proxy_http_response *response)
{
    if (response == NULL) {
        return;
    }
    rotmos_proxy_headers_cleanup(&response->headers);
    rotmos_proxy_headers_cleanup(&response->trailers);
    memset(response, 0, sizeof(*response));
}

rotmos_proxy_header_status rotmos_proxy_http_response_header(
    rotmos_proxy_http_response *response, const char *line, size_t length,
    rotmos_proxy_http_event *event, const char **reason)
{
    rotmos_proxy_header_status result;
    rotmos_proxy_headers *headers;
    const char *colon;
    const char *value;
    char *name_copy;
    char *value_copy;
    size_t name_length;
    size_t value_length;
    size_t i;

    if (event != NULL) {
        *event = ROTMOS_PROXY_HTTP_MORE;
    }
    if (reason != NULL) {
        *reason = NULL;
    }
    if (response == NULL || line == NULL || event == NULL || length < 2u ||
        length > ROTMOS_PROXY_HEADER_BLOCK_LIMIT || line[length - 2u] != '\r' ||
        line[length - 1u] != '\n' ||
        response->phase == ROTMOS_PROXY_HTTP_TRAILERS_DONE) {
        if (reason != NULL)
            *reason = "invalid upstream header line";
        return ROTMOS_PROXY_HEADER_INVALID;
    }
    for (i = 0u; i < length - 2u; ++i) {
        unsigned char c = (unsigned char)line[i];
        if (c == 0u || c == '\r' || c == '\n' || c == 0x7fu ||
            (c < 0x20u && c != '\t')) {
            if (reason != NULL)
                *reason = "upstream header line has a control byte";
            return ROTMOS_PROXY_HEADER_INVALID;
        }
    }
    if (response->phase == ROTMOS_PROXY_HTTP_EXPECT_STATUS) {
        /* Keep interim fields available to the callback until the next status
         * line; they are still bounded by the ordinary header limits. */
        rotmos_proxy_headers_cleanup(&response->headers);
        name_copy = (char *)malloc(length - 1u);
        if (name_copy == NULL) {
            return ROTMOS_PROXY_HEADER_NOMEM;
        }
        memcpy(name_copy, line, length - 2u);
        name_copy[length - 2u] = '\0';
        result = rotmos_proxy_http_status_line(name_copy, &response->status,
                                               &response->http2)
                     ? ROTMOS_PROXY_HEADER_OK
                     : ROTMOS_PROXY_HEADER_INVALID;
        free(name_copy);
        if (result != ROTMOS_PROXY_HEADER_OK) {
            if (reason != NULL)
                *reason = "invalid upstream status line";
            return result;
        }
        if (response->status == 101) {
            if (reason != NULL)
                *reason = "unsolicited upstream protocol upgrade";
            return ROTMOS_PROXY_HEADER_UNSUPPORTED_UPGRADE;
        }
        response->phase = ROTMOS_PROXY_HTTP_READING_HEADERS;
        return ROTMOS_PROXY_HEADER_OK;
    }
    if (length == 2u) {
        if (response->phase == ROTMOS_PROXY_HTTP_READING_HEADERS) {
            result = rotmos_proxy_http_parse_framing(response, reason);
            if (result != ROTMOS_PROXY_HEADER_OK) {
                return result;
            }
            if (response->status < 200) {
                if (++response->informational_count > 16u) {
                    if (reason != NULL)
                        *reason = "too many upstream informational responses";
                    return ROTMOS_PROXY_HEADER_LIMIT;
                }
                *event = ROTMOS_PROXY_HTTP_INFORMATIONAL;
                response->phase = ROTMOS_PROXY_HTTP_EXPECT_STATUS;
                return ROTMOS_PROXY_HEADER_OK;
            }
            response->body_allowed =
                !response->head_request && response->status != 204 &&
                response->status != 205 && response->status != 304;
            if (!response->body_allowed && response->status == 204 &&
                (response->has_content_length || response->chunked)) {
                if (reason != NULL)
                    *reason = "upstream 204 response has forbidden framing";
                return ROTMOS_PROXY_HEADER_INVALID;
            }
            if (response->status == 205 && response->has_content_length &&
                response->content_length != 0u) {
                if (reason != NULL)
                    *reason =
                        "upstream 205 response has nonzero Content-Length";
                return ROTMOS_PROXY_HEADER_INVALID;
            }
            response->phase = ROTMOS_PROXY_HTTP_READING_BODY;
            *event = ROTMOS_PROXY_HTTP_FINAL;
            return ROTMOS_PROXY_HEADER_OK;
        }
        if (response->phase == ROTMOS_PROXY_HTTP_READING_TRAILERS) {
            response->phase = ROTMOS_PROXY_HTTP_TRAILERS_DONE;
            *event = ROTMOS_PROXY_HTTP_TRAILERS;
            return ROTMOS_PROXY_HEADER_OK;
        }
        if (reason != NULL)
            *reason = "unexpected upstream header boundary";
        return ROTMOS_PROXY_HEADER_INVALID;
    }
    if (response->phase == ROTMOS_PROXY_HTTP_READING_BODY) {
        if (!response->http2 && !response->chunked) {
            if (reason != NULL)
                *reason = "HTTP/1.1 trailers require chunked framing";
            return ROTMOS_PROXY_HEADER_INVALID;
        }
        response->phase = ROTMOS_PROXY_HTTP_READING_TRAILERS;
    }
    if (response->phase != ROTMOS_PROXY_HTTP_READING_HEADERS &&
        response->phase != ROTMOS_PROXY_HTTP_READING_TRAILERS) {
        if (reason != NULL)
            *reason = "unexpected upstream header field";
        return ROTMOS_PROXY_HEADER_INVALID;
    }
    colon = memchr(line, ':', length - 2u);
    if (colon == NULL || colon == line) {
        if (reason != NULL)
            *reason = "invalid upstream header field";
        return ROTMOS_PROXY_HEADER_INVALID;
    }
    name_length = (size_t)(colon - line);
    value = colon + 1u;
    value_length = (size_t)((line + length - 2u) - value);
    while (value_length != 0u && (*value == ' ' || *value == '\t')) {
        value++;
        value_length--;
    }
    while (value_length != 0u && (value[value_length - 1u] == ' ' ||
                                  value[value_length - 1u] == '\t')) {
        value_length--;
    }
    name_copy = (char *)malloc(name_length + 1u);
    value_copy = (char *)malloc(value_length + 1u);
    if (name_copy == NULL || value_copy == NULL) {
        free(name_copy);
        free(value_copy);
        return ROTMOS_PROXY_HEADER_NOMEM;
    }
    memcpy(name_copy, line, name_length);
    name_copy[name_length] = '\0';
    memcpy(value_copy, value, value_length);
    value_copy[value_length] = '\0';
    if (response->phase == ROTMOS_PROXY_HTTP_READING_TRAILERS &&
        !rotmos_proxy_trailer_field_allowed(name_copy, name_length)) {
        free(name_copy);
        free(value_copy);
        if (reason != NULL)
            *reason = "forbidden upstream trailer field";
        return ROTMOS_PROXY_HEADER_INVALID;
    }
    headers = response->phase == ROTMOS_PROXY_HTTP_READING_HEADERS
                  ? &response->headers
                  : &response->trailers;
    result = rotmos_proxy_headers_add(headers, name_copy, value_copy);
    free(name_copy);
    free(value_copy);
    if (result != ROTMOS_PROXY_HEADER_OK && reason != NULL) {
        *reason = "invalid or oversized upstream header block";
    }
    return result;
}

rotmos_proxy_header_status
rotmos_proxy_http_response_body(rotmos_proxy_http_response *response,
                                size_t length, const char **reason)
{
    if (reason != NULL) {
        *reason = NULL;
    }
    if (response == NULL || response->phase != ROTMOS_PROXY_HTTP_READING_BODY ||
        (length != 0u && !response->body_allowed) ||
        (uint64_t)length > ((uint64_t)-1) - response->body_bytes ||
        (response->has_content_length &&
         (uint64_t)length > response->content_length - response->body_bytes)) {
        if (reason != NULL)
            *reason = "upstream response body violates declared framing";
        return ROTMOS_PROXY_HEADER_INVALID;
    }
    response->body_bytes += (uint64_t)length;
    return ROTMOS_PROXY_HEADER_OK;
}

rotmos_proxy_header_status
rotmos_proxy_http_response_curl_complete(rotmos_proxy_http_response *response,
                                         rotmos_proxy_http_event *event,
                                         const char **reason)
{
    if (event != NULL)
        *event = ROTMOS_PROXY_HTTP_MORE;
    if (reason != NULL)
        *reason = NULL;
    if (response == NULL || event == NULL) {
        if (reason != NULL)
            *reason = "upstream response completion requires parser and event";
        return ROTMOS_PROXY_HEADER_INVALID;
    }
    if (response->phase == ROTMOS_PROXY_HTTP_READING_TRAILERS) {
        response->phase = ROTMOS_PROXY_HTTP_TRAILERS_DONE;
        *event = ROTMOS_PROXY_HTTP_TRAILERS;
    }
    return rotmos_proxy_http_response_finish(response, reason);
}

rotmos_proxy_header_status
rotmos_proxy_http_response_finish(const rotmos_proxy_http_response *response,
                                  const char **reason)
{
    if (reason != NULL) {
        *reason = NULL;
    }
    if (response == NULL ||
        (response->phase != ROTMOS_PROXY_HTTP_READING_BODY &&
         response->phase != ROTMOS_PROXY_HTTP_TRAILERS_DONE) ||
        (response->body_allowed && response->has_content_length &&
         response->body_bytes != response->content_length)) {
        if (reason != NULL)
            *reason = "upstream response ended before body framing completed";
        return ROTMOS_PROXY_HEADER_INVALID;
    }
    return ROTMOS_PROXY_HEADER_OK;
}
