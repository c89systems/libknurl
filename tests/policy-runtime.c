#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
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
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "policy %d: %s\n", __LINE__, #t);                  \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#define VERIFY(f, t)                                                           \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "policy callback %d: %s\n", __LINE__, #t);         \
            (f)->failed = 1;                                                   \
        }                                                                      \
    } while (0)
struct fixture {
    pid_t child;
    pthread_t owner;
    char reject_file[32];
    int mode, failed, starts, closes, completed, committed, accepted, malformed,
        stalls;
    int rejected, connection_rejected, named, deny_next, start_failed,
        denied_without_input;
    int upload_open, upload_closed, upload_count, replaced, aborted,
        before_complete, plan_closed;
};
struct aborting {
    struct fixture *f;
    rotmos_response *response;
    rotmos_timer *timer;
};
struct raw_state {
    struct fixture *f;
};
static const char long_event[] =
    "application.named.events.retain.vectis.names.without.an.arbitrary."
    "sixty.three.byte.limit";
static int is(rotmos_request *request, const char *path)
{
    rotmos_bytes value = rotmos_request_path(request);
    return value.data != NULL && !strcmp(value.data, path);
}
static int same(rotmos_bytes value, const char *text)
{
    return value.length == strlen(text) &&
           (!value.length || !memcmp(value.data, text, value.length));
}
static rotmos_status correlation(rotmos_request *request, char *out,
                                 size_t capacity, size_t *length, void *data,
                                 rotmos_error *error)
{
    rotmos_bytes value;
    (void)data;
    (void)error;
    *length = 0;
    if (is(request, "/bad-id")) {
        out[0] = '\n';
        *length = 1;
        return ROTMOS_OK;
    }
    if (rotmos_request_header_first(request, "x-id", &value, NULL) ==
        ROTMOS_OK) {
        if (value.length >= capacity)
            return ROTMOS_LIMIT;
        memcpy(out, value.data, value.length);
        *length = value.length;
    }
    return ROTMOS_OK;
}
static rotmos_status resolve(rotmos_request *request, char *out,
                             size_t capacity, size_t *length, void *data,
                             rotmos_error *error)
{
    rotmos_bytes value;
    (void)data;
    (void)error;
    if (rotmos_request_header_first(request, "x-effective", &value, NULL) !=
        ROTMOS_OK)
        value = rotmos_request_effective_address(request);
    if (value.length >= capacity)
        return ROTMOS_LIMIT;
    memcpy(out, value.data, value.length);
    *length = value.length;
    return ROTMOS_OK;
}
static rotmos_status start_request(rotmos_server *server,
                                   rotmos_request *request, void *data,
                                   rotmos_error *error)
{
    struct fixture *f = data;
    (void)server;
    (void)error;
    ++f->starts;
    VERIFY(f, pthread_equal(f->owner, pthread_self()) &&
                  same(rotmos_request_peer(request).address, "127.0.0.1"));
    VERIFY(f, rotmos_request_effective_address(request).length != 0);
    VERIFY(f, rotmos_response_status(rotmos_request_response(request), 200,
                                     NULL) == ROTMOS_STATE);
    rotmos_request_set_userdata(request, f);
    if (is(request, "/start-fail")) {
        ++f->start_failed;
        return ROTMOS_IO;
    }
    return ROTMOS_OK;
}
static void end_request(rotmos_server *server, rotmos_request *request,
                        const rotmos_request_outcome *outcome, void *data)
{
    struct fixture *f = data;
    (void)server;
    (void)outcome;
    ++f->closes;
    VERIFY(f, pthread_equal(f->owner, pthread_self()) &&
                  f->before_complete == 1 && f->completed == f->closes);
    VERIFY(f, rotmos_request_path(request).length > 0 &&
                  rotmos_request_event(request, "late", NULL) == ROTMOS_STATE);
    if (is(request, "/bad-resolve") || is(request, "/bad-id"))
        VERIFY(f, rotmos_request_userdata(request) == NULL &&
                      same(rotmos_request_effective_address(request),
                           "127.0.0.1"));
    else
        VERIFY(f, rotmos_request_userdata(request) == f);
    f->before_complete = 0;
}
static void observe(rotmos_server *server, const rotmos_observation *event,
                    void *data)
{
    struct fixture *f = data;
    rotmos_response *response;
    (void)server;
    VERIFY(f, pthread_equal(f->owner, pthread_self()) &&
                  event->connection.process.role == ROTMOS_PROCESS_HTTP);
    VERIFY(f, same(event->connection.peer.address, "127.0.0.1") &&
                  event->connection.listener_index == 0 &&
                  !event->connection.tls);
    if (event->request == NULL) {
        VERIFY(f, event->effective_address.length == 0 &&
                      event->correlation_id.length == 0 && !event->has_outcome);
        if (event->kind == ROTMOS_OBSERVE_CONNECTION_ACCEPTED)
            ++f->accepted;
        if (event->kind == ROTMOS_OBSERVE_REQUEST_REJECTED)
            ++f->connection_rejected;
        if (event->kind == ROTMOS_OBSERVE_HTTP_STATUS && event->status == 400)
            ++f->malformed;
        if (event->kind == ROTMOS_OBSERVE_TCP_STALL) {
            ++f->stalls;
            VERIFY(f, event->cause == ROTMOS_OK);
        }
        return;
    }
    response = rotmos_request_response(event->request);
    VERIFY(f, same(event->effective_address,
                   rotmos_request_effective_address(event->request).data));
    VERIFY(f, rotmos_request_event(event->request, "recursive", NULL) ==
                  ROTMOS_STATE);
    VERIFY(f, rotmos_response_status(response, 200, NULL) == ROTMOS_STATE);
    if (event->kind == ROTMOS_OBSERVE_REQUEST_STARTED)
        VERIFY(f,
               event->status == 0 && !event->committed && !event->has_outcome);
    if (event->kind == ROTMOS_OBSERVE_REQUEST_REJECTED)
        ++f->rejected;
    if (event->kind == ROTMOS_OBSERVE_APPLICATION) {
        ++f->named;
        VERIFY(f,
               same(event->name,
                    is(event->request, "/long-event") ? long_event : "custom"));
    }
    if (event->kind == ROTMOS_OBSERVE_RESPONSE_COMMITTED) {
        ++f->committed;
        VERIFY(f, event->committed && !event->has_outcome);
        if (is(event->request, "/replace"))
            VERIFY(f, event->status == 502);
    }
    if (event->kind == ROTMOS_OBSERVE_REQUEST_COMPLETE) {
        ++f->completed;
        f->before_complete = 1;
        VERIFY(f, event->has_outcome &&
                      event->outcome.response.status == event->status);
        if (is(event->request, "/upload") && event->status == 429) {
            VERIFY(f, !event->input_counts_known &&
                          !event->outcome.body_complete && f->upload_open == 0);
            ++f->denied_without_input;
        }
        if (is(event->request, "/upload") && event->status == 200)
            VERIFY(f, event->input_counts_known &&
                          event->outcome.received_body_bytes.low == 3 &&
                          event->outcome.delivered_body_bytes.low == 3 &&
                          f->upload_closed == 1);
        if (is(event->request, "/replace")) {
            ++f->replaced;
            VERIFY(f, event->committed &&
                          event->outcome.response.body_bytes.low == 11);
        }
        if (is(event->request, "/abort")) {
            ++f->aborted;
            VERIFY(f, event->committed && event->outcome.cause == ROTMOS_IO &&
                          event->outcome.response.body_bytes.low == 4);
        }
        if (is(event->request, "/close"))
            VERIFY(f, !event->committed &&
                          event->outcome.response.wire_bytes.low == 0 &&
                          event->disconnect_side == ROTMOS_DISCONNECT_HOST);
        if (is(event->request, "/interim"))
            VERIFY(f, event->committed &&
                          event->outcome.response.interim_wire_bytes.low != 0 &&
                          event->outcome.response.body_bytes.low == 2);
    }
}
static rotmos_status admit_connection(rotmos_server *server,
                                      const rotmos_connection_info *info,
                                      rotmos_admission_action *action,
                                      void *data, rotmos_error *error)
{
    struct fixture *f = data;
    (void)server;
    (void)error;
    VERIFY(f, same(info->peer.address, "127.0.0.1") && info->peer.port != 0 &&
                  info->local.port != 0 && info->listener_index == 0);
    if (f->deny_next) {
        f->deny_next = 0;
        *action = ROTMOS_ADMISSION_CLOSE;
    }
    return ROTMOS_OK;
}
static rotmos_status admit_request(rotmos_request *request, rotmos_reply *reply,
                                   rotmos_admission_action *action, void *data,
                                   rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_bytes value;
    VERIFY(f, rotmos_request_userdata(request) == f &&
                  pthread_equal(f->owner, pthread_self()));
    VERIFY(f, rotmos_response_status(rotmos_request_response(request), 200,
                                     NULL) == ROTMOS_STATE);
    if (is(request, "/admit-error"))
        return ROTMOS_IO;
    if (is(request, "/admit-invalid")) {
        *action = (rotmos_admission_action)99;
        return ROTMOS_OK;
    }
    if (is(request, "/admit-empty"))
        *action = ROTMOS_ADMISSION_REJECT;
    if (is(request, "/admit-file")) {
        *action = ROTMOS_ADMISSION_REJECT;
        return rotmos_reply_file(reply, 403, "text/plain", f->reject_file,
                                 error);
    }
    if (rotmos_request_header_first(request, "x-deny", &value, NULL) ==
        ROTMOS_OK) {
        *action = ROTMOS_ADMISSION_REJECT;
        return rotmos_reply_text(reply, 429, "text/plain", "blocked", error);
    }
    if (is(request, "/close"))
        *action = ROTMOS_ADMISSION_CLOSE;
    return ROTMOS_OK;
}
static void abort_ready(rotmos_timer *timer, void *data)
{
    struct aborting *s = data;
    if (!rotmos_response_is_committed(s->response) ||
        rotmos_response_queued_bytes(s->response) != 0)
        return;
    rotmos_timer_cancel(timer);
    s->timer = NULL;
    VERIFY(s->f,
           rotmos_response_abort(s->response, ROTMOS_IO, NULL) == ROTMOS_OK);
}
static void abort_closed(rotmos_response *response,
                         const rotmos_response_outcome *out, void *data)
{
    struct aborting *s = data;
    (void)response;
    (void)out;
    if (s->timer != NULL)
        rotmos_timer_cancel(s->timer);
    free(s);
}
static rotmos_status handler(rotmos_request *request, rotmos_reply *reply,
                             void *data, rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_bytes address, id, body;
    char text[192];
    rotmos_response *response = rotmos_request_response(request);
    size_t accepted;
    VERIFY(f, rotmos_request_userdata(request) == f);
    if (is(request, "/stop")) {
        (void)rotmos_server_request_stop(rotmos_request_server(request), NULL);
        return rotmos_reply_text(reply, 200, "text/plain",
                                 f->failed ? "failed" : "stopped", error);
    }
    if (is(request, "/deny-connection")) {
        f->deny_next = 1;
        return rotmos_reply_text(reply, 200, "text/plain", "next", error);
    }
    if (is(request, "/long-event")) {
        VERIFY(f,
               rotmos_request_event(request, long_event, error) == ROTMOS_OK);
        return rotmos_reply_text(reply, 200, "text/plain", "named", error);
    }
    if (is(request, "/replace")) {
        if (rotmos_response_begin(response, error) != ROTMOS_OK ||
            rotmos_response_replace_uncommitted(response, error) != ROTMOS_OK)
            return ROTMOS_STATE;
        body.data = "replacement";
        body.length = 11;
        return rotmos_response_bytes(response, 502, "text/plain", body, error);
    }
    if (is(request, "/abort")) {
        struct aborting *s = calloc(1, sizeof(*s));
        if (s == NULL)
            return ROTMOS_NOMEM;
        s->f = f;
        s->response = response;
        if (rotmos_response_callbacks(response, NULL, abort_closed, s, error) !=
            ROTMOS_OK) {
            free(s);
            return ROTMOS_STATE;
        }
        if (rotmos_loop_timer(rotmos_request_loop(request), 1, 1, abort_ready,
                              NULL, s, &s->timer, error) != ROTMOS_OK)
            return ROTMOS_STATE;
        if (rotmos_response_begin(response, error) != ROTMOS_OK)
            return ROTMOS_STATE;
        return rotmos_response_write(response, "part", 4, &accepted, error);
    }
    if (is(request, "/interim")) {
        if (rotmos_response_interim(response, 103, NULL, 0, error) != ROTMOS_OK)
            return ROTMOS_STATE;
        return rotmos_reply_text(reply, 200, "text/plain", "ok", error);
    }
    VERIFY(f, rotmos_request_event(request, "custom", error) == ROTMOS_OK);
    address = rotmos_request_effective_address(request);
    id = rotmos_request_correlation_id(request);
    snprintf(text, sizeof(text), "%.*s|%.*s", (int)address.length,
             (const char *)address.data, (int)id.length,
             id.data != NULL ? (const char *)id.data : "");
    return rotmos_reply_text(reply, 200, "text/plain", text, error);
}
static rotmos_status upload_open(rotmos_request *request, void *data,
                                 void **state, rotmos_error *error)
{
    struct fixture *f = data;
    (void)error;
    VERIFY(f, rotmos_request_userdata(request) == f);
    ++f->upload_open;
    *state = f;
    return ROTMOS_OK;
}
static rotmos_status upload_write(rotmos_request *request, const void *bytes,
                                  size_t length, size_t *count, void *state,
                                  void *data, rotmos_error *error)
{
    struct fixture *f = data;
    (void)request;
    (void)bytes;
    (void)state;
    (void)error;
    f->upload_count += (int)length;
    *count = length;
    return ROTMOS_OK;
}
static rotmos_status upload_finish(rotmos_request *request, rotmos_reply *reply,
                                   void *state, void *data, rotmos_error *error)
{
    struct fixture *f = data;
    (void)request;
    (void)state;
    VERIFY(f, f->upload_count == 3);
    return rotmos_reply_text(reply, 200, "text/plain", "uploaded", error);
}
static void upload_close(rotmos_request *request, rotmos_status outcome,
                         void *state, void *data)
{
    struct fixture *f = data;
    (void)request;
    (void)state;
    VERIFY(f, outcome == ROTMOS_OK);
    ++f->upload_closed;
}
static rotmos_status raw_complete(rotmos_request *request, void *data,
                                  rotmos_error *error)
{
    struct raw_state *state = data;
    rotmos_bytes body = {"raw", 3};
    VERIFY(state->f, rotmos_request_userdata(request) == state->f);
    return rotmos_response_bytes(rotmos_request_response(request), 200,
                                 "text/plain", body, error);
}
static void raw_close(rotmos_request *request,
                      const rotmos_request_outcome *outcome, void *data)
{
    struct raw_state *state = data;
    (void)outcome;
    VERIFY(state->f, rotmos_request_userdata(request) == state->f);
    ++state->f->plan_closed;
    free(state);
}
static rotmos_status raw_headers(rotmos_request *request,
                                 rotmos_request_plan *plan, void *data,
                                 rotmos_error *error)
{
    struct raw_state *state = calloc(1, sizeof(*state));
    (void)error;
    if (state == NULL)
        return ROTMOS_NOMEM;
    state->f = data;
    plan->userdata = state;
    plan->on_close = raw_close;
    plan->on_body_end = raw_complete;
    return is(request, "/raw-fail") ? ROTMOS_IO : ROTMOS_OK;
}
static int connect_to(unsigned short port)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static int exchange(unsigned short port, const char *request, int expected,
                    const char *body)
{
    char bytes[4096], prefix[32], *value, *start;
    size_t used = 0;
    ssize_t n;
    int fd = connect_to(port);
    CHECK(fd >= 0);
    CHECK(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
    while (used < sizeof(bytes) - 1 &&
           (n = read(fd, bytes + used, sizeof(bytes) - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    bytes[used] = 0;
    if (expected == 0) {
        CHECK(used == 0);
        return 0;
    }
    start = bytes;
    if (!strncmp(start, "HTTP/1.1 103 ", 13)) {
        start = strstr(start, "\r\n\r\n");
        CHECK(start != NULL);
        start += 4;
    }
    snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d ", expected);
    if (strncmp(start, prefix, strlen(prefix)))
        fprintf(stderr, "received %s\n", bytes);
    CHECK(!strncmp(start, prefix, strlen(prefix)));
    value = strstr(start, "\r\n\r\n");
    CHECK(value != NULL);
    CHECK(!strcmp(value + 4, body));
    return 0;
}
#define GET(path, headers)                                                     \
    "GET " path                                                                \
    " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" headers "\r\n"
static int client(unsigned short port, int mode)
{
    char c;
    int fd;
    ssize_t n;
    CHECK(exchange(port, GET("/identity", ""), 200, "127.0.0.1|") == 0);
    CHECK(
        exchange(port,
                 GET("/identity", "X-Forwarded-For: 198.51.100.1, "
                                  "2001:0db8:0:0:0:0:0:2\r\nX-ID: id-123\r\n"),
                 200, mode ? "198.51.100.1|id-123" : "127.0.0.1|") == 0);
    CHECK(
        exchange(port,
                 GET("/identity", "X-Forwarded-For: 1.2.3.4, 198.51.100.8\r\n"),
                 200, mode ? "198.51.100.8|" : "127.0.0.1|") == 0);
    CHECK(
        exchange(port,
                 GET("/identity",
                     "X-Forwarded-For: invalid\r\nX-Real-IP: 2001:db8::9\r\n"),
                 200, mode ? "2001:db8::9|" : "127.0.0.1|") == 0);
    CHECK(exchange(
              port,
              GET("/identity", "X-Forwarded-For: ,\r\nX-Real-IP: garbage\r\n"),
              200, "127.0.0.1|") == 0);
    if (mode == 2) {
        CHECK(exchange(port, GET("/identity", "X-Effective: 203.0.113.9\r\n"),
                       200, "203.0.113.9|") == 0);
        CHECK(exchange(port, GET("/bad-resolve", "X-Effective: garbage\r\n"),
                       500, "") == 0);
        CHECK(exchange(port, GET("/bad-id", ""), 500, "") == 0);
    }
    CHECK(exchange(port, GET("/start-fail", ""), 500, "") == 0);
    CHECK(exchange(port, GET("/admit-error", ""), 500, "") == 0);
    CHECK(exchange(port, GET("/admit-invalid", ""), 500, "") == 0);
    CHECK(exchange(port, GET("/admit-empty", ""), 403, "") == 0);
    CHECK(exchange(port, GET("/admit-file", ""), 500, "") == 0);
    CHECK(exchange(port, GET("/raw", ""), 200, "raw") == 0);
    CHECK(exchange(port, GET("/raw-fail", ""), 500, "") == 0);
    CHECK(exchange(port, GET("/long-event", ""), 200, "named") == 0);
    CHECK(
        exchange(port,
                 "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                 "999999999\r\nX-Deny: yes\r\n\r\n",
                 429, "blocked") == 0);
    CHECK(
        exchange(port,
                 "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                 "3\r\nConnection: close\r\n\r\nabc",
                 200, "uploaded") == 0);
    CHECK(exchange(port, GET("/replace", ""), 502, "replacement") == 0);
    CHECK(exchange(port, GET("/interim", ""), 200, "ok") == 0);
    CHECK(exchange(port, GET("/abort", ""), 200, "4\r\npart\r\n") == 0);
    CHECK(exchange(port, GET("/close", ""), 0, "") == 0);
    CHECK(exchange(port,
                   "GET / HTTP/1.1\r\nHost: localhost\r\nHost: "
                   "bad\r\nConnection: close\r\n\r\n",
                   400, "") == 0);
    CHECK(exchange(port, GET("/deny-connection", ""), 200, "next") == 0);
    fd = connect_to(port);
    CHECK(fd >= 0);
    n = read(fd, &c, 1);
    CHECK(n == 0 || (n < 0 && errno == ECONNRESET));
    close(fd);
    if (mode == 0) {
        CHECK(exchange(port, "GET / HTTP/1.1\r\n", 408, "") == 0);
    }
    CHECK(exchange(port, GET("/stop", ""), 200, "stopped") == 0);
    return 0;
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process, void *data,
                           rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_bound_listener listener;
    int status;
    (void)process;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_STATE;
    f->child = fork();
    if (f->child < 0)
        return ROTMOS_IO;
    if (f->child == 0) {
        signal(SIGTERM, SIG_DFL);
        status = client(listener.port, f->mode);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    return ROTMOS_OK;
}
static rotmos_status started(rotmos_server *server,
                             const rotmos_process_context *process, void *data,
                             rotmos_error *error)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    (void)error;
    f->owner = pthread_self();
    return ROTMOS_OK;
}
static int exited(rotmos_server *server, const rotmos_process_context *process,
                  void *data)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    if (f->failed || f->closes != f->starts + (f->mode == 2 ? 2 : 0) ||
        f->completed != f->closes || f->committed != f->closes - 1 ||
        f->accepted < f->closes + 2 || f->malformed == 0 ||
        (f->mode == 0 && f->stalls == 0) || f->connection_rejected != 1 ||
        f->rejected != 3 || f->denied_without_input != 1 ||
        f->upload_open != 1 || f->upload_closed != 1 || f->replaced != 1 ||
        f->aborted != 1 || f->plan_closed != 2 ||
        f->named != (f->mode == 2 ? 7 : 6)) {
        fprintf(stderr,
                "policy mode=%d failed=%d starts=%d closes=%d completed=%d "
                "committed=%d accepts=%d malformed=%d stalls=%d reject=%d "
                "conn_reject=%d named=%d upload=%d/%d replace=%d abort=%d\n",
                f->mode, f->failed, f->starts, f->closes, f->completed,
                f->committed, f->accepted, f->malformed, f->stalls, f->rejected,
                f->connection_rejected, f->named, f->upload_open,
                f->upload_closed, f->replaced, f->aborted);
        return 1;
    }
    return 0;
}
static int run(int mode)
{
    struct fixture f;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_upload_stream_config upload;
    rotmos_route_config route;
    rotmos_server *server;
    rotmos_error error;
    rotmos_status status;
    rotmos_run_result result;
    int child, file;
    char proxy[46] = "127.0.0.1";
    const char *trusted[2];
    memset(&f, 0, sizeof(f));
    f.mode = mode;
    f.child = -1;
    trusted[0] = proxy;
    trusted[1] = "2001:db8::2";
    strcpy(f.reject_file, "rotmos-policy-XXXXXX");
    file = mkstemp(f.reject_file);
    CHECK(file >= 0 && write(file, "file", 4) == 4);
    CHECK(close(file) == 0);
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.http.backend_quiet = config.workers.affinity_disabled = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.timeouts.request_header.milliseconds = 1000;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_start = started;
    config.callbacks.on_worker_exit = exited;
    config.callbacks.on_request_start = start_request;
    config.callbacks.on_request_close = end_request;
    config.callbacks.on_observe = observe;
    config.callbacks.userdata = &f;
    config.admission.on_connection = admit_connection;
    config.admission.on_request = admit_request;
    config.admission.userdata = &f;
    if (mode != 0) {
        config.client_ip.trusted_proxies = trusted;
        config.client_ip.trusted_proxy_count = 2;
        config.correlation_id.resolve = correlation;
    }
    if (mode == 2)
        config.client_ip.resolve = resolve;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    strcpy(proxy, "192.0.2.3");
    rotmos_upload_stream_config_init(&upload);
    upload.path = "/upload";
    upload.method = ROTMOS_HTTP_POST;
    upload.body.max_bytes.low = 16;
    upload.open = upload_open;
    upload.write = upload_write;
    upload.finish = upload_finish;
    upload.close = upload_close;
    upload.userdata = &f;
    CHECK(rotmos_router_upload_stream(rotmos_server_router(server), &upload,
                                      NULL, &error) == ROTMOS_OK);
    rotmos_route_config_init(&route);
    route.path = "/raw";
    route.on_headers = raw_headers;
    route.userdata = &f;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    route.path = "/raw-fail";
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    rotmos_route_config_init(&route);
    route.path = "^/.*$";
    route.path_kind = ROTMOS_PATH_REGEX;
    route.handler = handler;
    route.userdata = &f;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    CHECK(rotmos_server_validate(server, &error) == ROTMOS_OK);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(waitpid(f.child, &child, 0) == f.child && WIFEXITED(child) &&
          WEXITSTATUS(child) == 0);
    CHECK(status == ROTMOS_OK && !result.escalated);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    CHECK(unlink(f.reject_file) == 0);
    return 0;
}
int main(void)
{
    CHECK(run(0) == 0);
    CHECK(run(1) == 0);
    CHECK(run(2) == 0);
    return 0;
}
