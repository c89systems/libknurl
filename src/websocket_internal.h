#ifndef KNURL_WEBSOCKET_INTERNAL_H
#define KNURL_WEBSOCKET_INTERNAL_H

#include "channel_internal.h"
#include <knurl/websocket.h>

struct knurl_runtime;
struct knurl_websocket_message {
    size_t bytes;
    int kind;
    unsigned int need;
    unsigned long value, minimum;
};
int knurl_websocket_message_check(struct knurl_websocket_message *,
                                  knurl_websocket_opcode, int,
                                  const unsigned char *, size_t, size_t);
int knurl_websocket_close_check(const unsigned char *, size_t);
struct knurl_websocket_impl {
    knurl_websocket receiver;
    knurl_websocket *next;
    struct knurl_runtime *runtime;
    knurl_server *server;
    knurl_channel *channel;
    knurl_websocket_config config;
    knurl_websocket_phase phase;
    knurl_websocket_outcome outcome;
    unsigned char *payload;
    unsigned char header[14];
    size_t header_used;
    size_t header_need;
    size_t payload_used;
    size_t payload_length;
    knurl_websocket_opcode opcode;
    int final;
    struct knurl_websocket_message incoming, outgoing;
    int frame_ready;
    int paused;
    int resume_requested;
    int in_frame;
    int ready;
    int handshake_done;
    int close_started;
    int close_queued;
    int peer_closed;
    int read_failed;
    int control_pending;
    unsigned char close_payload[125];
    size_t close_length;
    unsigned char peer_payload[125];
    size_t writable_need;
    knurl_u64 close_start;
};

int knurl_websocket_utf8(const unsigned char *, size_t);
int knurl_websocket_code(unsigned short);
knurl_status knurl_websocket_queue(knurl_websocket *, knurl_websocket_opcode,
                                   const void *, size_t, knurl_error *);
void knurl_websocket_stop(knurl_websocket *, knurl_status, unsigned short);
void knurl_websocket_schedule(knurl_websocket *);
long knurl_websocket_wait(knurl_websocket *);
void knurl_websocket_step(knurl_websocket *);
void knurl_websocket_reset_frame(knurl_websocket *);

#endif
