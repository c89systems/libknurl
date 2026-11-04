#ifndef ROTMOS_WEBSOCKET_H
#define ROTMOS_WEBSOCKET_H

#include <rotmos/router.h>
#include <rotmos/loop.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rotmos_websocket_opcode {
    ROTMOS_WEBSOCKET_CONTINUATION = 0,
    ROTMOS_WEBSOCKET_TEXT = 1,
    ROTMOS_WEBSOCKET_BINARY = 2,
    ROTMOS_WEBSOCKET_CLOSE = 8,
    ROTMOS_WEBSOCKET_PING = 9,
    ROTMOS_WEBSOCKET_PONG = 10
} rotmos_websocket_opcode;

typedef enum rotmos_websocket_phase {
    ROTMOS_WEBSOCKET_CONNECTING = 0,
    ROTMOS_WEBSOCKET_OPEN,
    ROTMOS_WEBSOCKET_CLOSING,
    ROTMOS_WEBSOCKET_CLOSED
} rotmos_websocket_phase;

typedef struct rotmos_websocket_outcome {
    rotmos_status cause;
    int opened;
    int clean;
    unsigned short local_code;
    unsigned short peer_code;
    rotmos_bytes peer_reason;
} rotmos_websocket_outcome;

typedef rotmos_status (*rotmos_websocket_open_fn)(rotmos_websocket *, void *,
                                                  rotmos_error *);
typedef rotmos_status (*rotmos_websocket_frame_fn)(rotmos_websocket *,
                                                   rotmos_websocket_opcode,
                                                   rotmos_bytes, void *,
                                                   rotmos_error *);
typedef void (*rotmos_websocket_writable_fn)(rotmos_websocket *, void *);
typedef void (*rotmos_websocket_close_fn)(rotmos_websocket *,
                                          const rotmos_websocket_outcome *,
                                          void *);

typedef struct rotmos_websocket_config {
    size_t max_frame_bytes;
    /* Zero retains Kore's send behavior (SIZE_MAX); positive sets a budget. */
    size_t output_queue_bytes;
    long close_timeout_ms;
    const char *protocol;
    const rotmos_header *headers;
    size_t header_count;
    rotmos_websocket_open_fn on_open;
    rotmos_websocket_frame_fn on_frame;
    rotmos_websocket_writable_fn on_writable;
    rotmos_websocket_close_fn on_close;
    void *userdata;
} rotmos_websocket_config;

typedef rotmos_status (*rotmos_websocket_headers_fn)(rotmos_request *,
                                                     rotmos_websocket_config *,
                                                     rotmos_request_plan *,
                                                     void *, rotmos_error *);

typedef struct rotmos_websocket_route_config {
    const char *path;
    rotmos_route_path_kind path_kind;
    rotmos_websocket_config websocket;
    /* Optional admission with a per-request config copy. REPLY rejects;
     * MANAGED proceeds. Set WebSocket userdata explicitly to transfer state. */
    rotmos_websocket_headers_fn on_headers;
    void *userdata;
} rotmos_websocket_route_config;

ROTMOS_API void rotmos_websocket_config_init(rotmos_websocket_config *config);
ROTMOS_API void
rotmos_websocket_route_config_init(rotmos_websocket_route_config *config);
/* Headers dispatch only. Success copies handshake fields and owns exactly one
 * close callback. The request expires before on_open; userdata is never freed
 * by the library. Failed accept retains no callback ownership. The borrowed
 * handle remains valid until on_close returns. Operations require its owner
 * loop; application callbacks run there without recursion. */
ROTMOS_API rotmos_status
rotmos_websocket_accept(rotmos_request *, const rotmos_websocket_config *,
                        rotmos_websocket **, rotmos_error *);
ROTMOS_API rotmos_status
rotmos_router_websocket(rotmos_router *, const rotmos_websocket_route_config *,
                        rotmos_route **, rotmos_error *);
/* Copies one raw frame, retaining Kore's opcode/text-byte behavior. The frame
 * uses FIN; continuation is accepted for sending. The receive payload limit is
 * independent of output capacity. AGAIN queues nothing and arms notification.
 * No extensions are negotiated. */
ROTMOS_API rotmos_status rotmos_websocket_send(rotmos_websocket *,
                                               rotmos_websocket_opcode,
                                               const void *, size_t,
                                               rotmos_error *);
ROTMOS_API rotmos_status rotmos_websocket_send_text(rotmos_websocket *,
                                                    const char *, size_t,
                                                    rotmos_error *);
ROTMOS_API rotmos_status rotmos_websocket_send_binary(rotmos_websocket *,
                                                      const void *, size_t,
                                                      rotmos_error *);
/* AGAIN from on_frame holds the complete frame. Resume redelivers it later,
 * before subsequent frames, without recursion. */
ROTMOS_API rotmos_status rotmos_websocket_resume(rotmos_websocket *,
                                                 rotmos_error *);
/* Code zero sends an empty close; otherwise use a valid protocol/application
 * code and at most 123 UTF-8 bytes. Repeated close is harmless. */
ROTMOS_API rotmos_status rotmos_websocket_close(rotmos_websocket *,
                                                unsigned short, rotmos_bytes,
                                                rotmos_error *);
ROTMOS_API rotmos_websocket_phase
rotmos_websocket_state(const rotmos_websocket *);
ROTMOS_API rotmos_server *rotmos_websocket_server(const rotmos_websocket *);
ROTMOS_API rotmos_loop *rotmos_websocket_loop(const rotmos_websocket *);
/* Includes each complete wire allocation until transport releases it. */
ROTMOS_API size_t rotmos_websocket_queued_bytes(const rotmos_websocket *);
ROTMOS_API void *rotmos_websocket_userdata(const rotmos_websocket *);
ROTMOS_API rotmos_status rotmos_websocket_set_userdata(rotmos_websocket *,
                                                       void *, rotmos_error *);

#ifdef __cplusplus
}
#endif
#endif
