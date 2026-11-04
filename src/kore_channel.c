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

struct rotmos_kore_channel {
    struct connection *connection;
    void (*event)(void *, int);
    int (*handle)(struct connection *);
    void (*disconnect)(struct connection *);
    unsigned int registered;
    unsigned int managed_wait;
    int handshake_drained;
};

static void interest(rotmos_channel *channel);

static rotmos_status failed(rotmos_error *error, int code,
                            const char *operation)
{
    (void)rotmos_fail(error, ROTMOS_IO, operation,
                      "channel transport operation failed");
    if (error != NULL) {
        error->source = ROTMOS_ERROR_BACKEND;
        error->native_code = code;
    }
    return ROTMOS_IO;
}

static rotmos_status retry_tls(rotmos_channel *channel, int result,
                               unsigned int *wait, int reading,
                               rotmos_error *error)
{
    struct rotmos_kore_channel *backend;
    int code;
    int saved;
    backend = channel->backend;
    saved = errno;
    code = SSL_get_error(backend->connection->tls, result);
    if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
        *wait = code == SSL_ERROR_WANT_READ ? ROTMOS_READABLE : ROTMOS_WRITABLE;
        channel->io_ready &= ~*wait;
        rotmos_error_clear(error);
        return ROTMOS_AGAIN;
    }
    if (reading && code == SSL_ERROR_ZERO_RETURN) {
        *wait = 0;
        channel->io_ready &= ~ROTMOS_READABLE;
        rotmos_error_clear(error);
        return ROTMOS_EOF;
    }
    if (code == SSL_ERROR_SYSCALL &&
        (saved == EAGAIN || saved == EWOULDBLOCK || saved == EINTR)) {
        *wait = reading ? ROTMOS_READABLE : ROTMOS_WRITABLE;
        channel->io_ready &= ~*wait;
        return ROTMOS_AGAIN;
    }
    return failed(error, code,
                  reading ? "channel_tls_read" : "channel_tls_write");
}

static rotmos_status read_channel(rotmos_channel *channel, void *buffer,
                                  size_t capacity, size_t *received,
                                  rotmos_error *error)
{
    struct rotmos_kore_channel *backend;
    struct connection *connection;
    ssize_t count;
    int result;
    backend = channel->backend;
    connection = backend->connection;
    *received = 0;
    if (connection == NULL || connection->state != CONN_STATE_ESTABLISHED)
        return failed(error, 0, "channel_read");
    if (connection->tls != NULL && SSL_pending(connection->tls) > 0)
        channel->io_ready |= ROTMOS_READABLE;
    if (channel->read_wait != 0 && !(channel->io_ready & channel->read_wait))
        return ROTMOS_AGAIN;
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
            channel->io_ready &= ~ROTMOS_READABLE;
            return ROTMOS_EOF;
        }
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                channel->read_wait = ROTMOS_READABLE;
                channel->io_ready &= ~ROTMOS_READABLE;
                return ROTMOS_AGAIN;
            }
            return failed(error, errno, "channel_recv");
        }
    }
    channel->read_wait = 0;
    *received = (size_t)count;
    return ROTMOS_OK;
}

static rotmos_status write_channel(rotmos_channel *channel, const void *buffer,
                                   size_t length, size_t *sent,
                                   rotmos_error *error)
{
    struct rotmos_kore_channel *backend;
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
        return ROTMOS_AGAIN;
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
                channel->write_wait = ROTMOS_WRITABLE;
                channel->io_ready &= ~ROTMOS_WRITABLE;
                return ROTMOS_AGAIN;
            }
            return failed(error, errno, "channel_send");
        }
    }
    channel->write_wait = 0;
    *sent = (size_t)count;
    return ROTMOS_OK;
}

static rotmos_status shutdown_channel(rotmos_channel *channel,
                                      rotmos_error *error)
{
    struct rotmos_kore_channel *backend;
    struct connection *connection;
    int result;
    backend = channel->backend;
    connection = backend->connection;
    if (connection == NULL)
        return failed(error, 0, "channel_shutdown");
    if (channel->shutdown_wait != 0 &&
        !(channel->io_ready & channel->shutdown_wait))
        return ROTMOS_AGAIN;
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
            channel->shutdown_wait = ROTMOS_WRITABLE;
            channel->io_ready &= ~ROTMOS_WRITABLE;
            return ROTMOS_AGAIN;
        }
        return failed(error, errno, "channel_shutdown");
    }
    channel->shutdown_wait = 0;
    return ROTMOS_OK;
}

