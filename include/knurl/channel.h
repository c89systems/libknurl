#ifndef KNURL_CHANNEL_H
#define KNURL_CHANNEL_H

/** @file channel.h Public channel API. Expected failures return knurl_status
 * and optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/loop.h>

/** Channel notification after its orderly write half has drained/shut down. */
#define KNURL_WRITE_CLOSED 16U

#ifdef __cplusplus
extern "C" {
#endif

/** Values for channel phase. */
typedef enum knurl_channel_phase {
    /** Takeover channel phase: http input. */
    KNURL_CHANNEL_HTTP_INPUT = 0,
    /** Takeover channel phase: tunnel. */
    KNURL_CHANNEL_TUNNEL,
    /** Takeover channel phase: closed. */
    KNURL_CHANNEL_CLOSED
} knurl_channel_phase;

/** Terminal takeover accounting including independently observed read EOF and
 * write shutdown.
 */
typedef struct knurl_channel_outcome {
    /** Portable terminal status; OK indicates successful completion. */
    knurl_status cause;
    /** Nonzero after orderly read-half EOF. */
    int read_eof;
    /** Nonzero after orderly write-half shutdown. */
    int write_shutdown;
    /** Exact bytes received by the takeover channel. */
    knurl_u64 received_bytes;
    /** Exact bytes delivered to the takeover consumer. */
    knurl_u64 delivered_bytes;
    /** Exact bytes accepted for takeover output. */
    knurl_u64 sent_bytes;
} knurl_channel_outcome;

/** Owner-loop takeover readiness notification using KNURL_* bits; bounded
 * read/write operations handle backpressure.
 */
typedef void (*knurl_channel_fn)(knurl_channel *, unsigned int, void *);
/** Terminal takeover callback deferred beyond any active event delivery and
 * before request close.
 */
typedef void (*knurl_channel_close_fn)(knurl_channel *,
                                       const knurl_channel_outcome *, void *);

/** Configuration for channel. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_channel_config {
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
    knurl_channel_fn on_event;
    /** Terminal owner-loop callback; its borrowed handle expires on return. */
    knurl_channel_close_fn on_close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_channel_config;

/** Initialize channel config with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_channel_config_init(knurl_channel_config *config);
/** Headers callback only. Success owns a borrowed channel until request close.
 * Zero capacities inherit server I/O bounds. Explicit send capacity is
 * independent; receive/tail capacities must fit server input bounds. */
KNURL_API knurl_status knurl_request_takeover(
    knurl_request *request, const knurl_channel_config *config,
    knurl_channel **out, knurl_error *error);
/** Return the borrowed takeover channel's current HTTP-input/tunnel/closed
 * phase.
 */
KNURL_API knurl_channel_phase knurl_channel_state(const knurl_channel *channel);
/** Return bytes currently retained in the channel's output queue. */
KNURL_API size_t knurl_channel_queued_bytes(const knurl_channel *channel);
/** Copy a bounded input prefix; AGAIN means no input is ready, EOF means read-
 * half closure.
 */
KNURL_API knurl_status knurl_channel_read(knurl_channel *channel, void *buffer,
                                          size_t capacity, size_t *received,
                                          knurl_error *error);
/** HTTP_INPUT cannot write raw bytes. Use its request's managed response. */
KNURL_API knurl_status knurl_channel_write(knurl_channel *channel,
                                           const void *data, size_t length,
                                           size_t *accepted,
                                           knurl_error *error);
/** Change owner-loop event interests; zero suspends notifications. */
KNURL_API knurl_status knurl_channel_interest(knurl_channel *channel,
                                              unsigned int interest,
                                              knurl_error *error);
/** Copies the framer's suffix followed by unread prefetched input, once. */
KNURL_API knurl_status knurl_channel_body_complete(knurl_channel *channel,
                                                   const void *tail,
                                                   size_t tail_length,
                                                   knurl_error *error);
/** Validated successful upstream WebSocket handshake; atomically queues 101. */
KNURL_API knurl_status knurl_channel_upgrade(knurl_channel *channel,
                                             const knurl_header *headers,
                                             size_t count, knurl_error *error);
/** Drain queued output and request orderly write-half closure. */
KNURL_API knurl_status knurl_channel_shutdown_write(knurl_channel *channel,
                                                    knurl_error *error);
/** OK requires drained output and completed HTTP input; error causes cancel.
 * Notifications are deferred past the current event callback. */
KNURL_API knurl_status knurl_channel_close(knurl_channel *channel,
                                           knurl_status cause,
                                           knurl_error *error);

#ifdef __cplusplus
}
#endif
#endif
