#ifndef ROTMOS_CHANNEL_H
#define ROTMOS_CHANNEL_H

#include <rotmos/loop.h>

/* Channel notification after its orderly write half has drained/shut down. */
#define ROTMOS_WRITE_CLOSED 16U

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rotmos_channel_phase {
    ROTMOS_CHANNEL_HTTP_INPUT = 0,
    ROTMOS_CHANNEL_TUNNEL,
    ROTMOS_CHANNEL_CLOSED
} rotmos_channel_phase;

typedef struct rotmos_channel_outcome {
    rotmos_status cause;
    int read_eof;
    int write_shutdown;
    rotmos_u64 received_bytes;
    rotmos_u64 delivered_bytes;
    rotmos_u64 sent_bytes;
} rotmos_channel_outcome;

typedef void (*rotmos_channel_fn)(rotmos_channel *, unsigned int, void *);
typedef void (*rotmos_channel_close_fn)(rotmos_channel *,
                                        const rotmos_channel_outcome *, void *);

typedef struct rotmos_channel_config {
    size_t receive_bytes;
    size_t send_bytes;
    size_t tail_bytes;
    long shutdown_timeout_ms;
    unsigned int interest;
    rotmos_channel_fn on_event;
    rotmos_channel_close_fn on_close;
    void *userdata;
} rotmos_channel_config;

ROTMOS_API void rotmos_channel_config_init(rotmos_channel_config *config);
/* Headers callback only. Success owns a borrowed channel until request close.
 * Zero capacities inherit the server's finite I/O bounds. */
ROTMOS_API rotmos_status rotmos_request_takeover(
    rotmos_request *request, const rotmos_channel_config *config,
    rotmos_channel **out, rotmos_error *error);
ROTMOS_API rotmos_channel_phase
rotmos_channel_state(const rotmos_channel *channel);
ROTMOS_API size_t rotmos_channel_queued_bytes(const rotmos_channel *channel);
ROTMOS_API rotmos_status rotmos_channel_read(rotmos_channel *channel,
                                             void *buffer, size_t capacity,
                                             size_t *received,
                                             rotmos_error *error);
/* HTTP_INPUT cannot write raw bytes. Use its request's managed response. */
ROTMOS_API rotmos_status rotmos_channel_write(rotmos_channel *channel,
                                              const void *data, size_t length,
                                              size_t *accepted,
                                              rotmos_error *error);
ROTMOS_API rotmos_status rotmos_channel_interest(rotmos_channel *channel,
                                                 unsigned int interest,
                                                 rotmos_error *error);
/* Copies the framer's suffix followed by unread prefetched input, once. */
ROTMOS_API rotmos_status rotmos_channel_body_complete(rotmos_channel *channel,
                                                      const void *tail,
                                                      size_t tail_length,
                                                      rotmos_error *error);
/* Validated successful upstream WebSocket handshake; atomically queues 101. */
ROTMOS_API rotmos_status rotmos_channel_upgrade(rotmos_channel *channel,
                                                const rotmos_header *headers,
                                                size_t count,
                                                rotmos_error *error);
ROTMOS_API rotmos_status rotmos_channel_shutdown_write(rotmos_channel *channel,
                                                       rotmos_error *error);
/* OK requires drained output and completed HTTP input; error causes cancel.
 * Notifications are deferred past the current event callback. */
ROTMOS_API rotmos_status rotmos_channel_close(rotmos_channel *channel,
                                              rotmos_status cause,
                                              rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
