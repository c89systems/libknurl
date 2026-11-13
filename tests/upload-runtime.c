#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "upload %d: %s\n", __LINE__, #t);                  \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#define VERIFY(f, t)                                                           \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "upload callback %d: %s\n", __LINE__, #t);         \
            __atomic_store_n(&(f)->failed, 1, __ATOMIC_RELEASE);               \
        }                                                                      \
    } while (0)
struct fixture {
    pid_t child;
    pthread_t owner;
    int failed, opened, closed, finished, aborted, resumed, after,
        source_closed;
    unsigned int native_started, native_done, slow_started, released,
        late_started, late_done;
    char directory[512], file[1024], partial[1024], missing[1024];
};
struct upload {
    struct fixture *f;
    knurl_request *request;
    knurl_timer *timer;
    size_t count;
    int calls;
};
static const char *name(knurl_request *request)
{
    knurl_bytes value;
    return knurl_request_path_param(request, "id", &value, NULL) == KNURL_OK
               ? value.data
               : "";
}
static knurl_status auth(knurl_request *request, knurl_reply *reply, int *stop,
                         void *data, knurl_error *error)
{
    struct fixture *f = data;
    VERIFY(f, pthread_equal(f->owner, pthread_self()));
    if (!strcmp(name(request), "deny")) {
        *stop = 1;
        return knurl_reply_text(reply, 403, "text/plain", "denied", error);
    }
    return KNURL_OK;
}
static knurl_status before(knurl_request *request, knurl_reply *reply,
                           int *stop, void *data, knurl_error *error)
{
    (void)stop;
    knurl_request_set_userdata(request, data);
    return knurl_reply_header_add(reply, "X-Before", "yes", error);
}
static knurl_status after(knurl_request *request, knurl_reply *reply, int *stop,
                          void *data, knurl_error *error)
{
    struct fixture *f = data;
    (void)stop;
    VERIFY(f, !strcmp(name(request), "no-reply") ||
                  knurl_reply_is_selected(reply));
    ++f->after;
    if (!strcmp(name(request), "replace"))
        return knurl_reply_text(reply, 202, "text/plain", "replaced", error);
    return KNURL_OK;
}
static void resume(knurl_timer *timer, void *data)
{
    struct upload *s = data;
    (void)timer;
    s->timer = NULL;
    VERIFY(s->f, s->calls == 1);
    ++s->f->resumed;
    VERIFY(s->f, knurl_request_body_resume(s->request, NULL) == KNURL_OK);
}
static knurl_status open_upload(knurl_request *request, void *data,
                                void **state, knurl_error *error)
{
    struct fixture *f = data;
    struct upload *s;
    (void)error;
    VERIFY(f, knurl_request_userdata(request) == f &&
                  pthread_equal(f->owner, pthread_self()));
    if (!strcmp(name(request), "open-fail"))
        return KNURL_IO;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return KNURL_NOMEM;
    s->f = f;
    s->request = request;
    *state = s;
    ++f->opened;
    return KNURL_OK;
}
static knurl_status write_upload(knurl_request *request, const void *data,
                                 size_t length, size_t *consumed, void *state,
                                 void *userdata, knurl_error *error)
{
    struct upload *s = state;
    const unsigned char *bytes = data;
    size_t i, n;
    (void)userdata;
    VERIFY(s->f,
           pthread_equal(s->f->owner, pthread_self()) && request == s->request);
    ++s->calls;
    *consumed = 0;
    if (!strcmp(name(request), "write-fail"))
        return KNURL_IO;
    if (!strcmp(name(request), "zero"))
        return KNURL_OK;
    if (s->calls == 1 && !strcmp(name(request), "paused")) {
        if (knurl_loop_timer(knurl_request_loop(request), 5, 0, resume, NULL, s,
                             &s->timer, error) != KNURL_OK)
            return KNURL_IO;
        return KNURL_AGAIN;
    }
    n = length > 3 ? 3 : length;
    for (i = 0; i < n; ++i)
        VERIFY(s->f, bytes[i] == 'a' + (s->count + i) % 26);
    s->count += n;
    *consumed = n;
    if (n < length)
        VERIFY(s->f, knurl_request_body_resume(request, NULL) == KNURL_OK);
    return KNURL_OK;
}
static knurl_status finish_upload(knurl_request *request, knurl_reply *reply,
                                  void *state, void *data, knurl_error *error)
{
    struct upload *s = state;
    struct fixture *f = data;
    char text[32];
    ++f->finished;
    VERIFY(f, s->request == request && pthread_equal(f->owner, pthread_self()));
    if (!strcmp(name(request), "finish-fail"))
        return KNURL_IO;
    if (!strcmp(name(request), "no-reply"))
        return KNURL_OK;
    snprintf(text, sizeof(text), "%lu", (unsigned long)s->count);
    return knurl_reply_text(reply, 201, "text/plain", text, error);
}
static void close_upload(knurl_request *request, knurl_status outcome,
                         void *state, void *data)
{
    struct fixture *f = data;
    struct upload *s = state;
    ++f->closed;
    VERIFY(f, s->request == request && knurl_request_path(request).length > 0 &&
                  knurl_request_userdata(request) == f &&
                  pthread_equal(f->owner, pthread_self()));
    if (!strcmp(name(request), "abort")) {
        VERIFY(f, outcome != KNURL_OK && s->count > 0 && s->count < 19);
        ++f->aborted;
    } else if (!strcmp(name(request), "write-fail") ||
               !strcmp(name(request), "finish-fail") ||
               !strcmp(name(request), "zero"))
        VERIFY(f, outcome != KNURL_OK);
    else
        VERIFY(f, outcome == KNURL_OK);
    if (s->timer != NULL)
        knurl_timer_cancel(s->timer);
    free(s);
}
static knurl_status stateless_write(knurl_request *request, const void *bytes,
                                    size_t length, size_t *consumed,
                                    void *state, void *data, knurl_error *error)
{
    (void)request;
    (void)bytes;
    (void)data;
    (void)error;
    (void)state;
    *consumed = length;
    return KNURL_OK;
}
static knurl_status stateless_finish(knurl_request *request, knurl_reply *reply,
                                     void *state, void *data,
                                     knurl_error *error)
{
    (void)request;
    (void)state;
    (void)data;
    return knurl_reply_text(reply, 200, "text/plain", "stateless", error);
}
struct output_source {
    struct fixture *f;
    int sent;
};
static knurl_status output_read(void *buffer, size_t capacity, size_t *produced,
                                void *data, knurl_error *error)
{
    struct output_source *s = data;
    (void)error;
    VERIFY(s->f, pthread_equal(s->f->owner, pthread_self()));
    *produced = 0;
    if (s->sent)
        return KNURL_EOF;
    VERIFY(s->f, capacity >= 3);
    memcpy(buffer, "out", 3);
    *produced = 3;
    s->sent = 1;
    return KNURL_OK;
}
static void output_closed(void *data, knurl_status outcome)
{
    struct output_source *s = data;
    VERIFY(s->f,
           outcome == KNURL_OK && pthread_equal(s->f->owner, pthread_self()));
    ++s->f->source_closed;
    free(s);
}
static knurl_status native_reader(const knurl_upload_metadata *m,
                                  knurl_source *reader, knurl_reply *reply,
                                  void *data, knurl_error *error)
{
    struct fixture *f = data;
    knurl_status status = KNURL_OK;
    const char *id;
    char bytes[2], text[32];
    size_t total = 0, count, i, duplicates = 0;
    int coding = 0;
    __atomic_add_fetch(&f->native_started, 1u, __ATOMIC_RELEASE);
    VERIFY(f, !pthread_equal(f->owner, pthread_self()));
    VERIFY(f, m->method == KNURL_HTTP_POST &&
                  !memcmp(m->method_token.data, "POST", 4));
    VERIFY(f, m->param_count == 1 && !strcmp(m->params[0].name.data, "id"));
    VERIFY(f, m->peer.address.length != 0 && m->local.address.length != 0 &&
                  m->listener_index == 0 && !m->tls);
    VERIFY(f, m->effective_address.length == m->peer.address.length &&
                  !memcmp(m->effective_address.data, m->peer.address.data,
                          m->peer.address.length) &&
                  m->correlation_id.length == 0);
    VERIFY(f, m->content_length.high == 0 && m->version.length == 8);
    id = m->params[0].value.data;
    for (i = 0; i < m->header_count; ++i) {
        if (!strcasecmp(m->headers[i].name.data, "X-Dup"))
            ++duplicates;
        if (!strcasecmp(m->headers[i].name.data, "Transfer-Encoding"))
            coding = 1;
    }
    VERIFY(f, m->has_content_length == !coding);
    VERIFY(f, duplicates == 2);
    VERIFY(f, knurl_source_destroy(reader, NULL) == KNURL_STATE &&
                  knurl_source_reset(reader, NULL) == KNURL_STATE);
    VERIFY(f, knurl_reply_source_take(reply, 200, "text/plain", reader, NULL) ==
                  KNURL_STATE);
    if (!strcmp(id, "slow")) {
        __atomic_add_fetch(&f->slow_started, 1u, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&f->released, __ATOMIC_ACQUIRE) &&
               !knurl_source_cancelled(reader))
            (void)poll(NULL, 0, 1);
    }
    if (!strcmp(id, "late")) {
        __atomic_add_fetch(&f->late_started, 1u, __ATOMIC_RELEASE);
        while (!knurl_source_cancelled(reader))
            (void)poll(NULL, 0, 1);
        /* The transport can already be destroyed; these are independent bytes.
         */
        VERIFY(f, !strcmp(m->path.data, "/api/reader/late") &&
                      m->header_count != 0 && !strcmp(id, "late"));
        __atomic_add_fetch(&f->late_done, 1u, __ATOMIC_RELEASE);
        status = KNURL_CANCELLED;
        goto done;
    }
    if (!strcmp(id, "error")) {
        status = KNURL_IO;
        goto done;
    }
    if (!strcmp(id, "no-read")) {
        status = knurl_reply_text(reply, 201, "text/plain", "early", error);
        goto done;
    }
    for (;;) {
        status = knurl_source_read(reader, bytes, sizeof(bytes), &count, error);
        if (status != KNURL_OK)
            break;
        for (i = 0; i < count; ++i)
            VERIFY(f, bytes[i] == (char)('a' + (total + i) % 26));
        total += count;
    }
    if (status == KNURL_EOF)
        status = KNURL_OK;
    if (!strcmp(id, "good")) {
        VERIFY(f, m->query_count == 2 && m->queries[0].value.length == 0 &&
                      !strcmp(m->queries[1].value.data, "b"));
    }
    if (status == KNURL_OK && !strcmp(id, "source")) {
        struct output_source *output = calloc(1, sizeof(*output));
        knurl_source_config config;
        knurl_source *source;
        if (output == NULL) {
            status = KNURL_NOMEM;
            goto done;
        }
        output->f = f;
        knurl_source_config_init(&config);
        config.read = output_read;
        config.close = output_closed;
        config.userdata = output;
        config.has_length = 1;
        config.length.low = 3;
        status = knurl_source_new(&config, &source, error);
        if (status == KNURL_OK) {
            status = knurl_reply_source_take(reply, 201, "text/plain", source,
                                             error);
            if (status != KNURL_OK)
                knurl_source_destroy(source, NULL);
        } else
            free(output);
    } else if (status == KNURL_OK && strcmp(id, "no-reply")) {
        snprintf(text, sizeof(text), "%lu", (unsigned long)total);
        status = knurl_reply_text(reply, 201, "text/plain", text, error);
    }
