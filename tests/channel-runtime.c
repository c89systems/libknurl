#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef KNURL_CHANNEL_BACKEND_TEST
#include "channel-backend.h"
#endif

#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            (void)fprintf(stderr, "channel line %d: %s\n", __LINE__, #test);   \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#define OBSERVE(test)                                                          \
    do {                                                                       \
        if (!(test)) {                                                         \
            (void)fprintf(stderr, "channel callback %d: %s\n", __LINE__,       \
                          #test);                                              \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

struct fixture {
    pid_t client;
};
struct context {
    knurl_request *request;
    knurl_channel *channel;
    knurl_timer *timer;
    pthread_t owner;
    const char *path;
    size_t used;
    size_t target;
    unsigned char *tail;
    unsigned char pending[1024];
    size_t pending_length;
    size_t pending_offset;
    int events;
    int channel_closed;
    int eof;
    int write_closed;
};
static int failures;
static int opened;
static int requests_closed;
static int pauses;
static int blocked;
static int prior_closed;

static knurl_status text(knurl_request *request, int status, const char *value)
{
    knurl_bytes bytes;
    bytes.data = value;
    bytes.length = strlen(value);
    return knurl_response_bytes(knurl_request_response(request), status,
                                "text/plain", bytes, NULL);
}

static void request_closed(knurl_request *request,
                           const knurl_request_outcome *out, void *userdata)
{
    struct context *c;
    c = userdata;
    ++requests_closed;
    OBSERVE(pthread_equal(c->owner, pthread_self()));
    OBSERVE(knurl_request_target(request).length != 0);
#ifdef KNURL_CHANNEL_BACKEND_TEST
    if (strcmp(c->path, "/bad-upgrade") == 0)
        OBSERVE(knurl_test_prefix_finished());
#endif
    if (c->channel != NULL)
        OBSERVE(c->channel_closed == 1 &&
                knurl_channel_state(c->channel) == KNURL_CHANNEL_CLOSED);
    if (strcmp(c->path, "/raw") == 0 || strcmp(c->path, "/chunked") == 0 ||
        strcmp(c->path, "/backlog") == 0)
        OBSERVE(out->body_complete && out->body_cause == KNURL_OK);
    if (strcmp(c->path, "/tunnel") == 0 || strcmp(c->path, "/prefix") == 0)
        OBSERVE(out->cause == KNURL_OK && out->body_complete &&
                out->response.status == 101);
    if (strcmp(c->path, "/cancel") == 0 ||
        strcmp(c->path, "/bad-upgrade") == 0 ||
        strcmp(c->path, "/tail-limit") == 0 || strcmp(c->path, "/small") == 0)
        OBSERVE(!out->body_complete && out->body_cause == KNURL_CANCELLED);
    if (strcmp(c->path, "/idle") == 0)
        OBSERVE(out->cause == KNURL_TIMEOUT &&
                out->body_cause == KNURL_TIMEOUT && c->events == 0);
    if (c->timer != NULL)
        knurl_timer_cancel(c->timer);
    free(c->tail);
    free(c);
}

static void channel_closed(knurl_channel *channel,
                           const knurl_channel_outcome *out, void *userdata)
{
    struct context *c;
    size_t count;
    c = userdata;
    ++c->channel_closed;
#ifdef KNURL_CHANNEL_BACKEND_TEST
    if (strcmp(c->path, "/bad-upgrade") == 0) {
        OBSERVE(!knurl_test_prefix_finished());
        OBSERVE(knurl_test_prefix_signal());
    }
#endif
    OBSERVE(c->channel_closed == 1 && pthread_equal(c->owner, pthread_self()));
    OBSERVE(knurl_request_target(c->request).length != 0);
    OBSERVE(knurl_channel_read(channel, c->pending, sizeof(c->pending), &count,
                               NULL) == KNURL_STATE &&
            count == 0);
    if (strcmp(c->path, "/tunnel") == 0 || strcmp(c->path, "/prefix") == 0) {
        OBSERVE(out->cause == KNURL_OK && out->read_eof && out->write_shutdown);
        OBSERVE(out->received_bytes.high == 0 &&
                out->received_bytes.low == 65536UL &&
                out->sent_bytes.low == 65536UL &&
                out->delivered_bytes.low == 65536UL);
#ifdef KNURL_CHANNEL_BACKEND_TEST
        if (strcmp(c->path, "/prefix") == 0)
            OBSERVE(knurl_test_prefix_finished());
#endif
    }
}

static void resume(knurl_timer *timer, void *userdata)
{
    struct context *c;
    (void)timer;
    c = userdata;
    c->timer = NULL;
    OBSERVE(c->events == 1);
    ++pauses;
    OBSERVE(knurl_channel_interest(c->channel, KNURL_READABLE, NULL) ==
            KNURL_OK);
    OBSERVE(knurl_channel_interest(c->channel, KNURL_READABLE, NULL) ==
            KNURL_OK);
}

static void tunnel(knurl_channel *channel, unsigned int ready,
                   struct context *c)
{
    knurl_status status;
    size_t count;
    unsigned int budget;
    if (ready & KNURL_WRITE_CLOSED)
        c->write_closed = 1;
    budget = 32;
    while (budget-- != 0) {
        if (c->pending_offset < c->pending_length) {
            status = knurl_channel_write(
                channel, c->pending + c->pending_offset,
                c->pending_length - c->pending_offset, &count, NULL);
            if (status == KNURL_AGAIN) {
                OBSERVE(count == 0);
                ++blocked;
                OBSERVE(knurl_channel_interest(channel, KNURL_WRITABLE, NULL) ==
                        KNURL_OK);
                return;
            }
            if (status != KNURL_OK || count == 0) {
                ++failures;
                (void)knurl_channel_close(channel, KNURL_IO, NULL);
                return;
            }
            c->pending_offset += count;
            OBSERVE(knurl_channel_queued_bytes(channel) <= 64);
            continue;
        }
        if (c->eof) {
            OBSERVE(knurl_channel_shutdown_write(channel, NULL) == KNURL_OK);
            if (c->write_closed && knurl_channel_queued_bytes(channel) == 0) {
                OBSERVE(knurl_channel_close(channel, KNURL_OK, NULL) ==
                        KNURL_OK);
                OBSERVE(c->channel_closed == 0);
            } else {
                OBSERVE(knurl_channel_interest(channel, 0, NULL) == KNURL_OK);
            }
            return;
        }
        c->pending_offset = c->pending_length = 0;
        status = knurl_channel_read(channel, c->pending, sizeof(c->pending),
                                    &count, NULL);
        if (status == KNURL_AGAIN) {
            OBSERVE(count == 0);
            OBSERVE(knurl_channel_interest(channel, KNURL_READABLE, NULL) ==
                    KNURL_OK);
            return;
        }
        if (status == KNURL_EOF) {
            c->eof = 1;
            continue;
        }
        if (status != KNURL_OK || count == 0) {
            ++failures;
            (void)knurl_channel_close(channel, KNURL_IO, NULL);
            return;
        }
        c->pending_length = count;
    }
    OBSERVE(knurl_channel_interest(channel, KNURL_READABLE | KNURL_WRITABLE,
                                   NULL) == KNURL_OK);
}

static void events(knurl_channel *channel, unsigned int ready, void *userdata)
{
    static const char chunked[] = "3\r\nabc\r\n0\r\nX-End: yes\r\n\r\n";
    struct context *c;
    knurl_channel *other;
    knurl_channel_config config;
    knurl_status status;
    size_t count;
    size_t take;
    const char *expected;
    c = userdata;
    ++c->events;
    OBSERVE(pthread_equal(c->owner, pthread_self()));
    if (knurl_channel_state(channel) == KNURL_CHANNEL_TUNNEL) {
        tunnel(channel, ready, c);
        return;
    }
    knurl_channel_config_init(&config);
    config.on_event = events;
    OBSERVE(knurl_request_takeover(c->request, &config, &other, NULL) ==
                KNURL_STATE &&
            other == NULL);
    OBSERVE(knurl_channel_write(channel, "forbidden", 9, &count, NULL) ==
                KNURL_STATE &&
            count == 0);
    if (!(ready & KNURL_READABLE))
        return;
    if (strcmp(c->path, "/backlog") == 0) {
        while (c->used < c->target) {
            status = knurl_channel_read(channel, c->tail + c->used,
                                        c->target - c->used, &count, NULL);
            if (status == KNURL_AGAIN)
                return;
            OBSERVE(status == KNURL_OK && count > 0);
            if (status != KNURL_OK)
                return;
            c->used += count;
        }
        OBSERVE(knurl_channel_body_complete(channel, c->tail, c->used, NULL) ==
                KNURL_OK);
        OBSERVE(text(c->request, 200, "raw") == KNURL_OK);
        return;
    }
    if (strcmp(c->path, "/tail-limit") == 0) {
        status = knurl_channel_read(channel, c->pending, 3, &count, NULL);
        OBSERVE(status == KNURL_OK && count == 3);
        OBSERVE(knurl_channel_body_complete(channel, c->pending, count, NULL) ==
                KNURL_LIMIT);
        OBSERVE(knurl_channel_state(channel) == KNURL_CHANNEL_HTTP_INPUT);
        OBSERVE(knurl_channel_read(channel, c->pending, 2, &count, NULL) ==
                    KNURL_OK &&
                count == 2 && memcmp(c->pending, "DE", 2) == 0);
        OBSERVE(text(c->request, 413, "tail") == KNURL_OK);
        OBSERVE(knurl_channel_close(channel, KNURL_CANCELLED, NULL) ==
                KNURL_OK);
        return;
    }
    expected = strcmp(c->path, "/chunked") == 0 ? chunked : "abc";
    while (c->used < c->target) {
        status = knurl_channel_read(channel, c->pending, 2, &count, NULL);
        if (status == KNURL_AGAIN)
            return;
        if (status == KNURL_EOF) {
            OBSERVE(strcmp(c->path, "/cancel") == 0);
            OBSERVE(text(c->request, 400, "bad") == KNURL_OK);
            OBSERVE(knurl_channel_close(channel, KNURL_CANCELLED, NULL) ==
                    KNURL_OK);
            return;
        }
        OBSERVE(status == KNURL_OK && count > 0);
        if (status != KNURL_OK)
            return;
        take = c->target - c->used;
        if (take > count)
            take = count;
        OBSERVE(memcmp(c->pending, expected + c->used, take) == 0);
        c->used += take;
        if (c->used == c->target) {
            OBSERVE(knurl_channel_body_complete(channel, c->pending + take,
                                                count - take,
                                                NULL) == KNURL_OK);
            OBSERVE(knurl_channel_body_complete(channel, NULL, 0, NULL) ==
                    KNURL_STATE);
            OBSERVE(knurl_channel_read(channel, c->pending, sizeof(c->pending),
                                       &count, NULL) == KNURL_STATE &&
                    count == 0);
            OBSERVE(text(c->request, 200,
                         strcmp(c->path, "/chunked") == 0 ? "chunked"
                                                          : "raw") == KNURL_OK);
            OBSERVE(knurl_channel_close(channel, KNURL_OK, NULL) == KNURL_OK);
            OBSERVE(c->channel_closed == 0);
            return;
        }
        if (strcmp(c->path, "/raw") == 0 && c->events == 1) {
            OBSERVE(knurl_channel_interest(channel, 0, NULL) == KNURL_OK);
            OBSERVE(knurl_loop_timer(knurl_request_loop(c->request), 5, 0,
                                     resume, NULL, c, &c->timer,
                                     NULL) == KNURL_OK);
            return;
        }
    }
}

static void upgrade(knurl_timer *timer, void *userdata)
{
    struct context *c;
    char padding[1201];
    knurl_header fields[5];
    const char *names[] = {"Upgrade", "Connection", "Sec-WebSocket-Accept",
                           "Sec-WebSocket-Protocol", "Connection"};
    const char *values[] = {"websocket", "Upgrade",
                            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "chat",
                            "keep-alive"};
    size_t i;
    (void)timer;
    c = userdata;
    c->timer = NULL;
    OBSERVE(c->events == 0);
    for (i = 0; i < 5; ++i) {
        fields[i].name.data = names[i];
        fields[i].name.length = strlen(names[i]);
        fields[i].value.data = values[i];
        fields[i].value.length = strlen(values[i]);
    }
    fields[2].value.data = "invalid";
    fields[2].value.length = 7;
    OBSERVE(knurl_channel_upgrade(c->channel, fields, 5, NULL) ==
            KNURL_INVALID);
    OBSERVE(knurl_channel_state(c->channel) == KNURL_CHANNEL_HTTP_INPUT &&
            !knurl_response_is_committed(knurl_request_response(c->request)));
    fields[2].value.data = values[2];
    fields[2].value.length = strlen(values[2]);
    if (strcmp(c->path, "/bad-upgrade") == 0) {
        OBSERVE(knurl_response_header_set(knurl_request_response(c->request),
                                          "Sec-WebSocket-Accept", "invalid",
                                          NULL) == KNURL_OK);
        OBSERVE(knurl_channel_upgrade(c->channel, fields, 5, NULL) ==
                KNURL_INVALID);
        OBSERVE(
            knurl_response_state(knurl_request_response(c->request)) ==
                KNURL_RESPONSE_BUILDING &&
            !knurl_response_is_committed(knurl_request_response(c->request)));
        OBSERVE(knurl_response_replace_uncommitted(
                    knurl_request_response(c->request), NULL) == KNURL_OK);
        OBSERVE(text(c->request, 502, "bad") == KNURL_OK);
        OBSERVE(knurl_channel_close(c->channel, KNURL_CANCELLED, NULL) ==
                KNURL_OK);
        return;
    }
    OBSERVE(knurl_response_header_set(knurl_request_response(c->request),
                                      "X-Upgrade-Context", "retained",
                                      NULL) == KNURL_OK);
    memset(padding, 'Z', sizeof(padding) - 1);
    padding[sizeof(padding) - 1] = '\0';
    OBSERVE(knurl_response_header_set(knurl_request_response(c->request),
                                      "X-Upgrade-Queue", padding,
                                      NULL) == KNURL_OK);
    OBSERVE(knurl_channel_upgrade(c->channel, fields, 5, NULL) == KNURL_OK);
    OBSERVE(knurl_channel_upgrade(c->channel, fields, 5, NULL) == KNURL_STATE);
    OBSERVE(knurl_channel_interest(c->channel, KNURL_READABLE | KNURL_WRITABLE,
                                   NULL) == KNURL_OK);
}

static knurl_status complete(knurl_request *request, void *userdata,
                             knurl_error *error)
{
    struct context *c;
    (void)error;
    c = userdata;
    return text(request, 200,
                strcmp(c->path, "/stop") == 0 ? "stopped" : "pong");
}

static knurl_status headers(knurl_request *request, knurl_request_plan *plan,
                            void *userdata, knurl_error *error)
{
    struct context *c;
    knurl_channel_config config;
    knurl_channel *other;
    knurl_status status;
    knurl_bytes field;
    knurl_u64 size;
    (void)userdata;
    if (strcmp(knurl_request_path(request).data, "/large") == 0 ||
        strcmp(knurl_request_path(request).data, "/prior") == 0)
        return knurl_router_dispatch_headers(
            knurl_server_router(knurl_request_server(request)), request, plan,
            error);
    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return KNURL_NOMEM;
    c->request = request;
    c->owner = pthread_self();
    c->path = knurl_request_path(request).data;
    plan->userdata = c;
    plan->on_close = request_closed;
    ++opened;
    if (strcmp(c->path, "/stop") == 0 || strcmp(c->path, "/ping") == 0) {
        if (strcmp(c->path, "/stop") == 0) {
            OBSERVE(opened == requests_closed + 1 && pauses == 2 &&
                    blocked > 0 && prior_closed == 1);
            (void)knurl_server_request_stop(knurl_request_server(request),
                                            NULL);
            if (failures)
                return KNURL_INVALID;
        }
        plan->on_body_end = complete;
        return KNURL_OK;
    }
    knurl_channel_config_init(&config);
    config.on_event = events;
    config.on_close = channel_closed;
    config.userdata = c;
    config.interest = KNURL_READABLE;
    config.send_bytes = 64;
    if (strcmp(c->path, "/idle") == 0)
        config.interest = 0;
    config.tail_bytes = strcmp(c->path, "/tail-limit") == 0 ? 4 : 262144;
    if (strcmp(c->path, "/small") == 0) {
        config.receive_bytes = 1;
        OBSERVE(knurl_request_takeover(request, &config, &c->channel, error) ==
                    KNURL_LIMIT &&
                c->channel == NULL);
        plan->disposition = KNURL_REQUEST_REPLY;
        return text(request, 413, "small");
    }
    if (strcmp(c->path, "/backlog") == 0) {
        OBSERVE(knurl_request_header_first(request, "x-tail-length", &field,
                                           error) == KNURL_OK);
        OBSERVE(knurl_u64_from_decimal(field.data, field.length, &size,
                                       error) == KNURL_OK);
        OBSERVE(knurl_u64_to_size(size, &c->target, error) == KNURL_OK &&
                c->target <= 262144);
        c->tail = malloc(c->target);
        if (c->tail == NULL)
            return KNURL_NOMEM;
    } else {
        c->target = strcmp(c->path, "/chunked") == 0
                        ? strlen("3\r\nabc\r\n0\r\nX-End: yes\r\n\r\n")
                        : (strcmp(c->path, "/cancel") == 0 ? 8 : 3);
    }
    if (strcmp(c->path, "/tunnel") == 0 || strcmp(c->path, "/prefix") == 0 ||
        strcmp(c->path, "/bad-upgrade") == 0) {
        config.interest = 0;
#ifdef KNURL_CHANNEL_BACKEND_TEST
        if ((strcmp(c->path, "/prefix") == 0 ||
             strcmp(c->path, "/bad-upgrade") == 0) &&
            !knurl_test_prefix(request))
            return KNURL_INVALID;
#endif
    }
    status = knurl_request_takeover(request, &config, &c->channel, error);
    if (status != KNURL_OK)
        return status;
    OBSERVE(knurl_request_takeover(request, &config, &other, NULL) ==
                KNURL_STATE &&
            other == NULL);
    plan->disposition = KNURL_REQUEST_TAKEOVER;
    if (strcmp(c->path, "/tunnel") == 0 || strcmp(c->path, "/prefix") == 0 ||
        strcmp(c->path, "/bad-upgrade") == 0)
        return knurl_loop_timer(knurl_request_loop(request), 5, 0, upgrade,
                                NULL, c, &c->timer, error);
    return KNURL_OK;
}

static knurl_status large(knurl_request *request, knurl_reply *reply,
                          void *userdata, knurl_error *error)
{
    knurl_bytes bytes;
    (void)userdata;
    OBSERVE(knurl_request_body_bytes(request, &bytes, error) == KNURL_OK &&
            bytes.length == 131072);
    return knurl_reply_text(reply, 200, "text/plain", "large", error);
}

static knurl_status prior_read(void *buffer, size_t capacity, size_t *produced,
                               void *userdata, knurl_error *error)
{
    size_t *position;
    size_t count;
    position = userdata;
    count = 65536 - *position;
    if (count > capacity)
        count = capacity;
    memset(buffer, 'p', count);
    *position += count;
    *produced = count;
    knurl_error_clear(error);
    return count == 0 ? KNURL_EOF : KNURL_OK;
}

static void prior_done(void *userdata, knurl_status outcome)
{
    OBSERVE(outcome == KNURL_OK);
    ++prior_closed;
    free(userdata);
}

static knurl_status prior(knurl_request *request, knurl_reply *reply,
                          void *userdata, knurl_error *error)
{
    knurl_source_config config;
    knurl_source *source;
    knurl_status status;
    size_t *position;
    (void)request;
    (void)userdata;
    position = calloc(1, sizeof(*position));
    if (position == NULL)
        return KNURL_NOMEM;
    knurl_source_config_init(&config);
    config.read = prior_read;
    config.close = prior_done;
    config.userdata = position;
    config.has_length = 1;
    config.length.low = 65536;
    status = knurl_source_new(&config, &source, error);
    if (status != KNURL_OK) {
        free(position);
        return status;
    }
    status = knurl_reply_source_take(reply, 200, "application/octet-stream",
                                     source, error);
    if (status != KNURL_OK)
        (void)knurl_source_destroy(source, NULL);
    return status;
}

static int connect_to(unsigned short port, int small)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (small) {
        int receive_size;
        receive_size = 4096;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_size,
                       sizeof(receive_size)) != 0)
            return -1;
    }
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
        return -1;
    return fd;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *data;
    ssize_t count;
    data = buffer;
    while (length != 0) {
        count = write(fd, data, length);
        if (count == -1 && errno == EINTR)
            continue;
        CHECK(count > 0);
        data += (size_t)count;
        length -= (size_t)count;
    }
    return 0;
}

