#ifndef ROTMOS_RESPONSE_INTERNAL_H
#define ROTMOS_RESPONSE_INTERNAL_H

#include "request_internal.h"
#include "reply_internal.h"
#include <rotmos/response.h>

struct rotmos_response_segment {
    struct rotmos_response_segment *next;
    unsigned char *data;
    size_t length;
    size_t body_start;
    size_t body_length;
};

struct rotmos_response {
    rotmos_request *request;
    rotmos_reply *headers;
    rotmos_response_phase phase;
    rotmos_response_outcome outcome;
    rotmos_u64 length;
    rotmos_u64 accepted;
    rotmos_u64 started;
    rotmos_u64 file_offset;
    rotmos_u64 file_remaining;
    int has_length;
    int suppressed;
    int chunked;
    int close_connection;
    int trailers_enabled;
    char *trailer_names;
    unsigned char *ending;
    size_t ending_length;
    struct rotmos_response_segment *head;
    struct rotmos_response_segment *tail;
    size_t queued;
    size_t bound;
    size_t chunk;
    size_t header_bound;
    rotmos_source *source;
    rotmos_source *retired;
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
    int terminal_notified;
    rotmos_response_writable_fn writable;
    rotmos_response_close_fn closed;
    void *userdata;
    void *backend;
    void (*schedule)(rotmos_response *);
};

rotmos_status rotmos_response_create(rotmos_request *request,
                                     void (*schedule)(rotmos_response *),
                                     rotmos_response **out,
                                     rotmos_error *error);
rotmos_status rotmos_response_submit(rotmos_response *response,
                                     rotmos_reply *reply, rotmos_error *error);
void rotmos_response_step(rotmos_response *response);
void rotmos_response_fail(rotmos_response *response, rotmos_status cause);
void rotmos_response_error(rotmos_response *response, rotmos_status cause,
                           int status);
void rotmos_response_sent(rotmos_response *response, size_t offset,
                          size_t amount, int retry);
void rotmos_response_shift(rotmos_response *response);
void rotmos_response_terminal(rotmos_response *response);
void rotmos_response_release(rotmos_response *response);

#endif
