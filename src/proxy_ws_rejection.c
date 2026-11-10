#include "proxy_ws_rejection.h"

#include "proxy_common.h"
#include "proxy_response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct knurl_proxy_ws_rejection_sink {
    knurl_proxy_ws_rejection *rejection;
    char *output;
    size_t capacity;
    size_t written;
    int failed;
} knurl_proxy_ws_rejection_sink;

void knurl_proxy_ws_rejection_init(knurl_proxy_ws_rejection *rejection)
{
    if (rejection == NULL)
        return;
    memset(rejection, 0, sizeof(*rejection));
    knurl_proxy_http_response_init(&rejection->response, 0);
}

void knurl_proxy_ws_rejection_cleanup(knurl_proxy_ws_rejection *rejection)
{
    if (rejection == NULL)
        return;
    knurl_proxy_http_response_cleanup(&rejection->response);
    knurl_proxy_http_wire_plan_cleanup(&rejection->wire);
    memset(rejection, 0, sizeof(*rejection));
}

static knurl_status
knurl_proxy_ws_rejection_interim(unsigned status,
                                 const knurl_proxy_headers *source, char **wire,
                                 size_t *wire_length, knurl_error *error)
{
    knurl_proxy_headers headers;
    char status_line[32];
    char *out;
    size_t capacity;
    size_t used;
    size_t i;
    int count;

    knurl_proxy_headers_init(&headers);
    if (knurl_proxy_headers_sanitize_response(source, &headers) !=
        KNURL_PROXY_HEADER_OK) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid informational response headers");
        return KNURL_INVALID;
    }
    count =
        snprintf(status_line, sizeof(status_line), "HTTP/1.1 %u \r\n", status);
    if (count < 0 || (size_t)count >= sizeof(status_line)) {
        knurl_proxy_headers_cleanup(&headers);
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid informational response status");
        return KNURL_INVALID;
    }
    capacity = (size_t)count + 2u;
    for (i = 0u; i < headers.count; ++i)
        capacity += strlen(headers.fields[i].name) +
                    strlen(headers.fields[i].value) + 4u;
    out = (char *)malloc(capacity + 1u);
    if (out == NULL) {
        knurl_proxy_headers_cleanup(&headers);
        knurl_proxy_error(error, KNURL_NOMEM,
                          "failed to allocate informational response headers");
        return KNURL_NOMEM;
    }
    used = (size_t)count;
    memcpy(out, status_line, used);
    for (i = 0u; i < headers.count; ++i) {
        size_t length;

        length = strlen(headers.fields[i].name);
        memcpy(out + used, headers.fields[i].name, length);
        used += length;
        memcpy(out + used, ": ", 2u);
        used += 2u;
        length = strlen(headers.fields[i].value);
        memcpy(out + used, headers.fields[i].value, length);
        used += length;
        memcpy(out + used, "\r\n", 2u);
        used += 2u;
    }
    memcpy(out + used, "\r\n", 2u);
    used += 2u;
    out[used] = '\0';
    *wire = out;
    *wire_length = used;
    knurl_proxy_headers_cleanup(&headers);
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status
knurl_proxy_ws_rejection_head(knurl_proxy_ws_rejection *rejection,
                              const unsigned char *head, size_t head_length,
                              int *final, char **wire, size_t *wire_length,
                              knurl_proxy_modify_response_fn modify,
                              void *modify_userdata, knurl_error *error)
{
    knurl_proxy_headers sanitized;
    knurl_proxy_http_response downstream;
    knurl_proxy_http_event event;
    knurl_proxy_header_status parsed;
    const char *reason;
    size_t pos;
    size_t end;
    knurl_status status;

    if (final != NULL)
        *final = 0;
    if (wire != NULL)
        *wire = NULL;
    if (wire_length != NULL)
        *wire_length = 0u;
    if (rejection == NULL || head == NULL || final == NULL || wire == NULL ||
        wire_length == NULL ||
        rejection->mode != KNURL_PROXY_WS_REJECTION_HEAD || head_length < 12u ||
        head_length > KNURL_PROXY_HEADER_BLOCK_LIMIT ||
        head[head_length - 2u] != '\r' || head[head_length - 1u] != '\n') {
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid upstream rejection response head");
        return KNURL_INVALID;
    }
    pos = 0u;
    while (pos < head_length) {
        for (end = pos; end + 1u < head_length; ++end) {
            if (head[end] == '\r' && head[end + 1u] == '\n')
                break;
        }
        if (end + 1u >= head_length) {
            knurl_proxy_error(error, KNURL_INVALID,
                              "unterminated upstream rejection header");
            return KNURL_INVALID;
        }
        parsed = knurl_proxy_http_response_header(
            &rejection->response, (const char *)head + pos, end + 2u - pos,
            &event, &reason);
        if (parsed != KNURL_PROXY_HEADER_OK) {
            knurl_proxy_error(error, KNURL_INVALID,
                              reason != NULL ? reason
                                             : "invalid upstream response");
            return KNURL_INVALID;
        }
        pos = end + 2u;
    }
    if (event == KNURL_PROXY_HTTP_INFORMATIONAL) {
        knurl_proxy_headers interim;
        size_t field_start;
        size_t field_end;
        char *copy;
        char *colon;

        knurl_proxy_headers_init(&interim);
        field_start = 0u;
        while (field_start + 1u < head_length &&
               !(head[field_start] == '\r' && head[field_start + 1u] == '\n'))
            ++field_start;
        field_start += 2u;
        while (field_start + 1u < head_length &&
               !(head[field_start] == '\r' && head[field_start + 1u] == '\n')) {
            for (field_end = field_start; field_end + 1u < head_length;
                 ++field_end) {
                if (head[field_end] == '\r' && head[field_end + 1u] == '\n')
                    break;
            }
            copy = (char *)malloc(field_end - field_start + 1u);
            if (copy == NULL) {
                knurl_proxy_headers_cleanup(&interim);
                knurl_proxy_error(
                    error, KNURL_NOMEM,
                    "failed to copy informational response field");
                return KNURL_NOMEM;
            }
            memcpy(copy, head + field_start, field_end - field_start);
            copy[field_end - field_start] = '\0';
            colon = strchr(copy, ':');
            if (colon == NULL) {
                free(copy);
                knurl_proxy_headers_cleanup(&interim);
                knurl_proxy_error(error, KNURL_INVALID,
                                  "invalid informational response field");
                return KNURL_INVALID;
            }
            *colon++ = '\0';
            while (*colon == ' ' || *colon == '\t')
                ++colon;
            parsed = knurl_proxy_headers_add(&interim, copy, colon);
            free(copy);
            if (parsed != KNURL_PROXY_HEADER_OK) {
                knurl_proxy_headers_cleanup(&interim);
                knurl_proxy_error(error, KNURL_INVALID,
                                  "invalid informational response field");
                return KNURL_INVALID;
            }
            field_start = field_end + 2u;
        }
        status = knurl_proxy_ws_rejection_interim(
            (unsigned)rejection->response.status, &interim, wire, wire_length,
            error);
        knurl_proxy_headers_cleanup(&interim);
        return status;
    }
    if (event != KNURL_PROXY_HTTP_FINAL) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "upstream rejection response head did not finish");
        return KNURL_INVALID;
    }
    knurl_proxy_headers_init(&sanitized);
    parsed = knurl_proxy_headers_sanitize_response(&rejection->response.headers,
                                                   &sanitized);
    if (parsed != KNURL_PROXY_HEADER_OK) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid upstream rejection hop-by-hop fields");
        return KNURL_INVALID;
    }
    status = knurl_proxy_response_apply(&rejection->response, &sanitized,
                                        modify, modify_userdata,
                                        &rejection->downstream_status, error);
    if (status == KNURL_OK) {
        downstream = rejection->response;
        downstream.status = rejection->downstream_status;
        status = knurl_proxy_http_wire_plan_build(&downstream, &sanitized, 0,
                                                  &rejection->wire, error);
    }
    knurl_proxy_headers_cleanup(&sanitized);
    if (status != KNURL_OK)
        return status;
    *wire = rejection->wire.head;
    *wire_length = rejection->wire.head_length;
    rejection->wire.head = NULL;
    if (!rejection->response.body_allowed) {
        rejection->mode = KNURL_PROXY_WS_REJECTION_COMPLETE;
    } else if (rejection->response.chunked) {
        rejection->mode = KNURL_PROXY_WS_REJECTION_CHUNKED;
        knurl_proxy_body_framer_chunked(&rejection->framer);
    } else if (rejection->response.has_content_length) {
        rejection->mode = rejection->response.content_length == 0u
                              ? KNURL_PROXY_WS_REJECTION_COMPLETE
                              : KNURL_PROXY_WS_REJECTION_FIXED;
        knurl_proxy_body_framer_fixed(&rejection->framer,
                                      rejection->response.content_length);
    } else {
        rejection->mode = KNURL_PROXY_WS_REJECTION_CLOSE;
    }
    *final = 1;
    knurl_error_clear(error);
    return KNURL_OK;
}

