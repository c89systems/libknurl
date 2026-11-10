#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "channel_internal.h"
#include "response_internal.h"

#include <sys/socket.h>
#include <openssl/err.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct knurl_kore_channel {
    struct connection *connection;
    void (*event)(void *, int);
    int (*handle)(struct connection *);
    void (*disconnect)(struct connection *);
    unsigned int registered;
    unsigned int managed_wait;
    int handshake_drained;
    int attached;
};

static void interest(knurl_channel *channel);
static int flush_channel(knurl_channel *channel);

static knurl_status failed(knurl_error *error, int code, const char *operation)
{
    (void)knurl_fail(error, KNURL_IO, operation,
                     "channel transport operation failed");
    if (error != NULL) {
        error->source = KNURL_ERROR_BACKEND;
        error->native_code = code;
    }
    return KNURL_IO;
}

static knurl_status retry_tls(knurl_channel *channel, int result,
                              unsigned int *wait, int reading,
                              knurl_error *error)
{
    struct knurl_kore_channel *backend;
    int code;
    int saved;
    backend = channel->backend;
    saved = errno;
    code = SSL_get_error(backend->connection->tls, result);
    if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
        *wait = code == SSL_ERROR_WANT_READ ? KNURL_READABLE : KNURL_WRITABLE;
        channel->io_ready &= ~*wait;
        knurl_error_clear(error);
        return KNURL_AGAIN;
    }
    if (reading && code == SSL_ERROR_ZERO_RETURN) {
        *wait = 0;
        channel->io_ready &= ~KNURL_READABLE;
        knurl_error_clear(error);
        return KNURL_EOF;
    }
    if (code == SSL_ERROR_SYSCALL &&
        (saved == EAGAIN || saved == EWOULDBLOCK || saved == EINTR)) {
        *wait = reading ? KNURL_READABLE : KNURL_WRITABLE;
        channel->io_ready &= ~*wait;
        return KNURL_AGAIN;
    }
    return failed(error, code,
                  reading ? "channel_tls_read" : "channel_tls_write");
}

static knurl_status read_channel(knurl_channel *channel, void *buffer,
                                 size_t capacity, size_t *received,
                                 knurl_error *error)
{
    struct knurl_kore_channel *backend;
    struct connection *connection;
    ssize_t count;
    int result;
    backend = channel->backend;
    connection = backend->connection;
    *received = 0;
    if (connection == NULL || connection->state != CONN_STATE_ESTABLISHED)
        return failed(error, 0, "channel_read");
    if (connection->tls != NULL && SSL_pending(connection->tls) > 0)
        channel->io_ready |= KNURL_READABLE;
    if (channel->read_wait != 0 && !(channel->io_ready & channel->read_wait))
        return KNURL_AGAIN;
    if (capacity > INT_MAX)
        capacity = INT_MAX;
    if (connection->tls != NULL) {
        ERR_clear_error();
        errno = 0;
        result = SSL_read(connection->tls, buffer, (int)capacity);
        if (result <= 0)
            return retry_tls(channel, result, &channel->read_wait, 1, error);
        count = result;
    } else {
        count = recv(connection->fd, buffer, capacity, 0);
        if (count == 0) {
            channel->read_wait = 0;
            channel->io_ready &= ~KNURL_READABLE;
            return KNURL_EOF;
        }
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                channel->read_wait = KNURL_READABLE;
                channel->io_ready &= ~KNURL_READABLE;
                return KNURL_AGAIN;
            }
            return failed(error, errno, "channel_recv");
        }
    }
    channel->read_wait = 0;
    *received = (size_t)count;
    return KNURL_OK;
}

static knurl_status write_channel(knurl_channel *channel, const void *buffer,
                                  size_t length, size_t *sent,
                                  knurl_error *error)
{
    struct knurl_kore_channel *backend;
    struct connection *connection;
    ssize_t count;
    int result;
    int flags;
    backend = channel->backend;
    connection = backend->connection;
    *sent = 0;
    if (connection == NULL || connection->state != CONN_STATE_ESTABLISHED)
        return failed(error, 0, "channel_write");
    if (channel->write_wait != 0 && !(channel->io_ready & channel->write_wait))
        return KNURL_AGAIN;
    if (length > INT_MAX)
        length = INT_MAX;
    if (connection->tls != NULL) {
        ERR_clear_error();
        errno = 0;
        result = SSL_write(connection->tls, buffer, (int)length);
        if (result <= 0)
            return retry_tls(channel, result, &channel->write_wait, 0, error);
        count = result;
    } else {
        flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        count = send(connection->fd, buffer, length, flags);
        if (count <= 0) {
            if (count < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                channel->write_wait = KNURL_WRITABLE;
                channel->io_ready &= ~KNURL_WRITABLE;
                return KNURL_AGAIN;
            }
            return failed(error, errno, "channel_send");
        }
    }
    channel->write_wait = 0;
    *sent = (size_t)count;
    return KNURL_OK;
}

