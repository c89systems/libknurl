#ifndef KNURL_RESPONSE_INTERNAL_H
#define KNURL_RESPONSE_INTERNAL_H

#include "request_internal.h"
#include "reply_internal.h"
#include <knurl/response.h>

struct knurl_response_segment {
    struct knurl_response_segment *next;
    unsigned char *data;
    size_t length;
    size_t body_start;
    size_t body_length;
    int interim;
};

struct knurl_response {
    knurl_request *request;
    knurl_reply *headers;
    knurl_response_phase phase;
    knurl_response_outcome outcome;
    knurl_u64 length;
    knurl_u64 accepted;
    knurl_u64 started;
    knurl_u64 file_offset;
    knurl_u64 file_remaining;
    int has_length;
    int explicit_length_header;
    int suppressed;
    int chunked;
    int close_connection;
    int trailers_enabled;
    char *trailer_names;
    unsigned char *ending;
    size_t ending_length;
    unsigned char *pending_head;
    size_t pending_head_length;
    size_t pending_head_position;
    struct knurl_response_segment *head;
    struct knurl_response_segment *tail;
    size_t queued;
    size_t bound;
    size_t chunk;
    size_t header_bound;
    size_t interim_bytes;
    unsigned int interim_count;
    void (*progress)(void *, size_t);
    void *progress_data;
    knurl_source *source;
    knurl_source *retired;
    unsigned char *memory;
    size_t memory_length;
    size_t memory_position;
    int file;
    int producer;
    int complete_reply;
    int paused;
    int eof;
    int busy;
    int inflight;
    int blocked;
    size_t writable_need;
    int terminal_notified;
    knurl_response_writable_fn writable;
    knurl_response_close_fn closed;
    void *userdata;
    void *backend;
    void (*schedule)(knurl_response *);
};

knurl_status knurl_response_create(knurl_request *request,
                                   void (*schedule)(knurl_response *),
                                   knurl_response **out, knurl_error *error);
knurl_status knurl_response_submit(knurl_response *response, knurl_reply *reply,
                                   knurl_error *error);
void knurl_response_step(knurl_response *response);
void knurl_response_fail(knurl_response *response, knurl_status cause);
void knurl_response_error(knurl_response *response, knurl_status cause,
                          int status);
void knurl_response_error_message(knurl_response *, knurl_status, int,
                                  const char *);
void knurl_response_sent(knurl_response *response, size_t offset, size_t amount,
                         int retry);
void knurl_response_shift(knurl_response *response);
void knurl_response_terminal(knurl_response *response);
void knurl_response_release(knurl_response *response);
knurl_status knurl_response_head(knurl_response *, knurl_owned_bytes *,
                                 knurl_error *);
knurl_status knurl_response_pretty(int, knurl_owned_bytes *, knurl_error *);
int knurl_response_file_metadata(knurl_response *, time_t, char *, size_t);
knurl_status knurl_response_upgrade_headers(knurl_response *response,
                                            const knurl_header *headers,
                                            size_t count, knurl_error *error);

#endif