static void channel_disconnected(struct connection *connection)
{
    rotmos_channel *channel;
    struct rotmos_kore_channel *backend;
    channel = connection->hdlr_extra;
    backend = channel->backend;
    connection->hdlr_extra = NULL;
    connection->evt.handle = backend->event;
    connection->handle = backend->handle;
    connection->disconnect = backend->disconnect;
    backend->connection = NULL;
    rotmos_channel_finish(channel, ROTMOS_IO);
    if (connection->disconnect != NULL)
        connection->disconnect(connection);
    rotmos_channel_schedule(channel);
}

static void event(void *userdata, int error)
{
    struct connection *connection;
    rotmos_channel *channel;
    unsigned int ready;
    connection = userdata;
    channel = connection->hdlr_extra;
    ready = 0;
    if (connection->evt.flags & KORE_EVENT_READ)
        ready |= ROTMOS_READABLE;
    if (connection->evt.flags & KORE_EVENT_WRITE)
        ready |= ROTMOS_WRITABLE;
    if (connection->evt.flags & KORE_EVENT_READ_CLOSED)
        ready |= ROTMOS_READABLE;
    if ((connection->evt.flags & KORE_EVENT_ERROR) || (error && ready == 0)) {
        connection->evt.flags = 0;
        rotmos_channel_finish(channel, ROTMOS_IO);
        kore_connection_disconnect(connection);
        return;
    }
    connection->evt.flags = 0;
    channel->io_ready |= ready;
    if ((ready & (channel->read_wait ? channel->read_wait : ROTMOS_READABLE)) !=
        0)
        channel->read_notified = 0;
    rotmos_channel_schedule(channel);
}

void rotmos_kore_channel_http_progress(struct connection *connection,
                                       size_t amount)
{
    rotmos_channel *channel;
    if (amount != 0 && connection->evt.handle == event) {
        channel = connection->hdlr_extra;
        (void)rotmos_loop_monotonic(&channel->last_activity, NULL);
    }
}

static void interest(rotmos_channel *channel)
{
    struct rotmos_kore_channel *backend;
    struct connection *connection;
    unsigned int wanted;
    int result;
    backend = channel->backend;
    connection = backend->connection;
    if (connection == NULL || connection->state != CONN_STATE_ESTABLISHED)
        return;
    if (channel->phase == ROTMOS_CHANNEL_CLOSED &&
        channel->previous_phase == ROTMOS_CHANNEL_TUNNEL) {
        kore_connection_disconnect(connection);
        return;
    }
    wanted = 0;
    if (connection->tls != NULL && SSL_pending(connection->tls) > 0 &&
        channel->phase != ROTMOS_CHANNEL_CLOSED && !channel->input_complete &&
        !channel->read_notified &&
        (channel->config.interest & ROTMOS_READABLE)) {
        channel->io_ready |= ROTMOS_READABLE;
        rotmos_channel_schedule(channel);
    }
    if (channel->phase != ROTMOS_CHANNEL_CLOSED && !channel->input_complete &&
        !channel->outcome.read_eof && !channel->read_notified &&
        channel->receive_length == 0 &&
        (channel->config.interest & ROTMOS_READABLE))
        wanted |= channel->read_wait ? channel->read_wait : ROTMOS_READABLE;
    if (!TAILQ_EMPTY(&connection->send_queue)) {
        wanted |=
            backend->managed_wait ? backend->managed_wait : ROTMOS_WRITABLE;
    } else if (channel->phase == ROTMOS_CHANNEL_TUNNEL &&
               channel->request->response->head == NULL) {
        if (channel->head != NULL)
            wanted |=
                channel->write_wait ? channel->write_wait : ROTMOS_WRITABLE;
        else if (channel->shutdown_requested &&
                 !channel->outcome.write_shutdown)
            wanted |= channel->shutdown_wait ? channel->shutdown_wait
                                             : ROTMOS_WRITABLE;
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

int rotmos_kore_channel_flush_http(rotmos_request *request)
{
    rotmos_channel *channel;
    struct rotmos_kore_channel *backend;
    struct connection *connection;
    unsigned int wanted;
    channel = request->channel;
    backend = channel->backend;
    connection = backend->connection;
    if (connection == NULL)
        return KORE_RESULT_ERROR;
    wanted = backend->managed_wait ? backend->managed_wait : ROTMOS_WRITABLE;
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
                ? ROTMOS_READABLE
                : ROTMOS_WRITABLE;
        channel->io_ready &= ~backend->managed_wait;
    } else {
        backend->managed_wait = 0;
    }
    interest(channel);
    return KORE_RESULT_OK;
}

static void pump(rotmos_channel *channel)
{
    struct rotmos_kore_channel *backend;
    struct connection *connection;
    struct rotmos_channel_chunk *chunk;
    rotmos_status status;
    size_t count;
    unsigned int budget;
    backend = channel->backend;
    connection = backend->connection;
    if (connection == NULL) {
        rotmos_channel_finish(channel, ROTMOS_IO);
        return;
    }
    if (!TAILQ_EMPTY(&connection->send_queue)) {
        if (!rotmos_kore_channel_flush_http(channel->request)) {
            rotmos_channel_finish(channel, ROTMOS_IO);
            kore_connection_disconnect(connection);
            return;
        }
        if (!TAILQ_EMPTY(&connection->send_queue))
            return;
    }
    if (channel->phase != ROTMOS_CHANNEL_TUNNEL ||
        channel->request->response->head != NULL)
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
        if (status == ROTMOS_AGAIN)
            break;
        if (status != ROTMOS_OK) {
            rotmos_channel_finish(channel, status);
            return;
        }
        chunk->offset += count;
        channel->queued -= count;
        rotmos_channel_progress(channel, 0, count);
        if (chunk->offset == chunk->length) {
            channel->head = chunk->next;
            if (channel->head == NULL)
                channel->last = NULL;
            free(chunk->data);
            free(chunk);
        }
    }
    if (channel->head != NULL &&
        (channel->write_wait == 0 || (channel->io_ready & channel->write_wait)))
        rotmos_channel_schedule(channel);
    if (channel->head == NULL && channel->shutdown_requested &&
        !channel->outcome.write_shutdown) {
        status = channel->ops.shutdown(channel, NULL);
        if (status == ROTMOS_OK) {
            channel->outcome.write_shutdown = 1;
            rotmos_channel_schedule(channel);
        } else if (status != ROTMOS_AGAIN) {
            rotmos_channel_finish(channel, status);
        }
    }
    interest(channel);
}

