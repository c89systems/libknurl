#ifndef KNURL_CHANNEL_INTERNAL_H
#define KNURL_CHANNEL_INTERNAL_H

#include "request_internal.h"
#include <knurl/channel.h>
#include <knurl/server.h>

struct knurl_channel_chunk {
    struct knurl_channel_chunk *next;
    unsigned char *data;
    size_t length;
    size_t offset;
};

struct knurl_channel_backend {
    knurl_status (*read)(knurl_channel *, void *, size_t, size_t *,
                         knurl_error *);
    knurl_status (*write)(knurl_channel *, const void *, size_t, size_t *,
                          knurl_error *);
    knurl_status (*shutdown)(knurl_channel *, knurl_error *);
    void (*interest)(knurl_channel *);
    void (*pump)(knurl_channel *);
    void (*completed)(knurl_channel *);
    void (*release)(knurl_channel *, int);
};

struct knurl_channel {
    knurl_request *request;
    knurl_server *server;
    knurl_response *response;
    void (*schedule)(knurl_channel *);
    void *schedule_data;
    knurl_deadline idle;
    knurl_channel_config config;
    knurl_channel_phase phase;
    knurl_channel_phase previous_phase;
    knurl_channel_outcome outcome;
    struct knurl_channel_backend ops;
    void *backend;
    unsigned char *receive;
    size_t receive_offset;
    size_t receive_length;
    unsigned char *tail;
    size_t tail_length;
    struct knurl_channel_chunk *head;
    struct knurl_channel_chunk *last;
    size_t queued;
    size_t retained;
    unsigned int io_ready;
    unsigned int read_wait;
    unsigned int write_wait;
    unsigned int shutdown_wait;
    int input_complete;
    int read_notified;
    int write_notified;
    int eof_notified;
    int close_notified;
    int in_event;
    int shutdown_requested;
    int shutdown_notified;
    knurl_u64 last_activity;
    knurl_u64 last_write;
    knurl_u64 shutdown_started;
};

knurl_status knurl_channel_create(knurl_request *request,
                                  const knurl_channel_config *config,
                                  knurl_bytes initial, knurl_bytes backlog,
                                  knurl_channel **out, knurl_error *error);
void knurl_channel_schedule(knurl_channel *channel);
void knurl_channel_turn(knurl_channel *channel);
void knurl_channel_finish(knurl_channel *channel, knurl_status cause);
void knurl_channel_notify_closed(knurl_channel *channel);
void knurl_channel_release(knurl_channel *channel, int restore);
void knurl_channel_progress(knurl_channel *channel, size_t received,
                            size_t sent);
long knurl_channel_wait(knurl_channel *channel);
void knurl_channel_check_timeout(knurl_channel *channel);
knurl_status knurl_channel_enqueue(knurl_channel *, unsigned char *, size_t,
                                   knurl_error *);
int knurl_websocket_accept_key(knurl_request *, char *);

#endif
