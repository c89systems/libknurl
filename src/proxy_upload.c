#include "proxy_upload.h"

#include "proxy_common.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static size_t knurl_proxy_upload_accept(void *userdata,
                                        const unsigned char *data,
                                        size_t length)
{
    knurl_proxy_upload_buffer *upload;
    size_t available;

    upload = (knurl_proxy_upload_buffer *)userdata;
    if (upload->begin == upload->end) {
        upload->begin = 0u;
        upload->end = 0u;
    } else if (upload->begin != 0u && upload->end == upload->capacity) {
        memmove(upload->bytes, upload->bytes + upload->begin,
                upload->end - upload->begin);
        upload->end -= upload->begin;
        upload->begin = 0u;
    }
    available = upload->capacity - upload->end;
    if (length > available)
        length = available;
    if ((uint64_t)length > ((uint64_t)-1) - upload->body_accepted ||
        (upload->body_limit != 0u &&
         (uint64_t)length > upload->body_limit - upload->body_accepted)) {
        upload->failure = KNURL_LIMIT;
        return 0u;
    }
    if (length != 0u) {
        memcpy(upload->bytes + upload->end, data, length);
        upload->end += length;
        upload->body_accepted += (uint64_t)length;
    }
    return length;
}

static int knurl_proxy_upload_trailer(void *userdata, const char *name,
                                      const char *value)
{
    knurl_proxy_upload_buffer *upload;

    upload = (knurl_proxy_upload_buffer *)userdata;
    if (!knurl_proxy_request_trailer_declared(&upload->declared, name))
        return 0;
    return knurl_proxy_headers_add(&upload->trailers, name, value) ==
           KNURL_PROXY_HEADER_OK;
}

