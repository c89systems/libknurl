#include "channel-backend.h"
#include <knurl/kore.h>
#include "kore.h"
#include "http.h"
#include "../src/websocket_internal.h"

#include <sys/socket.h>
#include <string.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned char payload[32768];
/* Reuse a bounded chunk; the full prefix must exceed both socket buffers so
 * cancellation is observed before the producer drains on every platform. */
static int completions;
static int failed;
static int notification[2] = {-1, -1};

static void idle_channel(knurl_channel *channel) { (void)channel; }

static int retained_frame_budget(void)
{
    struct knurl_websocket_impl state;
    knurl_websocket *ws = &state.receiver;
    knurl_channel *channel;
    int bounded;
    channel = calloc(1, sizeof(struct knurl_channel_impl));
    if (channel == NULL)
        return 0;
    memset(&state, 0, sizeof(state));
    knurl_websocket_receiver_init(ws);
    knurl_channel_receiver_init(channel);
    ws->impl->channel = channel;
    channel->impl->config.send_bytes = 127;
    /* Simulate a pinned frame whose prefix has reached the transport. */
    channel->impl->retained = 124;
    channel->impl->queued = 1;
    channel->impl->ops.interest = channel->impl->schedule = idle_channel;
    bounded = knurl_websocket_queue(ws, KNURL_WEBSOCKET_BINARY, "ab", 2,
                                    NULL) == KNURL_AGAIN;
    bounded = bounded && channel->impl->head == NULL &&
              channel->impl->queued == 1 && channel->impl->retained == 124;
    knurl_channel_release(channel, 0);
    return bounded;
}

/* Pool reuse must not carry error/half-close readiness into a new socket. */
static int clean_connection_events(void)
{
    struct connection *connection;
    int clean;
    connection = kore_connection_new(NULL);
    connection->fd = -1;
    connection->evt.flags = KORE_EVENT_ERROR | KORE_EVENT_READ_CLOSED;
    ++worker_active_connections;
    kore_connection_remove(connection);
    connection = kore_connection_new(NULL);
    connection->fd = -1;
    clean = connection->evt.flags == 0;
    ++worker_active_connections;
    kore_connection_remove(connection);
    return clean;
}

static int prefix_done(struct netbuf *buffer)
{
    if (buffer->s_off != buffer->b_len)
        failed = 1;
    ++completions;
    if ((size_t)completions < KNURL_TEST_PREFIX_BYTES / sizeof(payload))
        net_send_stream(buffer->owner, payload, sizeof(payload), prefix_done,
                        NULL);
    return KORE_RESULT_OK;
}

int knurl_test_prefix(knurl_request *request)
{
    struct http_request *native;
    int size;
    socklen_t size_length;
    if (!clean_connection_events() || !retained_frame_budget())
        return 0;
    native = knurl_kore_request(request);
    http_response_header(native, "X-Escape-Context", "retained");
    http_response_cookie(native, "escape", "kept", NULL, 0, 0, NULL);
    completions = failed = 0;
    size = 4096;
    if (setsockopt(native->owner->fd, SOL_SOCKET, SO_SNDBUF, &size,
                   sizeof(size)) != 0)
        return 0;
    size_length = sizeof(size);
    if (getsockopt(native->owner->fd, SOL_SOCKET, SO_SNDBUF, &size,
                   &size_length) != 0 ||
        size <= 0 || (unsigned long)size > KNURL_TEST_PREFIX_BYTES / 4)
        return 0;
    memset(payload, 'p', sizeof(payload));
    net_send_stream(native->owner, payload, sizeof(payload), prefix_done, NULL);
    return !TAILQ_EMPTY(&native->owner->send_queue);
}

int knurl_test_prefix_finished(void)
{
    return (size_t)completions == KNURL_TEST_PREFIX_BYTES / sizeof(payload) &&
           !failed;
}

int knurl_test_prefix_init(void) { return pipe(notification) == 0; }

void knurl_test_prefix_cleanup(void)
{
    (void)close(notification[0]);
    (void)close(notification[1]);
}

int knurl_test_prefix_signal(void)
{
    return write(notification[1], "x", 1) == 1;
}

int knurl_test_prefix_wait(void)
{
    struct pollfd fd;
    char byte;
    fd.fd = notification[0];
    fd.events = POLLIN;
    fd.revents = 0;
    return poll(&fd, 1, 3000) == 1 && read(notification[0], &byte, 1) == 1 &&
           byte == 'x';
}