done:
    __atomic_add_fetch(&f->native_done, 1u, __ATOMIC_RELEASE);
    return status;
}
static knurl_status control(knurl_request *request, knurl_reply *reply,
                            void *data, knurl_error *error)
{
    struct fixture *f = data;
    const char *path = knurl_request_path(request).data;
    int ready = 0;
    if (!strcmp(path, "/release")) {
        __atomic_store_n(&f->released, 1u, __ATOMIC_RELEASE);
        ready = 1;
    } else if (!strcmp(path, "/slow-started"))
        ready = __atomic_load_n(&f->slow_started, __ATOMIC_ACQUIRE) >= 2;
    else if (!strcmp(path, "/late-started"))
        ready = __atomic_load_n(&f->late_started, __ATOMIC_ACQUIRE) >= 1;
    else if (!strcmp(path, "/late-started2"))
        ready = __atomic_load_n(&f->late_started, __ATOMIC_ACQUIRE) >= 2;
    else if (!strcmp(path, "/late-done"))
        ready = __atomic_load_n(&f->late_done, __ATOMIC_ACQUIRE) >= 1;
    return knurl_reply_text(reply, 200, "text/plain", ready ? "yes" : "no",
                            error);
}
static knurl_status stop_server(knurl_request *request, knurl_reply *reply,
                                void *data, knurl_error *error)
{
    struct fixture *f = data;
    (void)knurl_server_request_stop(knurl_request_server(request), NULL);
    return knurl_reply_text(
        reply, 200, "text/plain",
        __atomic_load_n(&f->failed, __ATOMIC_ACQUIRE) ? "failed" : "stopped",
        error);
}
static int send_request(unsigned short port, const char *path, int length,
                        const char *body)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    struct timeval timeout;
    char request[512];
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    if (length == -2)
        snprintf(request, sizeof(request),
                 "POST %s HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: "
                 "chunked\r\nX-Dup: one\r\nX-Dup: two\r\nConnection: "
                 "close\r\n\r\n%s",
                 path, body);
    else if (length < 0)
        snprintf(
            request, sizeof(request),
            "POST %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
            path);
    else
        snprintf(
            request, sizeof(request),
            "POST %s HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
            "%d\r\nX-Dup: one\r\nX-Dup: two\r\nConnection: close\r\n\r\n%s",
            path, length, body);
    if (write(fd, request, strlen(request)) != (ssize_t)strlen(request)) {
        close(fd);
        return -1;
    }
    return fd;
}
static int exchange(unsigned short port, const char *path, int length,
                    const char *body, int expected, const char *result,
                    int hook)
{
    int fd = send_request(port, path, length, body);
    ssize_t n;
    size_t used = 0;
    char response[2048], status[32], *payload;
    CHECK(fd >= 0);
    while (used < sizeof(response) - 1 &&
           (n = read(fd, response + used, sizeof(response) - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    response[used] = 0;
    snprintf(status, sizeof(status), "HTTP/1.1 %d ", expected);
    if (strncmp(response, status, strlen(status)) != 0)
        fprintf(stderr, "%s: %s\n", path, response);
    CHECK(strncmp(response, status, strlen(status)) == 0);
    payload = strstr(response, "\r\n\r\n");
    CHECK(payload != NULL);
    if (expected == 413 && result[0] == 0)
        result = "request body exceeds route limit";
    CHECK(strcmp(payload + 4, result) == 0);
    if (hook)
        CHECK(strstr(response, "X-Before: yes\r\n") != NULL ||
              strstr(response, "x-before: yes\r\n") != NULL);
    return 0;
}
static int await_control(unsigned short port, const char *path)
{
    char bytes[1024];
    size_t used;
    ssize_t n;
    int i, fd;
    for (i = 0; i < 200; ++i) {
        fd = send_request(port, path, 0, "");
        CHECK(fd >= 0);
        used = 0;
        while (used < sizeof(bytes) - 1 &&
               (n = read(fd, bytes + used, sizeof(bytes) - 1 - used)) > 0)
            used += (size_t)n;
        close(fd);
        bytes[used] = 0;
        if (strstr(bytes, "\r\n\r\nyes"))
            return 0;
        (void)poll(NULL, 0, 1);
    }
    CHECK(0);
    return 1;
}
static int queued_result(int fd)
{
    char bytes[2048], *body;
    size_t used = 0;
    ssize_t n;
    while (used < sizeof(bytes) - 1 &&
           (n = read(fd, bytes + used, sizeof(bytes) - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    bytes[used] = 0;
    CHECK(strncmp(bytes, "HTTP/1.1 201 ", 13) == 0);
    body = strstr(bytes, "\r\n\r\n");
    CHECK(body != NULL && !strcmp(body + 4, "19"));
    return 0;
}
static int file_bytes(const char *path, const char *expected)
{
    char bytes[64];
    FILE *file = fopen(path, "rb");
    size_t n;
    CHECK(file != NULL);
    n = fread(bytes, 1, sizeof(bytes) - 1, file);
    bytes[n] = 0;
    CHECK(!ferror(file) && fclose(file) == 0);
    CHECK(!strcmp(bytes, expected));
    return 0;
}
static int native_cases(unsigned short port, const struct fixture *f)
{
    int fd, queued[36], i;
    struct linger reset;
    struct stat info;
    CHECK(exchange(port, "/api/reader/good?q=&q=b", 19, "abcdefghijklmnopqrs",
                   201, "19", 1) == 0);
    CHECK(
        exchange(
            port, "/api/reader/good?q=&q=b", -2,
            "3\r\nabc\r\n10\r\ndefghijklmnopqrs\r\n0\r\nX-Trace: done\r\n\r\n",
            201, "19", 1) == 0);
    CHECK(exchange(port, "/api/reader/replace", 3, "abc", 202, "replaced", 1) ==
          0);
    CHECK(exchange(port, "/api/reader/empty", 0, "", 201, "0", 1) == 0);
    CHECK(exchange(port, "/api/reader/source", 3, "abc", 201, "out", 1) == 0);
    CHECK(exchange(port, "/api/reader/deny", 3, "abc", 403, "denied", 0) == 0);
    CHECK(exchange(port, "/api/reader/error", 19, "abcdefghijklmnopqrs", 500,
                   "", 0) == 0);
    CHECK(exchange(port, "/api/reader/no-read", 19, "abcdefghijklmnopqrs", 500,
                   "", 0) == 0);
    CHECK(exchange(port, "/api/reader/no-reply", 3, "abc", 204, "", 1) == 0);
    for (i = 0; i < 36; ++i) {
        queued[i] =
            send_request(port, "/api/reader/slow", 19, "abcdefghijklmnopqrs");
        CHECK(queued[i] >= 0);
    }
    CHECK(await_control(port, "/slow-started") == 0);
    CHECK(exchange(port, "/release", 0, "", 200, "yes", 0) == 0);
    for (i = 0; i < 36; ++i)
        CHECK(queued_result(queued[i]) == 0);
    CHECK(exchange(port, "/api/file", 33, "", 413, "", 0) == 0);
    CHECK(file_bytes(f->file, "sentinel") == 0);
    CHECK(exchange(port, "/api/file", -1, "", 411, "", 0) == 0);
    CHECK(file_bytes(f->file, "sentinel") == 0);
    CHECK(exchange(port, "/api/file", 19, "abcdefghijklmnopqrs", 200, "19",
                   1) == 0);
    CHECK(file_bytes(f->file, "abcdefghijklmnopqrs") == 0);
    CHECK(exchange(port, "/api/file", 0, "", 200, "0", 1) == 0);
    CHECK(file_bytes(f->file, "") == 0);
    CHECK(exchange(port, "/api/missing", 3, "abc", 500, "", 0) == 0);
    fd = send_request(port, "/api/partial", 19, "abc");
    CHECK(fd >= 0);
    for (i = 0; i < 200; ++i) {
        if (stat(f->partial, &info) == 0 && info.st_size == 3)
            break;
        (void)poll(NULL, 0, 1);
    }
    /* stdio may retain this short prefix until cancellation closes the file. */
    reset.l_onoff = 1;
    reset.l_linger = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    close(fd);
    fd = send_request(port, "/api/reader/late", 19, "abc");
    CHECK(fd >= 0);
    CHECK(await_control(port, "/late-started") == 0);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    close(fd);
    CHECK(await_control(port, "/late-done") == 0);
    for (i = 0; i < 200; ++i) {
        if (stat(f->partial, &info) == 0 && info.st_size == 3)
            break;
        (void)poll(NULL, 0, 1);
    }
    CHECK(file_bytes(f->partial, "abc") == 0);
    fd = send_request(port, "/api/reader/late", 19, "abc");
    CHECK(fd >= 0);
    CHECK(await_control(port, "/late-started2") == 0);
    CHECK(exchange(port, "/stop", 0, "", 200, "stopped", 0) == 0);
    close(fd);
    return 0;
}
static int client(unsigned short port, const struct fixture *f)
{
    int fd;
    struct linger reset;
    CHECK(exchange(port, "/api/upload/good", 19, "abcdefghijklmnopqrs", 201,
                   "19", 1) == 0);
    CHECK(exchange(port, "/api/upload/paused", 19, "abcdefghijklmnopqrs", 201,
                   "19", 1) == 0);
    CHECK(exchange(port, "/api/upload/replace", 3, "abc", 202, "replaced", 1) ==
          0);
    CHECK(exchange(port, "/api/upload/empty", 0, "", 201, "0", 1) == 0);
    CHECK(exchange(port, "/api/upload/deny", 3, "abc", 403, "denied", 0) == 0);
    CHECK(exchange(port, "/api/upload/good", 33, "", 413, "", 0) == 0);
    CHECK(exchange(port, "/api/upload/good", -1, "", 411, "", 0) == 0);
    CHECK(exchange(port, "/api/upload/open-fail", 3, "abc", 500, "", 0) == 0);
    CHECK(exchange(port, "/api/upload/write-fail", 3, "abc", 500, "", 0) == 0);
    CHECK(exchange(port, "/api/upload/finish-fail", 3, "abc", 500, "", 0) == 0);
    CHECK(exchange(port, "/api/upload/no-reply", 3, "abc", 204, "", 1) == 0);
    CHECK(exchange(port, "/api/upload/zero", 3, "abc", 500, "", 0) == 0);
    CHECK(exchange(port, "/stateless", 3, "abc", 200, "stateless", 0) == 0);
    fd = send_request(port, "/api/upload/abort", 19, "abc");
    CHECK(fd >= 0);
    (void)poll(NULL, 0, 10);
    reset.l_onoff = 1;
    reset.l_linger = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    close(fd);
    return native_cases(port, f);
}
static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *data,
                          knurl_error *error)
{
    struct fixture *f = data;
    knurl_bound_listener listener;
    int status;
    (void)process;
    if (knurl_server_listener_at(server, 0, &listener, error) != KNURL_OK)
        return KNURL_STATE;
    if (knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return KNURL_STATE;
    f->child = fork();
    if (f->child < 0)
        return KNURL_IO;
    if (f->child == 0) {
        signal(SIGTERM, SIG_DFL);
        status = client(listener.port, f);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    return KNURL_OK;
}
static knurl_status started(knurl_server *server,
                            const knurl_process_context *process, void *data,
                            knurl_error *error)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    (void)error;
    f->owner = pthread_self();
    return KNURL_OK;
}
static int exited(knurl_server *server, const knurl_process_context *process,
                  void *data)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    if (f->failed || f->opened != 9 || f->closed != f->opened ||
        f->finished != 6 || f->resumed != 1 || f->aborted != 1 ||
        f->after != 49 || f->source_closed != 1 ||
        __atomic_load_n(&f->native_started, __ATOMIC_ACQUIRE) != 46 ||
        __atomic_load_n(&f->native_done, __ATOMIC_ACQUIRE) != 46 ||
        __atomic_load_n(&f->late_done, __ATOMIC_ACQUIRE) != 2) {
        fprintf(stderr,
                "upload counts: failed=%d opened=%d closed=%d finished=%d "
                "resumed=%d aborted=%d after=%d\n",
                f->failed, f->opened, f->closed, f->finished, f->resumed,
                f->aborted, f->after);
        return 1;
    }
    return 0;
}
int main(void)
{
    struct fixture f;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_upload_stream_config upload;
    knurl_upload_reader_config reader;
    knurl_upload_file_config file;
    FILE *initial;
    knurl_route_group_config group_config;
    knurl_route_group *group;
    knurl_route_config stop;
    knurl_server *server;
    knurl_error error;
    knurl_run_result result;
    int child;
    knurl_status status;
    memset(&f, 0, sizeof(f));
    f.child = -1;
    strcpy(f.directory, "upload-fixture-XXXXXX");
    CHECK(mkdtemp(f.directory) != NULL);
    snprintf(f.file, sizeof(f.file), "%s/output", f.directory);
    snprintf(f.partial, sizeof(f.partial), "%s/partial", f.directory);
    snprintf(f.missing, sizeof(f.missing), "%s/missing/output", f.directory);
    initial = fopen(f.file, "wb");
    CHECK(initial != NULL);
    CHECK(fwrite("sentinel", 1, 8, initial) == 8 && fclose(initial) == 0);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.http.backend_quiet = config.workers.affinity_disabled = 1;
    config.io.chunk_bytes = 5;
    config.io.input_queue_bytes = 32;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_start = started;
    config.callbacks.on_worker_exit = exited;
    config.callbacks.userdata = &f;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    knurl_route_group_config_init(&group_config);
    group_config.prefix = "/api";
    group_config.hooks.before = before;
    group_config.hooks.after = after;
    group_config.hooks.userdata = &f;
    CHECK(knurl_router_group(knurl_server_router(server), &group_config, &group,
                             &error) == KNURL_OK);
    knurl_upload_stream_config_init(&upload);
    upload.method = KNURL_HTTP_POST;
    upload.path = "/upload/:id";
    upload.path_kind = KNURL_PATH_PARAMS;
    upload.body.max_bytes.low = 32;
    upload.open = open_upload;
    upload.write = write_upload;
    upload.finish = finish_upload;
    upload.close = close_upload;
    upload.hooks.auth = auth;
    upload.hooks.userdata = upload.userdata = &f;
    CHECK(knurl_route_group_upload_stream(group, &upload, NULL, &error) ==
          KNURL_OK);
    CHECK(knurl_route_group_upload_stream(group, &upload, NULL, &error) ==
          KNURL_CONFLICT);
    upload.path = "/bad";
    upload.write = NULL;
    CHECK(knurl_router_upload_stream(knurl_server_router(server), &upload, NULL,
                                     &error) == KNURL_INVALID);
    knurl_upload_stream_config_init(&upload);
    upload.path = "/stateless";
    upload.write = stateless_write;
    upload.finish = stateless_finish;
    CHECK(knurl_router_upload_stream(knurl_server_router(server), &upload, NULL,
                                     &error) == KNURL_OK);
    knurl_upload_reader_config_init(&reader);
    reader.path = "/reader/:id";
    reader.path_kind = KNURL_PATH_PARAMS;
    reader.method = KNURL_HTTP_POST;
    reader.body.max_bytes.low = 32;
    reader.buffer_bytes = 3;
    reader.hooks.auth = auth;
    reader.hooks.userdata = reader.userdata = &f;
    reader.handler = native_reader;
    CHECK(knurl_route_group_upload_reader(group, &reader, NULL, &error) ==
          KNURL_OK);
    CHECK(knurl_route_group_upload_reader(group, &reader, NULL, &error) ==
          KNURL_CONFLICT);
    reader.handler = NULL;
    CHECK(knurl_router_upload_reader(knurl_server_router(server), &reader, NULL,
                                     &error) == KNURL_INVALID);
    knurl_upload_file_config_init(&file);
    file.path = "/file";
    file.method = KNURL_HTTP_POST;
    file.file_path = f.file;
    file.body.max_bytes.low = 32;
    file.body.memory_buffer_limit_bytes = 3;
    CHECK(knurl_route_group_upload_file(group, &file, NULL, &error) ==
          KNURL_OK);
    file.path = "/partial";
    file.file_path = f.partial;
    CHECK(knurl_route_group_upload_file(group, &file, NULL, &error) ==
          KNURL_OK);
    file.path = "/missing";
    file.file_path = f.missing;
    CHECK(knurl_route_group_upload_file(group, &file, NULL, &error) ==
          KNURL_OK);
    file.file_path = "";
    CHECK(knurl_router_upload_file(knurl_server_router(server), &file, NULL,
                                   &error) == KNURL_INVALID);
    CHECK(f.opened == 0 && f.closed == 0);
    knurl_route_config_init(&stop);
    stop.path = "/stop";
    stop.handler = stop_server;
    stop.userdata = &f;
    CHECK(knurl_router_route(knurl_server_router(server), &stop, NULL,
                             &error) == KNURL_OK);
    stop.path =
        "^/(release|slow-started|late-started|late-started2|late-done)$";
    stop.path_kind = KNURL_PATH_REGEX;
    stop.handler = control;
    CHECK(knurl_router_route(knurl_server_router(server), &stop, NULL,
                             &error) == KNURL_OK);
    status = knurl_server_run(server, &result, &error);
    if (status != KNURL_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(waitpid(f.child, &child, 0) == f.child && WIFEXITED(child) &&
          WEXITSTATUS(child) == 0);
    CHECK(status == KNURL_OK && !result.escalated);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    CHECK(unlink(f.file) == 0 && unlink(f.partial) == 0 &&
          rmdir(f.directory) == 0);
    return 0;
}