static void body_completed(rotmos_channel *channel)
{
    rotmos_kore_response_body_end(channel->request);
}

static void release(rotmos_channel *channel, int restore)
{
    struct rotmos_kore_channel *backend;
    struct connection *connection;
    backend = channel->backend;
    connection = backend->connection;
    if (connection != NULL) {
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

rotmos_status rotmos_request_takeover(rotmos_request *request,
                                      const rotmos_channel_config *config,
                                      rotmos_channel **out, rotmos_error *error)
{
    struct http_request *native;
    struct connection *connection;
    struct rotmos_kore_channel *backend;
    rotmos_bytes initial;
    rotmos_bytes backlog;
    rotmos_channel *channel;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (request == NULL || config == NULL || out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "request_takeover",
                           "provide request, configuration and output");
    status = rotmos_loop_owner(request->server->loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (!request->in_headers || request->channel != NULL ||
        request->close_notified)
        return rotmos_fail(
            error, ROTMOS_STATE, "request_takeover",
            "takeover is exclusive and available only during headers dispatch");
    native = request->native;
    connection = native->owner;
    if (connection == NULL || connection->state != CONN_STATE_ESTABLISHED ||
        connection->hdlr_extra != NULL)
        return rotmos_fail(error, ROTMOS_CONFLICT, "request_takeover",
                           "connection already has an exclusive owner");
    rotmos_kore_body_initial(request, &initial);
    backlog.data = connection->http_pipeline;
    backlog.length = connection->http_pipeline_len;
    status = rotmos_channel_create(request, config, initial, backlog, &channel,
                                   error);
    if (status != ROTMOS_OK)
        return status;
    backend = calloc(1, sizeof(*backend));
    if (backend == NULL) {
        free(channel->receive);
        free(channel);
        return rotmos_fail(error, ROTMOS_NOMEM, "request_takeover",
                           "cannot allocate channel adapter");
    }
    backend->connection = connection;
    backend->event = connection->evt.handle;
    backend->handle = connection->handle;
    backend->disconnect = connection->disconnect;
    backend->registered = ROTMOS_WRITABLE;
    channel->backend = backend;
    channel->ops.read = read_channel;
    channel->ops.write = write_channel;
    channel->ops.shutdown = shutdown_channel;
    channel->ops.interest = interest;
    channel->ops.pump = pump;
    channel->ops.release = release;
    channel->ops.completed = body_completed;
    request->channel = channel;
    rotmos_kore_body_transferred(request);
    kore_free(connection->http_pipeline);
    connection->http_pipeline = NULL;
    connection->http_pipeline_len = 0;
    connection->hdlr_extra = channel;
    connection->evt.handle = event;
    connection->disconnect = channel_disconnected;
    connection->http_timeout = 0;
    *out = channel;
    rotmos_channel_schedule(channel);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
