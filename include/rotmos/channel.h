#ifndef ROTMOS_CHANNEL_H
#define ROTMOS_CHANNEL_H

/** @file channel.h Public channel API. Expected failures return rotmos_status
 * and optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/loop.h>

/** Channel notification after its orderly write half has drained/shut down. */
#define ROTMOS_WRITE_CLOSED 16U

#ifdef __cplusplus
extern "C" {
#endif

/** Values for channel phase. */
typedef enum rotmos_channel_phase {
    /** Takeover channel phase: http input. */
    ROTMOS_CHANNEL_HTTP_INPUT = 0,
    /** Takeover channel phase: tunnel. */
    ROTMOS_CHANNEL_TUNNEL,
    /** Takeover channel phase: closed. */
    ROTMOS_CHANNEL_CLOSED
} rotmos_channel_phase;

/** Terminal takeover accounting including independently observed read EOF and
 * write shutdown.
 */
typedef struct rotmos_channel_outcome {
    /** Portable terminal status; OK indicates successful completion. */
    rotmos_status cause;
    /** Nonzero after orderly read-half EOF. */
    int read_eof;
    /** Nonzero after orderly write-half shutdown. */
    int write_shutdown;
    /** Exact bytes received by the takeover channel. */
    rotmos_u64 received_bytes;
    /** Exact bytes delivered to the takeover consumer. */
    rotmos_u64 delivered_bytes;
    /** Exact bytes accepted for takeover output. */
    rotmos_u64 sent_bytes;
} rotmos_channel_outcome;

/** Owner-loop takeover readiness notification using ROTMOS_* bits; bounded
 * read/write operations handle backpressure.
 */
typedef void (*rotmos_channel_fn)(rotmos_channel *, unsigned int, void *);
/** Terminal takeover callback deferred beyond any active event delivery and
 * before request close.
 */
typedef void (*rotmos_channel_close_fn)(rotmos_channel *,
                                        const rotmos_channel_outcome *, void *);

/** Configuration for channel. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_channel_config {
    /** Bounded takeover input queue size; zero inherits server input bounds. */
    size_t receive_bytes;
    /** Bounded takeover output queue size; zero inherits server output bounds.
     */
    size_t send_bytes;
    /** Maximum prefetched suffix retained at takeover body completion. */
    size_t tail_bytes;
    /** Grace period in milliseconds before shutdown escalation. */
    long shutdown_timeout_ms;
    /** Readiness event mask; zero suspends delivery where supported. */
    unsigned int interest;
    /** Owner-loop readiness/event callback. */
    rotmos_channel_fn on_event;
    /** Terminal owner-loop callback; its borrowed handle expires on return. */
    rotmos_channel_close_fn on_close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_channel_config;

/** Initialize channel config with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_channel_config_init(rotmos_channel_config *config);
/** Headers callback only. Success owns a borrowed channel until request close.
 * Zero capacities inherit server I/O bounds. Explicit send capacity is
 * independent; receive/tail capacities must fit server input bounds. */
ROTMOS_API rotmos_status rotmos_request_takeover(
    rotmos_request *request, const rotmos_channel_config *config,
    rotmos_channel **out, rotmos_error *error);
/** Return the borrowed takeover channel's current HTTP-input/tunnel/closed
 * phase.
 */
ROTMOS_API rotmos_channel_phase
rotmos_channel_state(const rotmos_channel *channel);
/** Return bytes currently retained in the channel's output queue. */
ROTMOS_API size_t rotmos_channel_queued_bytes(const rotmos_channel *channel);
/** Copy a bounded input prefix; AGAIN means no input is ready, EOF means read-
 * half closure.
 */
ROTMOS_API rotmos_status rotmos_channel_read(rotmos_channel *channel,
                                             void *buffer, size_t capacity,
                                             size_t *received,
                                             rotmos_error *error);
/** HTTP_INPUT cannot write raw bytes. Use its request's managed response. */
ROTMOS_API rotmos_status rotmos_channel_write(rotmos_channel *channel,
                                              const void *data, size_t length,
                                              size_t *accepted,
                                              rotmos_error *error);
/** Change owner-loop event interests; zero suspends notifications. */
ROTMOS_API rotmos_status rotmos_channel_interest(rotmos_channel *channel,
                                                 unsigned int interest,
                                                 rotmos_error *error);
/** Copies the framer's suffix followed by unread prefetched input, once. */
ROTMOS_API rotmos_status rotmos_channel_body_complete(rotmos_channel *channel,
                                                      const void *tail,
                                                      size_t tail_length,
                                                      rotmos_error *error);
/** Validated successful upstream WebSocket handshake; atomically queues 101. */
ROTMOS_API rotmos_status rotmos_channel_upgrade(rotmos_channel *channel,
                                                const rotmos_header *headers,
                                                size_t count,
                                                rotmos_error *error);
/** Drain queued output and request orderly write-half closure. */
ROTMOS_API rotmos_status rotmos_channel_shutdown_write(rotmos_channel *channel,
                                                       rotmos_error *error);
/** OK requires drained output and completed HTTP input; error causes cancel.
 * Notifications are deferred past the current event callback. */
ROTMOS_API rotmos_status rotmos_channel_close(rotmos_channel *channel,
                                              rotmos_status cause,
                                              rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