static int exchange(unsigned short port, const char *request,
                    const char *expected, int half)
{
    char response[2048];
    size_t used;
    ssize_t count;
    int fd;
#ifdef KNURL_CHANNEL_BACKEND_TEST
    fd = connect_to(port, strstr(request, "GET /bad-upgrade ") != NULL);
#else
    fd = connect_to(port, 0);
#endif
    CHECK(fd >= 0);
    CHECK(write_all(fd, request, strlen(request)) == 0);
#ifdef KNURL_CHANNEL_BACKEND_TEST
    if (strstr(request, "GET /bad-upgrade ") != NULL) {
        CHECK(knurl_test_prefix_wait());
        used = 0;
        while (used < 65536) {
            size_t wanted;
            size_t i;
            wanted = 65536 - used;
            if (wanted > sizeof(response))
                wanted = sizeof(response);
            count = read(fd, response, wanted);
            CHECK(count > 0);
            for (i = 0; i < (size_t)count; ++i)
                CHECK(response[i] == 'p');
            used += (size_t)count;
        }
    }
#endif
    if (half)
        CHECK(shutdown(fd, SHUT_WR) == 0);
    used = 0;
    while (used < sizeof(response) - 1) {
        count = read(fd, response + used, sizeof(response) - 1 - used);
        if (count == -1 && errno == EINTR)
            continue;
        CHECK(count >= 0);
        if (count == 0)
            break;
        used += (size_t)count;
    }
    response[used] = '\0';
    if (strstr(response, expected) == NULL) {
        (void)fprintf(stderr, "expected %s, received %s\n", expected, response);
        return 1;
    }
    CHECK(close(fd) == 0);
    return 0;
}