static knurl_status shutdown_channel(knurl_channel *channel, knurl_error *error)
{
    struct knurl_kore_channel *backend;
    struct connection *connection;
    int result;
    backend = channel->backend;
    connection = backend->connection;
    if (connection == NULL)
        return failed(error, 0, "channel_shutdown");
    if (channel->shutdown_wait != 0 &&
        !(channel->io_ready & channel->shutdown_wait))
        return KNURL_AGAIN;
    if (connection->tls != NULL) {
        ERR_clear_error();
        errno = 0;
        result = SSL_shutdown(connection->tls);
        if (result < 0)
            return retry_tls(channel, result, &channel->shutdown_wait, 0,
                             error);
        /* Return 0 completes our write half. Continue SSL_read for the peer's
         * data/close_notify rather than discarding its pending application
         * data. */
    } else if (shutdown(connection->fd, SHUT_WR) != 0) {
        if (errno == EINTR || errno == EAGAIN) {
            channel->shutdown_wait = KNURL_WRITABLE;
            channel->io_ready &= ~KNURL_WRITABLE;
            return KNURL_AGAIN;
        }
        return failed(error, errno, "channel_shutdown");
    }
    channel->shutdown_wait = 0;
    return KNURL_OK;
}

static void channel_disconnected(struct connection *connection)
{
    knurl_channel *channel;
    struct knurl_kore_channel *backend;
    channel = connection->hdlr_extra;
    backend = channel->backend;
    connection->hdlr_extra = NULL;
    connection->evt.handle = backend->event;
    connection->handle = backend->handle;
    connection->disconnect = backend->disconnect;
    backend->connection = NULL;
    knurl_channel_finish(channel, KNURL_IO);
    if (connection->disconnect != NULL)
        connection->disconnect(connection);
    knurl_channel_schedule(channel);
}

static void event(void *userdata, int error)
{
    struct connection *connection;
    knurl_channel *channel;
    unsigned int ready;
    connection = userdata;
    channel = connection->hdlr_extra;
    ready = 0;
    if (connection->evt.flags & KORE_EVENT_READ)
        ready |= KNURL_READABLE;
    if (connection->evt.flags & KORE_EVENT_WRITE)
        ready |= KNURL_WRITABLE;
    if (connection->evt.flags & KORE_EVENT_READ_CLOSED)
        ready |= KNURL_READABLE;
    if ((connection->evt.flags & KORE_EVENT_ERROR) || (error && ready == 0)) {
        connection->evt.flags = 0;
        knurl_channel_finish(channel, KNURL_IO);
        kore_connection_disconnect(connection);
        return;
    }
    connection->evt.flags = 0;
    channel->io_ready |= ready;
    if ((ready & (channel->read_wait ? channel->read_wait : KNURL_READABLE)) !=
        0)
        channel->read_notified = 0;
    knurl_channel_schedule(channel);
}

void knurl_kore_channel_http_progress(struct connection *connection,
                                      size_t amount)
{
    knurl_channel *channel;
    if (amount != 0 && connection->evt.handle == event) {
        channel = connection->hdlr_extra;
        (void)knurl_loop_monotonic(&channel->last_activity, NULL);
    }
}