static size_t knurl_proxy_ws_rejection_body(void *userdata,
                                            const unsigned char *data,
                                            size_t length)
{
    knurl_proxy_ws_rejection_sink *sink;
    knurl_error error;
    const char *reason;
    size_t accepted;

    sink = (knurl_proxy_ws_rejection_sink *)userdata;
    if (sink->written != 0u)
        return 0u;
    accepted = length;
    if (accepted > sink->capacity - 32u)
        accepted = sink->capacity - 32u;
    if (accepted == 0u)
        return 0u;
    if (knurl_proxy_http_response_body(&sink->rejection->response, accepted,
                                       &reason) != KNURL_PROXY_HEADER_OK ||
        knurl_proxy_http_wire_chunk(&sink->rejection->wire, data, accepted,
                                    sink->output, sink->capacity,
                                    &sink->written, &error) != KNURL_OK) {
        sink->failed = 1;
        return 0u;
    }
    return accepted;
}

static int knurl_proxy_ws_rejection_trailer(void *userdata, const char *name,
                                            const char *value)
{
    knurl_proxy_ws_rejection_sink *sink;

    sink = (knurl_proxy_ws_rejection_sink *)userdata;
    return knurl_proxy_headers_add(&sink->rejection->response.trailers, name,
                                   value) == KNURL_PROXY_HEADER_OK;
}