#define WS_FIELDS                                                              \
    "Host: localhost\r\nConnection: keep-alive\r\nConnection: "                \
    "Upgrade\r\nUpgrade: "                                                     \
    "websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "            \
    "dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: chat, other\r\n\r\n"

static int echo_tunnel(unsigned short port, const char *path, int prefix)
{
    char request[512];
    char response[2048];
    unsigned char data[1024];
    char *end;
    size_t used;
    size_t sent;
    size_t received;
    size_t i;
    ssize_t count;
    int fd;
    fd = connect_to(port, 0);
    CHECK(fd >= 0);
    (void)snprintf(
        request, sizeof(request), "%sGET %s HTTP/1.1\r\n" WS_FIELDS "abcde",
        prefix == 2 ? "GET /prior HTTP/1.1\r\nHost: localhost\r\n\r\n" : "",
        path);
    CHECK(write_all(fd, request, strlen(request)) == 0);
    if (prefix == 2) {
        used = 0;
        do {
            CHECK(read(fd, response + used, 1) == 1);
            response[++used] = '\0';
        } while (strstr(response, "\r\n\r\n") == NULL &&
                 used < sizeof(response) - 1);
        CHECK(strncmp(response, "HTTP/1.1 200 ", 13) == 0);
    }
    if (prefix == 2
#ifdef KNURL_CHANNEL_BACKEND_TEST
        || prefix == 1
#endif
    ) {
        received = 0;
        while (received < 65536) {
            size_t wanted;
            wanted = 65536 - received;
            if (wanted > sizeof(data))
                wanted = sizeof(data);
            count = read(fd, data, wanted);
            CHECK(count > 0 && (size_t)count <= 65536 - received);
            for (i = 0; i < (size_t)count; ++i)
                CHECK(data[i] == 'p');
            received += (size_t)count;
        }
    }
    used = 0;
    do {
        count = read(fd, response + used, sizeof(response) - used - 1);
        CHECK(count > 0);
        used += (size_t)count;
        response[used] = '\0';
    } while ((end = strstr(response, "\r\n\r\n")) == NULL);
    CHECK(strncmp(response, "HTTP/1.1 101 Switching Protocols", 31) == 0);
    CHECK(strstr(response, "content-length:") == NULL &&
          strstr(response, "transfer-encoding:") == NULL);
    CHECK(strstr(response, "Sec-WebSocket-Protocol: chat\r\n") != NULL);
    CHECK(strstr(response, "X-Upgrade-Context: retained\r\n") != NULL);
#ifdef KNURL_CHANNEL_BACKEND_TEST
    if (prefix == 1) {
        CHECK(strstr(response, "X-Escape-Context: retained\r\n") != NULL);
        CHECK(strstr(response, "set-cookie: escape=kept; Domain=localhost; "
                               "HttpOnly; Secure\r\n") != NULL);
    }
#endif
    end = strstr(response, "X-Upgrade-Queue: ");
    CHECK(end != NULL);
    end += strlen("X-Upgrade-Queue: ");
    for (i = 0; i < 1200; ++i)
        CHECK(end[i] == 'Z');
    CHECK(memcmp(end + 1200, "\r\n", 2) == 0);
    end = strstr(response, "\r\n\r\n");
    received = used - (size_t)(end + 4 - response);
    for (i = 0; i < received; ++i)
        CHECK((unsigned char)end[4 + i] == (unsigned char)('a' + i % 26));
    sent = 5;
    while (sent < 65536) {
        size_t length;
        length = 65536 - sent;
        if (length > sizeof(data))
            length = sizeof(data);
        for (i = 0; i < length; ++i)
            data[i] = (unsigned char)('a' + (sent + i) % 26);
        CHECK(write_all(fd, data, length) == 0);
        sent += length;
    }
    CHECK(shutdown(fd, SHUT_WR) == 0);
    while (received < 65536) {
        count = read(fd, data, sizeof(data));
        CHECK(count > 0 && (size_t)count <= 65536 - received);
        for (i = 0; i < (size_t)count; ++i)
            CHECK(data[i] == (unsigned char)('a' + (received + i) % 26));
        received += (size_t)count;
    }
    CHECK(read(fd, data, sizeof(data)) == 0);
    CHECK(close(fd) == 0);
    return 0;
}

