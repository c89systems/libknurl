#ifndef ROTMOS_WEBSOCKET_H
#define ROTMOS_WEBSOCKET_H

/** @file websocket.h Public websocket API. Expected failures return
 * rotmos_status and optional bounded rotmos_error diagnostics. Take operations
 * transfer ownership only on success; views remain borrowed for their
 * documented scope.
 */

#include <rotmos/router.h>
#include <rotmos/loop.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for websocket opcode. */
typedef enum rotmos_websocket_opcode {
    /** WebSocket opcode/connection phase: continuation. */
    ROTMOS_WEBSOCKET_CONTINUATION = 0,
    /** WebSocket opcode/connection phase: text. */
    ROTMOS_WEBSOCKET_TEXT = 1,
    /** WebSocket opcode/connection phase: binary. */
    ROTMOS_WEBSOCKET_BINARY = 2,
    /** WebSocket opcode/connection phase: close. */
    ROTMOS_WEBSOCKET_CLOSE = 8,
    /** WebSocket opcode/connection phase: ping. */
    ROTMOS_WEBSOCKET_PING = 9,
    /** WebSocket opcode/connection phase: pong. */
    ROTMOS_WEBSOCKET_PONG = 10
} rotmos_websocket_opcode;

/** Values for websocket phase. */
typedef enum rotmos_websocket_phase {
    /** WebSocket opcode/connection phase: connecting. */
    ROTMOS_WEBSOCKET_CONNECTING = 0,
    /** WebSocket opcode/connection phase: open. */
    ROTMOS_WEBSOCKET_OPEN,
    /** WebSocket opcode/connection phase: closing. */
    ROTMOS_WEBSOCKET_CLOSING,
    /** WebSocket opcode/connection phase: closed. */
    ROTMOS_WEBSOCKET_CLOSED
} rotmos_websocket_phase;

/** Terminal connection accounting borrowed during on_close; peer_reason expires
 * when the callback returns.
 */
typedef struct rotmos_websocket_outcome {
    /** Portable terminal status; OK indicates successful completion. */
    rotmos_status cause;
    /** Nonzero after a successful opening handshake. */
    int opened;
    /** Nonzero for an orderly WebSocket closing handshake. */
    int clean;
    /** Locally sent WebSocket close status; zero means no status code. */
    unsigned short local_code;
    /** Peer WebSocket close status; zero means absent. */
    unsigned short peer_code;
    /** Borrowed peer close reason, valid during terminal callback. */
    rotmos_bytes peer_reason;
} rotmos_websocket_outcome;

/** Owner-loop notification after acceptance; original HTTP request is already
 * expired. Handle remains valid through on_close.
 */
typedef rotmos_status (*rotmos_websocket_open_fn)(rotmos_websocket *, void *,
                                                  rotmos_error *);
/** Owner-loop borrowed frame callback. AGAIN holds this complete frame until
 * explicit resume; payload expires when consumed.
 */
typedef rotmos_status (*rotmos_websocket_frame_fn)(rotmos_websocket *,
                                                   rotmos_websocket_opcode,
                                                   rotmos_bytes, void *,
                                                   rotmos_error *);
/** Owner-loop notification to retry an atomic frame after output capacity
 * returns.
 */
typedef void (*rotmos_websocket_writable_fn)(rotmos_websocket *, void *);
/** Final owner-loop connection callback; borrowed handle/outcome expire on
 * return. Library never frees caller userdata.
 */
typedef void (*rotmos_websocket_close_fn)(rotmos_websocket *,
                                          const rotmos_websocket_outcome *,
                                          void *);

/** Configuration for websocket. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_websocket_config {
    /** Maximum accepted WebSocket frame payload size. */
    size_t max_frame_bytes;
    /** Zero retains Kore's send behavior (SIZE_MAX); positive sets a budget. */
    size_t output_queue_bytes;
    /** WebSocket close-handshake deadline in milliseconds. */
    long close_timeout_ms;
    /** Optional selected WebSocket subprotocol, copied on acceptance. */
    const char *protocol;
    /** Ordered header pairs; repeated names preserve order. */
    const rotmos_header *headers;
    /** Number of ordered header pairs. */
    size_t header_count;
    /** Owner-loop callback after WebSocket acceptance. */
    rotmos_websocket_open_fn on_open;
    /** Owner-loop frame callback; AGAIN holds the frame until explicit resume.
     */
    rotmos_websocket_frame_fn on_frame;
    /** Owner-loop callback when bounded output capacity becomes available. */
    rotmos_websocket_writable_fn on_writable;
    /** Terminal owner-loop callback; its borrowed handle expires on return. */
    rotmos_websocket_close_fn on_close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_websocket_config;