static void interest(knurl_channel *channel)
{
    struct knurl_kore_channel *backend;
    struct connection *connection;
    unsigned int wanted;
    int result;
    backend = channel->backend;
    connection = backend->connection;
    if (!backend->attached || connection == NULL ||
        connection->state != CONN_STATE_ESTABLISHED)
        return;
    if (channel->phase == KNURL_CHANNEL_CLOSED &&
        channel->previous_phase == KNURL_CHANNEL_TUNNEL) {
        kore_connection_disconnect(connection);
        return;
    }
    wanted = 0;
    if (connection->tls != NULL && SSL_pending(connection->tls) > 0 &&
        channel->phase != KNURL_CHANNEL_CLOSED && !channel->input_complete &&
        !channel->read_notified &&
        (channel->config.interest & KNURL_READABLE)) {
        channel->io_ready |= KNURL_READABLE;
        knurl_channel_schedule(channel);
    }
    if (channel->phase != KNURL_CHANNEL_CLOSED && !channel->input_complete &&
        !channel->outcome.read_eof && !channel->read_notified &&
        channel->receive_length == 0 &&
        (channel->config.interest & KNURL_READABLE))
        wanted |= channel->read_wait ? channel->read_wait : KNURL_READABLE;
    if (!TAILQ_EMPTY(&connection->send_queue)) {
        wanted |=
            backend->managed_wait ? backend->managed_wait : KNURL_WRITABLE;
    } else if (channel->phase == KNURL_CHANNEL_TUNNEL &&
               (channel->response == NULL ||
                (channel->response->head == NULL &&
                 channel->response->pending_head == NULL))) {
        if (channel->head != NULL)
            wanted |=
                channel->write_wait ? channel->write_wait : KNURL_WRITABLE;
        else if (channel->shutdown_requested &&
                 !channel->outcome.write_shutdown)
            wanted |= channel->shutdown_wait ? channel->shutdown_wait
                                             : KNURL_WRITABLE;
    }
    if (wanted == backend->registered)
        return;
    result = kore_library_watch_update(connection->fd, backend->registered,
                                       wanted, connection);
    if (result != 0) {
        kore_connection_disconnect(connection);
        return;
    }
    backend->registered = wanted;
}

int knurl_kore_channel_flush_http(knurl_request *request)
{
    return flush_channel(request->channel);
}

static int flush_channel(knurl_channel *channel)
{
    struct knurl_kore_channel *backend;
    struct connection *connection;
    unsigned int wanted;
    backend = channel->backend;
    connection = backend->connection;
    if (connection == NULL)
        return KORE_RESULT_ERROR;
    wanted = backend->managed_wait ? backend->managed_wait : KNURL_WRITABLE;
    if (!(channel->io_ready & wanted)) {
        interest(channel);
        return KORE_RESULT_OK;
    }
    connection->evt.flags |= KORE_EVENT_WRITE;
    if (!net_send_flush(connection))
        return KORE_RESULT_ERROR;
    if (!TAILQ_EMPTY(&connection->send_queue)) {
        backend->managed_wait =
            connection->tls != NULL && SSL_want(connection->tls) == SSL_READING
                ? KNURL_READABLE
                : KNURL_WRITABLE;
        channel->io_ready &= ~backend->managed_wait;
    } else {
        backend->managed_wait = 0;
    }
    interest(channel);
    return KORE_RESULT_OK;
}

static void pump(knurl_channel *channel)
{
    struct knurl_kore_channel *backend;
    struct connection *connection;
    struct knurl_channel_chunk *chunk;
    knurl_status status;
    size_t count;
    unsigned int budget;
    backend = channel->backend;
    connection = backend->connection;
    if (connection == NULL) {
        knurl_channel_finish(channel, KNURL_IO);
        return;
    }
    if (!TAILQ_EMPTY(&connection->send_queue)) {
        if (!flush_channel(channel)) {
            knurl_channel_finish(channel, KNURL_IO);
            kore_connection_disconnect(connection);
            return;
        }
        if (!TAILQ_EMPTY(&connection->send_queue))
            return;
    }
    if (channel->phase != KNURL_CHANNEL_TUNNEL ||
        (channel->response != NULL &&
         (channel->response->head != NULL ||
          channel->response->pending_head != NULL)))
        return;
    if (!backend->handshake_drained) {
        backend->handshake_drained = 1;
        channel->write_notified = 0;
    }
    budget = 64;
    while (channel->head != NULL && budget-- != 0) {
        chunk = channel->head;
        count = 0;
        status =
            channel->ops.write(channel, chunk->data + chunk->offset,
                               chunk->length - chunk->offset, &count, NULL);
        if (status == KNURL_AGAIN)
            break;
        if (status != KNURL_OK) {
            knurl_channel_finish(channel, status);
            return;
        }
        chunk->offset += count;
        channel->queued -= count;
        knurl_channel_progress(channel, 0, count);
        if (chunk->offset == chunk->length) {
            channel->retained -= chunk->length;
            channel->head = chunk->next;
            if (channel->head == NULL)
                channel->last = NULL;
            free(chunk->data);
            free(chunk);
        }
    }
    if (channel->head != NULL &&
        (channel->write_wait == 0 || (channel->io_ready & channel->write_wait)))
        knurl_channel_schedule(channel);
    if (channel->head == NULL && channel->shutdown_requested &&
        !channel->outcome.write_shutdown) {
        status = channel->ops.shutdown(channel, NULL);
        if (status == KNURL_OK) {
            channel->outcome.write_shutdown = 1;
            knurl_channel_schedule(channel);
        } else if (status != KNURL_AGAIN) {
            knurl_channel_finish(channel, status);
        }
    }
    interest(channel);
}