knurl_status knurl_proxy_upload_init(knurl_proxy_upload_buffer *upload,
                                     size_t capacity, int chunked,
                                     uint64_t content_length,
                                     const knurl_proxy_headers *request_headers,
                                     knurl_error *error)
{
    size_t i;

    if (upload == NULL || request_headers == NULL || capacity < 8192u ||
        capacity > 1048576u || (chunked != 0 && chunked != 1)) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid bounded proxy upload configuration");
        return KNURL_INVALID;
    }
    memset(upload, 0, sizeof(*upload));
    knurl_proxy_headers_init(&upload->declared);
    knurl_proxy_headers_init(&upload->trailers);
    upload->bytes = (unsigned char *)malloc(capacity);
    if (upload->bytes == NULL) {
        knurl_proxy_error(error, KNURL_NOMEM,
                          "failed to allocate bounded proxy upload chunk");
        return KNURL_NOMEM;
    }
    upload->capacity = capacity;
    if (chunked)
        knurl_proxy_body_framer_chunked(&upload->framer);
    else
        knurl_proxy_body_framer_fixed(&upload->framer, content_length);
    upload->complete = upload->framer.phase == KNURL_PROXY_PHASE_COMPLETE;
    for (i = 0u; i < request_headers->count; ++i) {
        if (strcasecmp(request_headers->fields[i].name, "trailer") != 0)
            continue;
        if (knurl_proxy_headers_add(&upload->declared, "Trailer",
                                    request_headers->fields[i].value) !=
            KNURL_PROXY_HEADER_OK) {
            knurl_proxy_upload_cleanup(upload);
            knurl_proxy_error(error, KNURL_INVALID,
                              "invalid proxy request Trailer declaration");
            return KNURL_INVALID;
        }
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_proxy_upload_cleanup(knurl_proxy_upload_buffer *upload)
{
    if (upload == NULL)
        return;
    knurl_proxy_headers_cleanup(&upload->declared);
    knurl_proxy_headers_cleanup(&upload->trailers);
    free(upload->bytes);
    memset(upload, 0, sizeof(*upload));
}

knurl_proxy_frame_result
knurl_proxy_upload_feed(knurl_proxy_upload_buffer *upload,
                        const unsigned char *data, size_t length,
                        size_t *consumed)
{
    knurl_proxy_frame_result result;

    if (upload == NULL || upload->bytes == NULL || consumed == NULL ||
        upload->fixed_reservation != 0u || (data == NULL && length != 0u)) {
        if (consumed != NULL)
            *consumed = 0u;
        return KNURL_PROXY_FRAME_INVALID;
    }
    result = knurl_proxy_body_framer_feed(&upload->framer, data, length,
                                          consumed, knurl_proxy_upload_accept,
                                          knurl_proxy_upload_trailer, upload);
    if (upload->failure != KNURL_OK) {
        upload->framer.phase = KNURL_PROXY_PHASE_INVALID;
        return KNURL_PROXY_FRAME_INVALID;
    }
    if (result == KNURL_PROXY_FRAME_COMPLETE)
        upload->complete = 1;
    return result;
}

unsigned char *
knurl_proxy_upload_reserve_fixed(knurl_proxy_upload_buffer *upload,
                                 size_t *capacity)
{
    size_t available;

    if (capacity != NULL)
        *capacity = 0u;
    if (upload == NULL || capacity == NULL || upload->bytes == NULL ||
        upload->framer.phase != KNURL_PROXY_PHASE_FIXED)
        return NULL;
    if (upload->fixed_reservation != 0u) {
        *capacity = upload->fixed_reservation;
        return upload->bytes + upload->end;
    }
    if (upload->begin == upload->end) {
        upload->begin = 0u;
        upload->end = 0u;
    } else if (upload->begin != 0u && upload->end == upload->capacity) {
        memmove(upload->bytes, upload->bytes + upload->begin,
                upload->end - upload->begin);
        upload->end -= upload->begin;
        upload->begin = 0u;
    }
    available = upload->capacity - upload->end;
    if (upload->framer.remaining < (uint64_t)available)
        available = (size_t)upload->framer.remaining;
    if (available == 0u)
        return NULL;
    *capacity = available;
    upload->fixed_reservation = available;
    return upload->bytes + upload->end;
}

knurl_proxy_frame_result
knurl_proxy_upload_commit_fixed(knurl_proxy_upload_buffer *upload,
                                size_t length)
{
    if (upload == NULL || upload->bytes == NULL ||
        upload->framer.phase != KNURL_PROXY_PHASE_FIXED || length == 0u ||
        length > upload->fixed_reservation ||
        (uint64_t)length > upload->framer.remaining)
        return KNURL_PROXY_FRAME_INVALID;
    upload->fixed_reservation = 0u;
    if ((uint64_t)length > ((uint64_t)-1) - upload->body_accepted ||
        (upload->body_limit != 0u &&
         (uint64_t)length > upload->body_limit - upload->body_accepted)) {
        upload->failure = KNURL_LIMIT;
        upload->framer.phase = KNURL_PROXY_PHASE_INVALID;
        return KNURL_PROXY_FRAME_INVALID;
    }
    upload->end += length;
    upload->body_accepted += (uint64_t)length;
    upload->framer.remaining -= (uint64_t)length;
    if (upload->framer.remaining == 0u) {
        upload->framer.phase = KNURL_PROXY_PHASE_COMPLETE;
        upload->complete = 1;
        return KNURL_PROXY_FRAME_COMPLETE;
    }
    return KNURL_PROXY_FRAME_MORE;
}

size_t knurl_proxy_upload_read(char *buffer, size_t size, size_t count,
                               void *userdata)
{
    knurl_proxy_upload_buffer *upload;
    size_t available;
    size_t capacity;

    upload = (knurl_proxy_upload_buffer *)userdata;
    if (upload == NULL || upload->failure != KNURL_OK || buffer == NULL ||
        (size != 0u && count > ((size_t)-1) / size))
        return CURL_READFUNC_ABORT;
    capacity = size * count;
    available = upload->end - upload->begin;
    if (available == 0u) {
        if (upload->complete)
            return 0u;
        upload->curl_paused = 1;
        return CURL_READFUNC_PAUSE;
    }
    if (available > capacity)
        available = capacity;
    if (available == 0u)
        return CURL_READFUNC_ABORT;
    memcpy(buffer, upload->bytes + upload->begin, available);
    upload->begin += available;
    if (upload->begin == upload->end && upload->fixed_reservation == 0u) {
        upload->begin = 0u;
        upload->end = 0u;
    }
    if (upload->on_consume != NULL)
        upload->on_consume(upload->consume_userdata, available);
    return available;
}

int knurl_proxy_upload_full(const knurl_proxy_upload_buffer *upload)
{
    return upload != NULL && upload->bytes != NULL &&
           upload->end - upload->begin == upload->capacity;
}

int knurl_proxy_upload_can_resume(const knurl_proxy_upload_buffer *upload)
{
    return upload != NULL && upload->curl_paused &&
           (upload->begin != upload->end || upload->complete);
}

void knurl_proxy_upload_resumed(knurl_proxy_upload_buffer *upload)
{
    if (upload != NULL)
        upload->curl_paused = 0;
}

int knurl_proxy_upload_curl_trailers(struct curl_slist **list, void *userdata)
{
    knurl_proxy_upload_buffer *upload;
    struct curl_slist *fields;
    struct curl_slist *next;
    char *line;
    size_t length;
    size_t i;

    if (list == NULL)
        return CURL_TRAILERFUNC_ABORT;
    *list = NULL;
    upload = (knurl_proxy_upload_buffer *)userdata;
    if (upload == NULL || !upload->complete)
        return CURL_TRAILERFUNC_ABORT;
    fields = NULL;
    for (i = 0u; i < upload->trailers.count; ++i) {
        length = strlen(upload->trailers.fields[i].name) +
                 strlen(upload->trailers.fields[i].value) + 3u;
        line = (char *)malloc(length);
        if (line == NULL)
            goto failed;
        (void)snprintf(line, length, "%s: %s", upload->trailers.fields[i].name,
                       upload->trailers.fields[i].value);
        next = curl_slist_append(fields, line);
        free(line);
        if (next == NULL)
            goto failed;
        fields = next;
    }
    *list = fields;
    return CURL_TRAILERFUNC_OK;
failed:
    curl_slist_free_all(fields);
    return CURL_TRAILERFUNC_ABORT;
}
