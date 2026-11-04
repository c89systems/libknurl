#ifndef ROTMOS_CHANNEL_INTERNAL_H
#define ROTMOS_CHANNEL_INTERNAL_H

#include "request_internal.h"
#include <rotmos/channel.h>

struct rotmos_channel_chunk {
    struct rotmos_channel_chunk *next;
    unsigned char *data;
    size_t length;
    size_t offset;
};

struct rotmos_channel_backend {
    rotmos_status (*read)(rotmos_channel *, void *, size_t, size_t *,
                          rotmos_error *);
    rotmos_status (*write)(rotmos_channel *, const void *, size_t, size_t *,
                           rotmos_error *);
    rotmos_status (*shutdown)(rotmos_channel *, rotmos_error *);
    void (*interest)(rotmos_channel *);
    void (*pump)(rotmos_channel *);
    void (*completed)(rotmos_channel *);
    void (*release)(rotmos_channel *, int);
};

struct rotmos_channel {
    rotmos_request *request;
    rotmos_channel_config config;
    rotmos_channel_phase phase;
    rotmos_channel_phase previous_phase;
    rotmos_channel_outcome outcome;
    struct rotmos_channel_backend ops;
    void *backend;
    unsigned char *receive;
    size_t receive_offset;
    size_t receive_length;
    unsigned char *tail;
    size_t tail_length;
    struct rotmos_channel_chunk *head;
    struct rotmos_channel_chunk *last;
    size_t queued;
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
    rotmos_u64 last_activity;
    rotmos_u64 last_write;
    rotmos_u64 shutdown_started;
};

rotmos_status rotmos_channel_create(rotmos_request *request,
                                    const rotmos_channel_config *config,
                                    rotmos_bytes initial, rotmos_bytes backlog,
                                    rotmos_channel **out, rotmos_error *error);
void rotmos_channel_schedule(rotmos_channel *channel);
void rotmos_channel_turn(rotmos_channel *channel);
void rotmos_channel_finish(rotmos_channel *channel, rotmos_status cause);
void rotmos_channel_notify_closed(rotmos_channel *channel);
void rotmos_channel_release(rotmos_channel *channel, int restore);
void rotmos_channel_progress(rotmos_channel *channel, size_t received,
                             size_t sent);
long rotmos_channel_wait(rotmos_channel *channel);
void rotmos_channel_check_timeout(rotmos_channel *channel);

#endif