static void body_completed(knurl_channel *channel)
{
    knurl_kore_response_body_end(channel->request);
}

static void release(knurl_channel *channel, int restore)
{
    struct knurl_kore_channel *backend;
    struct connection *connection;
    backend = channel->backend;
    connection = backend->connection;
    if (backend->attached && connection != NULL) {
        connection->hdlr_extra = NULL;
        connection->evt.handle = backend->event;
        connection->handle = backend->handle;
        connection->disconnect = backend->disconnect;
        if (restore) {
            if (channel->tail_length != 0)
                kore_library_http_pipeline_save(connection, channel->tail,
                                                channel->tail_length);
        } else {
            kore_connection_disconnect(connection);
        }
    }
    free(backend);
    channel->backend = NULL;
}

knurl_status knurl_kore_channel_prepare(knurl_request *request,
                                        const knurl_channel_config *config,
                                        knurl_channel **out, knurl_error *error)
{
    struct http_request *native;
    struct connection *connection;
    struct knurl_kore_channel *backend;
    knurl_bytes initial;
    knurl_bytes backlog;
    knurl_channel *channel;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (request == NULL || config == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "request_takeover",
                          "provide request, configuration and output");
    status = knurl_loop_owner(request->server->loop, error);
    if (status != KNURL_OK)
        return status;
    if (!request->in_headers || request->channel != NULL ||
        request->close_notified)
        return knurl_fail(
            error, KNURL_STATE, "request_takeover",
            "takeover is exclusive and available only during headers dispatch");
    native = request->native;
    connection = native->owner;
    if (connection == NULL || connection->state != CONN_STATE_ESTABLISHED ||
        connection->hdlr_extra != NULL)
        return knurl_fail(error, KNURL_CONFLICT, "request_takeover",
                          "connection already has an exclusive owner");
    knurl_kore_body_initial(request, &initial);
    backlog.data = connection->http_pipeline;
    backlog.length = connection->http_pipeline_len;
    status = knurl_channel_create(request, config, initial, backlog, &channel,
                                  error);
    if (status != KNURL_OK)
        return status;
    backend = calloc(1, sizeof(*backend));
    if (backend == NULL) {
        free(channel->receive);
        free(channel);
        return knurl_fail(error, KNURL_NOMEM, "request_takeover",
                          "cannot allocate channel adapter");
    }
    backend->connection = connection;
    backend->event = connection->evt.handle;
    backend->handle = connection->handle;
    backend->disconnect = connection->disconnect;
    backend->registered = KNURL_WRITABLE;
    channel->backend = backend;
    channel->ops.read = read_channel;
    channel->ops.write = write_channel;
    channel->ops.shutdown = shutdown_channel;
    channel->ops.interest = interest;
    channel->ops.pump = pump;
    channel->ops.release = release;
    channel->ops.completed = body_completed;
    *out = channel;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_kore_channel_attach(knurl_channel *channel)
{
    struct knurl_kore_channel *backend;
    struct connection *connection;
    knurl_request *request;
    backend = channel->backend;
    connection = backend->connection;
    request = channel->request;
    backend->attached = 1;
    request->channel = channel;
    knurl_kore_body_transferred(request);
    kore_free(connection->http_pipeline);
    connection->http_pipeline = NULL;
    connection->http_pipeline_len = 0;
    connection->hdlr_extra = channel;
    connection->evt.handle = event;
    connection->disconnect = channel_disconnected;
    connection->http_timeout = 0;
    knurl_channel_schedule(channel);
}

knurl_status knurl_request_takeover(knurl_request *request,
                                    const knurl_channel_config *config,
                                    knurl_channel **out, knurl_error *error)
{
    knurl_status status;
    status = knurl_kore_channel_prepare(request, config, out, error);
    if (status == KNURL_OK)
        knurl_kore_channel_attach(*out);
    return status;
}

void knurl_kore_channel_detach(knurl_channel *channel)
{
    struct knurl_kore_channel *backend;
    backend = channel->backend;
    if (backend->connection != NULL) {
        backend->connection->proto = CONN_PROTO_WEBSOCKET;
        kore_connection_stop_idletimer(backend->connection);
    }
    channel->request = NULL;
    channel->response = NULL;
}
