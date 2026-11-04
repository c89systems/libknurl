#include "proxy_ws_rejection.h"

#include "proxy_common.h"
#include "proxy_response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rotmos_proxy_ws_rejection_sink {
    rotmos_proxy_ws_rejection *rejection;
    char *output;
    size_t capacity;
    size_t written;
    int failed;
} rotmos_proxy_ws_rejection_sink;

void rotmos_proxy_ws_rejection_init(rotmos_proxy_ws_rejection *rejection)
{
    if (rejection == NULL)
        return;
    memset(rejection, 0, sizeof(*rejection));
    rotmos_proxy_http_response_init(&rejection->response, 0);
}

void rotmos_proxy_ws_rejection_cleanup(rotmos_proxy_ws_rejection *rejection)
{
    if (rejection == NULL)
        return;
    rotmos_proxy_http_response_cleanup(&rejection->response);
    rotmos_proxy_http_wire_plan_cleanup(&rejection->wire);
    memset(rejection, 0, sizeof(*rejection));
}

static rotmos_status rotmos_proxy_ws_rejection_interim(
    unsigned status, const rotmos_proxy_headers *source, char **wire,
    size_t *wire_length, rotmos_error *error)
{
    rotmos_proxy_headers headers;
    char status_line[32];
    char *out;
    size_t capacity;
    size_t used;
    size_t i;
    int count;

    rotmos_proxy_headers_init(&headers);
    if (rotmos_proxy_headers_sanitize_response(source, &headers) !=
        ROTMOS_PROXY_HEADER_OK) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid informational response headers");
        return ROTMOS_INVALID;
    }
    count =
        snprintf(status_line, sizeof(status_line), "HTTP/1.1 %u \r\n", status);
    if (count < 0 || (size_t)count >= sizeof(status_line)) {
        rotmos_proxy_headers_cleanup(&headers);
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid informational response status");
        return ROTMOS_INVALID;
    }
    capacity = (size_t)count + 2u;
    for (i = 0u; i < headers.count; ++i)
        capacity += strlen(headers.fields[i].name) +
                    strlen(headers.fields[i].value) + 4u;
    out = (char *)malloc(capacity + 1u);
    if (out == NULL) {
        rotmos_proxy_headers_cleanup(&headers);
        rotmos_proxy_error(error, ROTMOS_NOMEM,
                           "failed to allocate informational response headers");
        return ROTMOS_NOMEM;
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
    rotmos_proxy_headers_cleanup(&headers);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status
rotmos_proxy_ws_rejection_head(rotmos_proxy_ws_rejection *rejection,
                               const unsigned char *head, size_t head_length,
                               int *final, char **wire, size_t *wire_length,
                               rotmos_proxy_modify_response_fn modify,
                               void *modify_userdata, rotmos_error *error)
{
    rotmos_proxy_headers sanitized;
    rotmos_proxy_http_response downstream;
    rotmos_proxy_http_event event;
    rotmos_proxy_header_status parsed;
    const char *reason;
    size_t pos;
    size_t end;
    rotmos_status status;

    if (final != NULL)
        *final = 0;
    if (wire != NULL)
        *wire = NULL;
    if (wire_length != NULL)
        *wire_length = 0u;
    if (rejection == NULL || head == NULL || final == NULL || wire == NULL ||
        wire_length == NULL ||
        rejection->mode != ROTMOS_PROXY_WS_REJECTION_HEAD ||
        head_length < 12u || head_length > ROTMOS_PROXY_HEADER_BLOCK_LIMIT ||
        head[head_length - 2u] != '\r' || head[head_length - 1u] != '\n') {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid upstream rejection response head");
        return ROTMOS_INVALID;
    }
    pos = 0u;
    while (pos < head_length) {
        for (end = pos; end + 1u < head_length; ++end) {
            if (head[end] == '\r' && head[end + 1u] == '\n')
                break;
        }
        if (end + 1u >= head_length) {
            rotmos_proxy_error(error, ROTMOS_INVALID,
                               "unterminated upstream rejection header");
            return ROTMOS_INVALID;
        }
        parsed = rotmos_proxy_http_response_header(
            &rejection->response, (const char *)head + pos, end + 2u - pos,
            &event, &reason);
        if (parsed != ROTMOS_PROXY_HEADER_OK) {
            rotmos_proxy_error(error, ROTMOS_INVALID,
                               reason != NULL ? reason
                                              : "invalid upstream response");
            return ROTMOS_INVALID;
        }
        pos = end + 2u;
    }
    if (event == ROTMOS_PROXY_HTTP_INFORMATIONAL) {
        rotmos_proxy_headers interim;
        size_t field_start;
        size_t field_end;
        char *copy;
        char *colon;

        rotmos_proxy_headers_init(&interim);
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
                rotmos_proxy_headers_cleanup(&interim);
                rotmos_proxy_error(
                    error, ROTMOS_NOMEM,
                    "failed to copy informational response field");
                return ROTMOS_NOMEM;
            }
            memcpy(copy, head + field_start, field_end - field_start);
            copy[field_end - field_start] = '\0';
            colon = strchr(copy, ':');
            if (colon == NULL) {
                free(copy);
                rotmos_proxy_headers_cleanup(&interim);
                rotmos_proxy_error(error, ROTMOS_INVALID,
                                   "invalid informational response field");
                return ROTMOS_INVALID;
            }
            *colon++ = '\0';
            while (*colon == ' ' || *colon == '\t')
                ++colon;
            parsed = rotmos_proxy_headers_add(&interim, copy, colon);
            free(copy);
            if (parsed != ROTMOS_PROXY_HEADER_OK) {
                rotmos_proxy_headers_cleanup(&interim);
                rotmos_proxy_error(error, ROTMOS_INVALID,
                                   "invalid informational response field");
                return ROTMOS_INVALID;
            }
            field_start = field_end + 2u;
        }
        status = rotmos_proxy_ws_rejection_interim(
            (unsigned)rejection->response.status, &interim, wire, wire_length,
            error);
        rotmos_proxy_headers_cleanup(&interim);
        return status;
    }
    if (event != ROTMOS_PROXY_HTTP_FINAL) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "upstream rejection response head did not finish");
        return ROTMOS_INVALID;
    }
    rotmos_proxy_headers_init(&sanitized);
    parsed = rotmos_proxy_headers_sanitize_response(
        &rejection->response.headers, &sanitized);
    if (parsed != ROTMOS_PROXY_HEADER_OK) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid upstream rejection hop-by-hop fields");
        return ROTMOS_INVALID;
    }
    status = rotmos_proxy_response_apply(&rejection->response, &sanitized,
                                         modify, modify_userdata,
                                         &rejection->downstream_status, error);
    if (status == ROTMOS_OK) {
        downstream = rejection->response;
        downstream.status = rejection->downstream_status;
        status = rotmos_proxy_http_wire_plan_build(&downstream, &sanitized, 0,
                                                   &rejection->wire, error);
    }
    rotmos_proxy_headers_cleanup(&sanitized);
    if (status != ROTMOS_OK)
        return status;
    *wire = rejection->wire.head;
    *wire_length = rejection->wire.head_length;
    rejection->wire.head = NULL;
    if (!rejection->response.body_allowed) {
        rejection->mode = ROTMOS_PROXY_WS_REJECTION_COMPLETE;
    } else if (rejection->response.chunked) {
        rejection->mode = ROTMOS_PROXY_WS_REJECTION_CHUNKED;
        rotmos_proxy_body_framer_chunked(&rejection->framer);
    } else if (rejection->response.has_content_length) {
        rejection->mode = rejection->response.content_length == 0u
                              ? ROTMOS_PROXY_WS_REJECTION_COMPLETE
                              : ROTMOS_PROXY_WS_REJECTION_FIXED;
        rotmos_proxy_body_framer_fixed(&rejection->framer,
                                       rejection->response.content_length);
    } else {
        rejection->mode = ROTMOS_PROXY_WS_REJECTION_CLOSE;
    }
    *final = 1;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static size_t rotmos_proxy_ws_rejection_body(void *userdata,
                                             const unsigned char *data,
                                             size_t length)
{
    rotmos_proxy_ws_rejection_sink *sink;
    rotmos_error error;
    const char *reason;
    size_t accepted;

    sink = (rotmos_proxy_ws_rejection_sink *)userdata;
    if (sink->written != 0u)
        return 0u;
    accepted = length;
    if (accepted > sink->capacity - 32u)
        accepted = sink->capacity - 32u;
    if (accepted == 0u)
        return 0u;
    if (rotmos_proxy_http_response_body(&sink->rejection->response, accepted,
                                        &reason) != ROTMOS_PROXY_HEADER_OK ||
        rotmos_proxy_http_wire_chunk(&sink->rejection->wire, data, accepted,
                                     sink->output, sink->capacity,
                                     &sink->written, &error) != ROTMOS_OK) {
        sink->failed = 1;
        return 0u;
    }
    return accepted;
}

