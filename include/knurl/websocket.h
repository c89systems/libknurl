#ifndef KNURL_WEBSOCKET_H
#define KNURL_WEBSOCKET_H

/** @file websocket.h Public websocket API. Expected failures return
 * knurl_status and optional bounded knurl_error diagnostics. Take operations
 * transfer ownership only on success; views remain borrowed for their
 * documented scope.
 */

#include <knurl/router.h>
#include <knurl/loop.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for websocket opcode. */
typedef enum knurl_websocket_opcode {
    /** WebSocket opcode/connection phase: continuation. */
    KNURL_WEBSOCKET_CONTINUATION = 0,
    /** WebSocket opcode/connection phase: text. */
    KNURL_WEBSOCKET_TEXT = 1,
    /** WebSocket opcode/connection phase: binary. */
    KNURL_WEBSOCKET_BINARY = 2,
    /** WebSocket opcode/connection phase: close. */
    KNURL_WEBSOCKET_CLOSE = 8,
    /** WebSocket opcode/connection phase: ping. */
    KNURL_WEBSOCKET_PING = 9,
    /** WebSocket opcode/connection phase: pong. */
    KNURL_WEBSOCKET_PONG = 10
} knurl_websocket_opcode;

/** Values for websocket phase. */
typedef enum knurl_websocket_phase {
    /** WebSocket opcode/connection phase: connecting. */
    KNURL_WEBSOCKET_CONNECTING = 0,
    /** WebSocket opcode/connection phase: open. */
    KNURL_WEBSOCKET_OPEN,
    /** WebSocket opcode/connection phase: closing. */
    KNURL_WEBSOCKET_CLOSING,
    /** WebSocket opcode/connection phase: closed. */
    KNURL_WEBSOCKET_CLOSED
} knurl_websocket_phase;

/** Terminal connection accounting borrowed during on_close; peer_reason expires
 * when the callback returns.
 */
typedef struct knurl_websocket_outcome {
    /** Portable terminal status; OK indicates successful completion. */
    knurl_status cause;
    /** Nonzero after a successful opening handshake. */
    int opened;
    /** Nonzero for an orderly WebSocket closing handshake. */
    int clean;
    /** Locally sent WebSocket close status; zero means no status code. */
    unsigned short local_code;
    /** Peer close status; 1005 represents an empty close, 1006 a missing close.
     * Neither reserved value is transmitted on the wire. */
    unsigned short peer_code;
    /** Borrowed peer close reason, valid during terminal callback. */
    knurl_bytes peer_reason;
} knurl_websocket_outcome;

/** Owner-loop notification after acceptance; original HTTP request is already
 * expired. Handle remains valid through on_close.
 */
typedef knurl_status (*knurl_websocket_open_fn)(knurl_websocket *, void *,
                                                knurl_error *);
/** Owner-loop borrowed frame callback. AGAIN holds this complete frame until
 * explicit resume; payload expires when consumed.
 */
typedef knurl_status (*knurl_websocket_frame_fn)(knurl_websocket *,
                                                 knurl_websocket_opcode,
                                                 int final, knurl_bytes, void *,
                                                 knurl_error *);
/** Owner-loop notification to retry an atomic frame after output capacity
 * returns.
 */
typedef void (*knurl_websocket_writable_fn)(knurl_websocket *, void *);
/** Final owner-loop connection callback; borrowed handle/outcome expire on
 * return. Library never frees caller userdata.
 */
typedef void (*knurl_websocket_close_fn)(knurl_websocket *,
                                         const knurl_websocket_outcome *,
                                         void *);