knurl_status knurl_proxy_ws_rejection_feed(knurl_proxy_ws_rejection *rejection,
                                           const unsigned char *input,
                                           size_t input_length,
                                           size_t *consumed, char *output,
                                           size_t output_capacity,
                                           size_t *written, knurl_error *error)
{
    knurl_proxy_ws_rejection_sink sink;
    knurl_proxy_frame_result result;

    if (consumed != NULL)
        *consumed = 0u;
    if (written != NULL)
        *written = 0u;
    if (rejection == NULL || (input == NULL && input_length != 0u) ||
        consumed == NULL || output == NULL || output_capacity < 64u ||
        written == NULL || rejection->mode == KNURL_PROXY_WS_REJECTION_HEAD) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid upstream rejection body input");
        return KNURL_INVALID;
    }
    if (rejection->mode == KNURL_PROXY_WS_REJECTION_COMPLETE) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    memset(&sink, 0, sizeof(sink));
    sink.rejection = rejection;
    sink.output = output;
    sink.capacity = output_capacity;
    if (rejection->mode == KNURL_PROXY_WS_REJECTION_CLOSE) {
        size_t amount;

        amount = input_length;
        if (amount > output_capacity - 32u)
            amount = output_capacity - 32u;
        if (amount != 0u &&
            knurl_proxy_ws_rejection_body(&sink, input, amount) != amount)
            sink.failed = 1;
        *consumed = amount;
    } else {
        result = knurl_proxy_body_framer_feed(
            &rejection->framer, input, input_length, consumed,
            knurl_proxy_ws_rejection_body, knurl_proxy_ws_rejection_trailer,
            &sink);
        if (result == KNURL_PROXY_FRAME_INVALID)
            sink.failed = 1;
        else if (result == KNURL_PROXY_FRAME_COMPLETE)
            rejection->mode = KNURL_PROXY_WS_REJECTION_COMPLETE;
    }
    if (sink.failed) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid upstream rejection body framing");
        return KNURL_INVALID;
    }
    *written = sink.written;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status
knurl_proxy_ws_rejection_finish(knurl_proxy_ws_rejection *rejection, int eof,
                                char **wire, size_t *wire_length,
                                knurl_error *error)
{
    const char *reason;

    if (wire != NULL)
        *wire = NULL;
    if (wire_length != NULL)
        *wire_length = 0u;
    if (rejection == NULL || wire == NULL || wire_length == NULL ||
        (rejection->mode != KNURL_PROXY_WS_REJECTION_COMPLETE &&
         !(eof && rejection->mode == KNURL_PROXY_WS_REJECTION_CLOSE)) ||
        knurl_proxy_http_response_finish(&rejection->response, &reason) !=
            KNURL_PROXY_HEADER_OK) {
        knurl_proxy_error(
            error, KNURL_INVALID,
            "upstream rejection ended before body framing completed");
        return KNURL_INVALID;
    }
    rejection->mode = KNURL_PROXY_WS_REJECTION_COMPLETE;
    return knurl_proxy_http_wire_finish(&rejection->wire,
                                        &rejection->response.trailers, wire,
                                        wire_length, error);
}
