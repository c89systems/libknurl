#ifndef ROTMOS_WEBSOCKET_INTERNAL_H
#define ROTMOS_WEBSOCKET_INTERNAL_H

#include "channel_internal.h"
#include <rotmos/websocket.h>

struct rotmos_runtime;
struct rotmos_websocket {
    rotmos_websocket *next;
    struct rotmos_runtime *runtime;
    rotmos_server *server;
    rotmos_channel *channel;
    rotmos_websocket_config config;
    rotmos_websocket_phase phase;
    rotmos_websocket_outcome outcome;
    unsigned char *payload;
    unsigned char header[14];
    size_t header_used;
    size_t header_need;
    size_t payload_used;
    size_t payload_length;
    rotmos_websocket_opcode opcode;
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
    rotmos_u64 close_start;
};

int rotmos_websocket_utf8(const unsigned char *, size_t);
int rotmos_websocket_code(unsigned short);
rotmos_status rotmos_websocket_queue(rotmos_websocket *,
                                     rotmos_websocket_opcode, const void *,
                                     size_t, rotmos_error *);
void rotmos_websocket_stop(rotmos_websocket *, rotmos_status, unsigned short);
void rotmos_websocket_schedule(rotmos_websocket *);
long rotmos_websocket_wait(rotmos_websocket *);
void rotmos_websocket_step(rotmos_websocket *);
void rotmos_websocket_reset_frame(rotmos_websocket *);

#endif