/** Configuration for websocket. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_websocket_config {
    /** Maximum accepted WebSocket frame payload size. */
    size_t max_frame_bytes;
    /** Maximum message size across all fragments; zero inherits frame limit. */
    size_t max_message_bytes;
    /** Zero selects max_frame_bytes + 14, with a 127-byte control minimum. */
    size_t output_queue_bytes;
    /** WebSocket close-handshake deadline in milliseconds. */
    long close_timeout_ms;
    /** Optional selected WebSocket subprotocol, copied on acceptance. */
    const char *protocol;
    /** Ordered header pairs; repeated names preserve order. */
    const knurl_header *headers;
    /** Number of ordered header pairs. */
    size_t header_count;
    /** Owner-loop callback after WebSocket acceptance. */
    knurl_websocket_open_fn on_open;
    /** Owner-loop frame callback; AGAIN holds the frame until explicit resume.
     */
    knurl_websocket_frame_fn on_frame;
    /** Owner-loop callback when bounded output capacity becomes available. */
    knurl_websocket_writable_fn on_writable;
    /** Terminal owner-loop callback; its borrowed handle expires on return. */
    knurl_websocket_close_fn on_close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_websocket_config;

/** Headers-time WebSocket admission using a per-request config copy. REPLY
 * rejects, MANAGED proceeds; explicitly assign connection userdata for state
 * that must outlive the HTTP request.
 */
typedef knurl_status (*knurl_websocket_headers_fn)(knurl_request *,
                                                   knurl_websocket_config *,
                                                   knurl_request_plan *, void *,
                                                   knurl_error *);

/** Configuration for websocket route. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_websocket_route_config {
    /** Optional retained route ACL, checked before handshake callbacks. */
    knurl_acl *acl;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    knurl_route_path_kind path_kind;
    /** Copied WebSocket handshake/callback configuration. */
    knurl_websocket_config websocket;
    /** Optional admission with a per-request config copy. REPLY rejects;
     * MANAGED proceeds. Set WebSocket userdata explicitly to transfer state. */
    knurl_websocket_headers_fn on_headers;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_websocket_route_config;

/** Initialize websocket config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_websocket_config_init(knurl_websocket_config *config);
/** Initialize websocket route config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void
knurl_websocket_route_config_init(knurl_websocket_route_config *config);
/** Headers dispatch only. Success copies handshake fields and owns exactly one
 * close callback. The request expires before on_open; userdata is never freed
 * by the library. Failed accept retains no callback ownership. The borrowed
 * handle remains valid until on_close returns. Operations require its owner
 * loop; application callbacks run there without recursion. */
KNURL_API knurl_status knurl_websocket_accept(knurl_request *,
                                              const knurl_websocket_config *,
                                              knurl_websocket **,
                                              knurl_error *);
/** Register an application WebSocket route. Handshake callbacks run on the
 * owner loop; connection handles outlive the HTTP request.
 */
KNURL_API knurl_status
knurl_router_websocket(knurl_router *, const knurl_websocket_route_config *,
                       knurl_route **, knurl_error *);
/** Copy one complete frame. Sequencing, UTF-8 and controls are validated.
 * AGAIN queues nothing, changes no message state and arms notification. */
KNURL_API knurl_status knurl_websocket_send(knurl_websocket *,
                                            knurl_websocket_opcode,
                                            const void *, size_t,
                                            knurl_error *);
/** Copy one bounded frame, with explicit FIN. Continuations require an active
 * fragmented message. Message state changes only on queue acceptance. */
KNURL_API knurl_status knurl_websocket_send_frame(knurl_websocket *,
                                                  knurl_websocket_opcode,
                                                  int final, const void *,
                                                  size_t, knurl_error *);
/** Copy one text frame; AGAIN queues nothing. See knurl_websocket_send for
 * byte/opcode behavior.
 */
KNURL_API knurl_status knurl_websocket_send_text(knurl_websocket *,
                                                 const char *, size_t,
                                                 knurl_error *);
/** Copy one binary frame; AGAIN queues nothing. See knurl_websocket_send. */
KNURL_API knurl_status knurl_websocket_send_binary(knurl_websocket *,
                                                   const void *, size_t,
                                                   knurl_error *);
/** AGAIN from on_frame holds the complete frame. Resume redelivers it later,
 * before subsequent frames, without recursion. */