static int backlog(unsigned short port)
{
    const char *ping =
        "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char header[256];
    char *tail;
    char response[2048];
    size_t length;
    size_t total;
    size_t used;
    ssize_t count;
    int fd;
    (void)snprintf(header, sizeof(header),
                   "POST /large HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                   "131072\r\n\r\n");
    length = strlen(header);
    total = length + 131072 + strlen(ping);
    tail = malloc(total);
    CHECK(tail != NULL);
    memcpy(tail, header, length);
    memset(tail + length, 'x', 131072);
    memcpy(tail + length + 131072, ping, strlen(ping));
    fd = connect_to(port, 0);
    CHECK(fd >= 0);
    (void)snprintf(header, sizeof(header),
                   "GET /backlog HTTP/1.1\r\nHost: localhost\r\nX-Tail-Length: "
                   "%lu\r\n\r\n",
                   (unsigned long)total);
    CHECK(write_all(fd, header, strlen(header)) == 0 &&
          write_all(fd, tail, total) == 0);
    free(tail);
    used = 0;
    while (used < sizeof(response) - 1) {
        count = read(fd, response + used, sizeof(response) - 1 - used);
        CHECK(count >= 0);
        if (count == 0)
            break;
        used += (size_t)count;
    }
    response[used] = '\0';
    CHECK(strstr(response, "\r\n\r\nrawHTTP/1.1 200 ") != NULL);
    CHECK(strstr(response, "\r\n\r\nlargeHTTP/1.1 200 ") != NULL);
    CHECK(used >= 4 && strcmp(response + used - 4, "pong") == 0);
    CHECK(close(fd) == 0);
    return 0;
}

static int client(unsigned short port)
{
    const char *raw =
        "POST /raw HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
        "3\r\n\r\nabcGET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: "
        "close\r\n\r\n";
    CHECK(exchange(port, raw, "\r\n\r\nrawHTTP/1.1 200 ", 0) == 0);
    CHECK(exchange(port, raw, "\r\n\r\npong", 0) == 0);
    CHECK(exchange(port,
                   "POST /chunked HTTP/1.1\r\nHost: "
                   "localhost\r\nTransfer-Encoding: chunked\r\nConnection: "
                   "close\r\n\r\n3\r\nabc\r\n0\r\nX-End: yes\r\n\r\n",
                   "\r\n\r\nchunked", 0) == 0);
    CHECK(exchange(port,
                   "GET /tail-limit HTTP/1.1\r\nHost: localhost\r\n\r\nABCDE",
                   "HTTP/1.1 413 ", 0) == 0);
    CHECK(exchange(port,
                   "POST /small HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                   "3\r\n\r\nabc",
                   "HTTP/1.1 413 ", 0) == 0);
    CHECK(exchange(port,
                   "POST /cancel HTTP/1.1\r\nHost: "
                   "localhost\r\nContent-Length: 8\r\n\r\nabc",
                   "\r\n\r\nbad", 1) == 0);
    CHECK(exchange(port, "GET /bad-upgrade HTTP/1.1\r\n" WS_FIELDS "abc",
                   "HTTP/1.1 502 ", 0) == 0);
    CHECK(echo_tunnel(port, "/tunnel", 0) == 0);
    CHECK(echo_tunnel(port, "/prefix", 1) == 0);
    CHECK(echo_tunnel(port, "/tunnel", 2) == 0);
    CHECK(backlog(port) == 0);
    CHECK(exchange(port, "GET /idle HTTP/1.1\r\nHost: localhost\r\n\r\n",
                   "HTTP/1.1 500 ", 0) == 0);
    CHECK(exchange(port,
                   "GET /stop HTTP/1.1\r\nHost: localhost\r\nConnection: "
                   "close\r\n\r\n",
                   "\r\n\r\nstopped", 0) == 0);
    return 0;
}

static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *userdata,
                          knurl_error *error)
{
    struct fixture *fixture;
    knurl_bound_listener listener;
    int result;
    (void)process;
    fixture = userdata;
    if (knurl_server_listener_at(server, 0, &listener, error) != KNURL_OK)
        return KNURL_INVALID;
    if (knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return KNURL_STATE;
    fixture->client = fork();
    if (fixture->client == -1)
        return KNURL_IO;
    if (fixture->client == 0) {
        result = client(listener.port);
        if (result != 0)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    return KNURL_OK;
}

int main(void)
{
    struct fixture fixture;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config route;
    knurl_server *server;
    knurl_run_result result;
    knurl_error error;
    knurl_status status;
    int child_status;
    fixture.client = -1;
#ifdef KNURL_CHANNEL_BACKEND_TEST
    CHECK(knurl_test_prefix_init());
#endif
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.workers.shutdown_timeout_ms = 1000;
    config.dispatch.mode = KNURL_DISPATCH_CALLBACK;
    config.dispatch.on_headers = headers;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.io.chunk_bytes = 1024;
    config.io.output_queue_bytes = 1024;
    config.timeouts.connection_idle.milliseconds = 250;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.userdata = &fixture;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    knurl_route_config_init(&route);
    route.path = "/large";
    route.handler = large;
    knurl_body_policy_init_buffered(&route.body);
    route.body.max_bytes.low = route.body.memory_buffer_limit_bytes = 262144;
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    knurl_route_config_init(&route);
    route.path = "/prior";
    route.handler = prior;
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    status = knurl_server_run(server, &result, &error);
    if (status != KNURL_OK)
        (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(status == KNURL_OK && !result.escalated);
    CHECK(waitpid(fixture.client, &child_status, 0) == fixture.client);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
#ifdef KNURL_CHANNEL_BACKEND_TEST
    knurl_test_prefix_cleanup();
#endif
    return 0;
}