static int rotmos_proxy_ws_rejection_trailer(void *userdata, const char *name,
                                             const char *value)
{
    rotmos_proxy_ws_rejection_sink *sink;

    sink = (rotmos_proxy_ws_rejection_sink *)userdata;
    return rotmos_proxy_headers_add(&sink->rejection->response.trailers, name,
                                    value) == ROTMOS_PROXY_HEADER_OK;
}

rotmos_status rotmos_proxy_ws_rejection_feed(
    rotmos_proxy_ws_rejection *rejection, const unsigned char *input,
    size_t input_length, size_t *consumed, char *output, size_t output_capacity,
    size_t *written, rotmos_error *error)
{
    rotmos_proxy_ws_rejection_sink sink;
    rotmos_proxy_frame_result result;

    if (consumed != NULL)
        *consumed = 0u;
    if (written != NULL)
        *written = 0u;
    if (rejection == NULL || (input == NULL && input_length != 0u) ||
        consumed == NULL || output == NULL || output_capacity < 64u ||
        written == NULL || rejection->mode == ROTMOS_PROXY_WS_REJECTION_HEAD) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid upstream rejection body input");
        return ROTMOS_INVALID;
    }
    if (rejection->mode == ROTMOS_PROXY_WS_REJECTION_COMPLETE) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    memset(&sink, 0, sizeof(sink));
    sink.rejection = rejection;
    sink.output = output;
    sink.capacity = output_capacity;
    if (rejection->mode == ROTMOS_PROXY_WS_REJECTION_CLOSE) {
        size_t amount;

        amount = input_length;
        if (amount > output_capacity - 32u)
            amount = output_capacity - 32u;
        if (amount != 0u &&
            rotmos_proxy_ws_rejection_body(&sink, input, amount) != amount)
            sink.failed = 1;
        *consumed = amount;
    } else {
        result = rotmos_proxy_body_framer_feed(
            &rejection->framer, input, input_length, consumed,
            rotmos_proxy_ws_rejection_body, rotmos_proxy_ws_rejection_trailer,
            &sink);
        if (result == ROTMOS_PROXY_FRAME_INVALID)
            sink.failed = 1;
        else if (result == ROTMOS_PROXY_FRAME_COMPLETE)
            rejection->mode = ROTMOS_PROXY_WS_REJECTION_COMPLETE;
    }
    if (sink.failed) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "invalid upstream rejection body framing");
        return ROTMOS_INVALID;
    }
    *written = sink.written;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status
rotmos_proxy_ws_rejection_finish(rotmos_proxy_ws_rejection *rejection, int eof,
                                 char **wire, size_t *wire_length,
                                 rotmos_error *error)
{
    const char *reason;

    if (wire != NULL)
        *wire = NULL;
    if (wire_length != NULL)
        *wire_length = 0u;
    if (rejection == NULL || wire == NULL || wire_length == NULL ||
        (rejection->mode != ROTMOS_PROXY_WS_REJECTION_COMPLETE &&
         !(eof && rejection->mode == ROTMOS_PROXY_WS_REJECTION_CLOSE)) ||
        rotmos_proxy_http_response_finish(&rejection->response, &reason) !=
            ROTMOS_PROXY_HEADER_OK) {
        rotmos_proxy_error(
            error, ROTMOS_INVALID,
            "upstream rejection ended before body framing completed");
        return ROTMOS_INVALID;
    }
    rejection->mode = ROTMOS_PROXY_WS_REJECTION_COMPLETE;
    return rotmos_proxy_http_wire_finish(&rejection->wire,
                                         &rejection->response.trailers, wire,
                                         wire_length, error);
}