KNURL_API knurl_status knurl_websocket_resume(knurl_websocket *, knurl_error *);
/** Code zero sends an empty close; otherwise use a valid protocol/application
 * code and at most 123 UTF-8 bytes. Repeated close is harmless. */
KNURL_API knurl_status knurl_websocket_close(knurl_websocket *, unsigned short,
                                             knurl_bytes, knurl_error *);
/** Inspect the connection phase while its borrowed handle is valid. */
KNURL_API knurl_websocket_phase knurl_websocket_state(const knurl_websocket *);
/** Borrow the connection's server; the WebSocket handle expires after on_close
 * returns.
 */
KNURL_API knurl_server *knurl_websocket_server(const knurl_websocket *);
/** Borrow the connection's owner loop; never retain across fork/run boundaries.
 */
KNURL_API knurl_loop *knurl_websocket_loop(const knurl_websocket *);
/** Includes each complete wire allocation until transport releases it. */
KNURL_API size_t knurl_websocket_queued_bytes(const knurl_websocket *);
/** Read connection-scoped caller userdata while the borrowed handle remains
 * valid.
 */
KNURL_API void *knurl_websocket_userdata(const knurl_websocket *);
/** Replace the caller-owned connection association; the library never frees it.
 */
KNURL_API knurl_status knurl_websocket_set_userdata(knurl_websocket *, void *,
                                                    knurl_error *);

struct knurl_bytes;
struct knurl_error;
struct knurl_loop;
struct knurl_server;
/** Library-owned websocket receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_websocket {
    /** Copy one frame with explicit FIN and validated message state. */
    knurl_status (*send_frame)(struct knurl_websocket *, knurl_websocket_opcode,
                               int, const void *, size_t, struct knurl_error *);
    /** Copy one complete frame; equivalent to send_frame with FIN true. */
    knurl_status (*send)(struct knurl_websocket *, knurl_websocket_opcode,
                         const void *, size_t, struct knurl_error *);
    /** Copy one text frame; AGAIN queues nothing. See knurl_websocket_send for
     * byte/opcode behavior.
     */
    knurl_status (*send_text)(struct knurl_websocket *, const char *, size_t,
                              struct knurl_error *);
    /** Copy one binary frame; AGAIN queues nothing. See knurl_websocket_send.
     */
    knurl_status (*send_binary)(struct knurl_websocket *, const void *, size_t,
                                struct knurl_error *);
    /** AGAIN from on_frame holds the complete frame. Resume redelivers it
     * later, before subsequent frames, without recursion. */
    knurl_status (*resume)(struct knurl_websocket *, struct knurl_error *);
    /** Code zero sends an empty close; otherwise use a valid
     * protocol/application code and at most 123 UTF-8 bytes. Repeated close is
     * harmless. */
    knurl_status (*close)(struct knurl_websocket *, unsigned short,
                          struct knurl_bytes, struct knurl_error *);
    /** Inspect the connection phase while its borrowed handle is valid. */
    knurl_websocket_phase (*state)(const struct knurl_websocket *);
    /** Borrow the connection's server; the WebSocket handle expires after
     * on_close returns.
     */
    struct knurl_server *(*server)(const struct knurl_websocket *);
    /** Borrow the connection's owner loop; never retain across fork/run
     * boundaries.
     */
    struct knurl_loop *(*loop)(const struct knurl_websocket *);
    /** Includes each complete wire allocation until transport releases it. */
    size_t (*queued_bytes)(const struct knurl_websocket *);
    /** Read connection-scoped caller userdata while the borrowed handle remains
     * valid.
     */
    void *(*userdata)(const struct knurl_websocket *);
    /** Replace the caller-owned connection association; the library never frees
     * it.
     */
    knurl_status (*set_userdata)(struct knurl_websocket *, void *,
                                 struct knurl_error *);
    /** Private library state; applications must not access it. */
    struct knurl_websocket_impl *impl;
};

#ifdef __cplusplus
}
#endif
#endif