/** Headers-time WebSocket admission using a per-request config copy. REPLY
 * rejects, MANAGED proceeds; explicitly assign connection userdata for state
 * that must outlive the HTTP request.
 */
typedef rotmos_status (*rotmos_websocket_headers_fn)(rotmos_request *,
                                                     rotmos_websocket_config *,
                                                     rotmos_request_plan *,
                                                     void *, rotmos_error *);

/** Configuration for websocket route. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct rotmos_websocket_route_config {
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    rotmos_route_path_kind path_kind;
    /** Copied WebSocket handshake/callback configuration. */
    rotmos_websocket_config websocket;
    /** Optional admission with a per-request config copy. REPLY rejects;
     * MANAGED proceeds. Set WebSocket userdata explicitly to transfer state. */
    rotmos_websocket_headers_fn on_headers;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_websocket_route_config;

/** Initialize websocket config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_websocket_config_init(rotmos_websocket_config *config);
/** Initialize websocket route config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void
rotmos_websocket_route_config_init(rotmos_websocket_route_config *config);
/** Headers dispatch only. Success copies handshake fields and owns exactly one
 * close callback. The request expires before on_open; userdata is never freed
 * by the library. Failed accept retains no callback ownership. The borrowed
 * handle remains valid until on_close returns. Operations require its owner
 * loop; application callbacks run there without recursion. */
ROTMOS_API rotmos_status
rotmos_websocket_accept(rotmos_request *, const rotmos_websocket_config *,
                        rotmos_websocket **, rotmos_error *);
/** Register an application WebSocket route. Handshake callbacks run on the
 * owner loop; connection handles outlive the HTTP request.
 */
ROTMOS_API rotmos_status
rotmos_router_websocket(rotmos_router *, const rotmos_websocket_route_config *,
                        rotmos_route **, rotmos_error *);
/** Copies one raw frame, retaining Kore's opcode/text-byte behavior. The frame
 * uses FIN; continuation is accepted for sending. The receive payload limit is
 * independent of output capacity. AGAIN queues nothing and arms notification.
 * No extensions are negotiated. */
ROTMOS_API rotmos_status rotmos_websocket_send(rotmos_websocket *,
                                               rotmos_websocket_opcode,
                                               const void *, size_t,
                                               rotmos_error *);
/** Copy one text frame; AGAIN queues nothing. See rotmos_websocket_send for
 * byte/opcode behavior.
 */
ROTMOS_API rotmos_status rotmos_websocket_send_text(rotmos_websocket *,
                                                    const char *, size_t,
                                                    rotmos_error *);
/** Copy one binary frame; AGAIN queues nothing. See rotmos_websocket_send. */
ROTMOS_API rotmos_status rotmos_websocket_send_binary(rotmos_websocket *,
                                                      const void *, size_t,
                                                      rotmos_error *);
/** AGAIN from on_frame holds the complete frame. Resume redelivers it later,
 * before subsequent frames, without recursion. */
ROTMOS_API rotmos_status rotmos_websocket_resume(rotmos_websocket *,
                                                 rotmos_error *);
/** Code zero sends an empty close; otherwise use a valid protocol/application
 * code and at most 123 UTF-8 bytes. Repeated close is harmless. */
ROTMOS_API rotmos_status rotmos_websocket_close(rotmos_websocket *,
                                                unsigned short, rotmos_bytes,
                                                rotmos_error *);
/** Inspect the connection phase while its borrowed handle is valid. */
ROTMOS_API rotmos_websocket_phase
rotmos_websocket_state(const rotmos_websocket *);
/** Borrow the connection's server; the WebSocket handle expires after on_close
 * returns.
 */
ROTMOS_API rotmos_server *rotmos_websocket_server(const rotmos_websocket *);
/** Borrow the connection's owner loop; never retain across fork/run boundaries.
 */
ROTMOS_API rotmos_loop *rotmos_websocket_loop(const rotmos_websocket *);
/** Includes each complete wire allocation until transport releases it. */
ROTMOS_API size_t rotmos_websocket_queued_bytes(const rotmos_websocket *);
/** Read connection-scoped caller userdata while the borrowed handle remains
 * valid.
 */
ROTMOS_API void *rotmos_websocket_userdata(const rotmos_websocket *);
/** Replace the caller-owned connection association; the library never frees it.
 */
ROTMOS_API rotmos_status rotmos_websocket_set_userdata(rotmos_websocket *,
                                                       void *, rotmos_error *);

#ifdef __cplusplus
}
#endif
#endif
