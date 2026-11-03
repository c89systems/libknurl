#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <rotmos/kore.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            (void)fprintf(stderr, "line %d: %s\n", __LINE__, #test);           \
            return 1;                                                          \
        }                                                                      \
    } while (0)

struct fixture {
    rotmos_server_config config;
    pid_t client;
    int mode;
    int phase;
    int worker_phase;
    int notes[2];
};

static int source_closed;
static int source_read;

static rotmos_status unused_read(void *buffer, size_t capacity,
                                 size_t *produced, void *userdata,
                                 rotmos_error *error)
{
    (void)buffer;
    (void)capacity;
    (void)userdata;
    (void)error;
    ++source_read;
    *produced = 0;
    return ROTMOS_EOF;
}

static void source_close(void *userdata, rotmos_status outcome)
{
    rotmos_request *request;
    request = userdata;
    if (rotmos_request_target(request).length == 0 ||
        rotmos_response_state(rotmos_request_response(request)) >=
            ROTMOS_RESPONSE_COMPLETE)
        source_closed += 100;
    source_closed += outcome == ROTMOS_CANCELLED ? 1 : 100;
}

static int equal(rotmos_bytes value, const char *text)
{
    return value.length == strlen(text) &&
           (value.length == 0 || memcmp(value.data, text, value.length) == 0);
}

static rotmos_status handler(rotmos_request *request, rotmos_reply *reply,
                             void *userdata, rotmos_error *error)
{
    rotmos_bytes value;
    rotmos_header header;
    const char *kind;
    rotmos_source_config source_config;
    rotmos_source *source;
    kind = userdata;
    if (strcmp(kind, "chain") == 0) {
        rotmos_source_config_init(&source_config);
        source_config.read = unused_read;
        source_config.close = source_close;
        source_config.userdata = request;
        if (rotmos_source_new(&source_config, &source, error) != ROTMOS_OK)
            return ROTMOS_NOMEM;
        if (rotmos_reply_source_take(reply, 200, "text/plain", source, error) !=
            ROTMOS_OK) {
            (void)rotmos_source_destroy(source, NULL);
            return ROTMOS_INVALID;
        }
        return ROTMOS_OK;
    }
    if (strcmp(kind, "optional") == 0) {
        if (rotmos_request_path_param(request, "first", &value, error) !=
                ROTMOS_EOF ||
            rotmos_request_path_param(request, "last", &value, error) !=
                ROTMOS_OK ||
            !equal(value, "end"))
            return ROTMOS_INVALID;
        return rotmos_reply_text(reply, 200, "text/plain", kind, error);
    }
    if (strcmp(kind, "metadata") == 0) {
        if (rotmos_request_method(request) != ROTMOS_HTTP_GET ||
            !equal(rotmos_request_method_token(request), "gEt") ||
            !equal(rotmos_request_target(request),
                   "/items/a%2Bb?x=first&x=second&bare&empty=&=skip&&z=a+b") ||
            !equal(rotmos_request_raw_path(request), "/items/a%2Bb") ||
            !equal(rotmos_request_path(request), "/items/a+b") ||
            !equal(rotmos_request_authority(request), "localhost") ||
            rotmos_request_query_count(request) != 5 ||
            rotmos_request_header_at(request, 1, &header, error) != ROTMOS_OK ||
            !equal(header.name, "host") ||
            rotmos_request_header_first(request, "X-REPEAT", &value, error) !=
                ROTMOS_OK ||
            !equal(value, "first") ||
            rotmos_request_query_first(request, "x", &value, error) !=
                ROTMOS_OK ||
            !equal(value, "first") ||
            rotmos_request_query_first(request, "X", &value, error) !=
                ROTMOS_EOF ||
            rotmos_request_query_first(request, "empty", &value, error) !=
                ROTMOS_OK ||
            value.length != 0 ||
            rotmos_request_query_first(request, "z", &value, error) !=
                ROTMOS_OK ||
            !equal(value, "a b") ||
            rotmos_request_path_param(request, "id", &value, error) !=
                ROTMOS_OK ||
            !equal(value, "a+b") || rotmos_request_peer(request).port == 0 ||
            rotmos_request_local(request).port == 0 ||
            rotmos_request_is_tls(request) ||
            rotmos_kore_request(request) == NULL ||
            rotmos_server_get_state(rotmos_request_server(request)) !=
                ROTMOS_SERVER_SERVING)
            return ROTMOS_INVALID;
        rotmos_request_set_userdata(request, userdata);
        if (rotmos_request_userdata(request) != userdata)
            return ROTMOS_INVALID;
        return rotmos_reply_text(reply, 200, "text/plain", "metadata", error);
    }
    if (strcmp(kind, "echo") == 0) {
        if (rotmos_request_body_bytes(request, &value, error) != ROTMOS_OK)
            return ROTMOS_INVALID;
        return rotmos_reply_bytes(reply, 200, "application/octet-stream", value,
                                  error);
    }
    if (strcmp(kind, "failure") == 0) {
        (void)rotmos_reply_text(reply, 200, "text/plain", "must be discarded",
                                error);
        return ROTMOS_INVALID;
    }
    if (strcmp(kind, "empty") == 0)
        return ROTMOS_OK;
    if (strcmp(kind, "head") == 0) {
        if (rotmos_reply_header_add(reply, "content-length", "123", error) !=
            ROTMOS_OK)
            return ROTMOS_INVALID;
        return rotmos_reply_text(reply, 200, "text/plain", "hidden", error);
    }
    if (strcmp(kind, "stop") == 0) {
        if (source_closed != 1 || source_read != 0)
            return ROTMOS_INVALID;
        if (rotmos_server_request_stop(rotmos_request_server(request), error) !=
            ROTMOS_OK)
            return ROTMOS_INVALID;
        return rotmos_reply_text(reply, 200, "text/plain", "stopped", error);
    }
    return rotmos_reply_text(reply, 200, "text/plain", kind, error);
}

static rotmos_status before(rotmos_request *request, rotmos_reply *reply,
                            int *finish, void *userdata, rotmos_error *error)
{
    (void)request;
    (void)finish;
    (void)userdata;
    if (rotmos_reply_header_add(reply, "Set-Cookie", "one=1", error) !=
        ROTMOS_OK)
        return ROTMOS_INVALID;
    return rotmos_reply_header_add(reply, "Set-Cookie", "two=2", error);
}

static rotmos_status after(rotmos_request *request, rotmos_reply *reply,
                           int *finish, void *userdata, rotmos_error *error)
{
    (void)request;
    (void)finish;
    (void)userdata;
    if (rotmos_reply_payload_kind(reply) != ROTMOS_REPLY_SOURCE)
        return ROTMOS_INVALID;
    if (rotmos_reply_text(reply, 201, "text/plain", "after", error) !=
        ROTMOS_OK)
        return ROTMOS_INVALID;
    return source_closed == 0 && source_read == 0 ? ROTMOS_OK : ROTMOS_INVALID;
}

static int exchange(unsigned short port, const char *request, int expected,
                    const char *body, const char *field)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char response[4096];
    char status[32];
    size_t used;
    size_t sent;
    ssize_t count;
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
          0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    CHECK(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    address.sin_port = htons(port);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    sent = 0;
    while (sent < strlen(request)) {
        count = write(fd, request + sent, strlen(request) - sent);
        if (count < 0 && errno == EINTR)
            continue;
        CHECK(count > 0);
        sent += (size_t)count;
    }
    used = 0;
    while (used < sizeof(response) - 1) {
        count = read(fd, response + used, sizeof(response) - 1 - used);
        if (count < 0 && errno == EINTR)
            continue;
        CHECK(count >= 0);
        if (count == 0)
            break;
        used += (size_t)count;
    }
    CHECK(close(fd) == 0);
    response[used] = '\0';
    (void)snprintf(status, sizeof(status), "HTTP/1.1 %d ", expected);
    if (strncmp(response, status, strlen(status)) != 0) {
        (void)fprintf(stderr, "expected %d, received %s\n", expected, response);
        return 1;
    }
    CHECK(strstr(response, "\r\n\r\n") != NULL);
    CHECK(strcmp(strstr(response, "\r\n\r\n") + 4, body) == 0);
    if (field != NULL)
        CHECK(strstr(response, field) != NULL);
    return 0;
}

#define REQUEST(path)                                                          \
    "GET " path " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"

static int client(unsigned short port)
{
    CHECK(exchange(port,
                   "gEt /items/a%2Bb?x=first&x=second&bare&empty=&=skip&&z=a+b "
                   "HTTP/1.1\r\nX-First: 1\r\nHost: localhost\r\nX-Repeat: "
                   "first\r\nX-Repeat: second\r\nConnection: close\r\n\r\n",
                   200, "metadata", NULL) == 0);
    CHECK(exchange(port,
                   "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                   "7\r\nConnection: close\r\n\r\npayload",
                   200, "payload", NULL) == 0);
    CHECK(exchange(port, REQUEST("/empty"), 204, "", NULL) == 0);
    CHECK(exchange(port, REQUEST("/failure"), 400, "", NULL) == 0);
    CHECK(exchange(port, REQUEST("/absent"), 404, "", NULL) == 0);
    CHECK(exchange(port,
                   "HEAD /head HTTP/1.1\r\nHost: localhost\r\nConnection: "
                   "close\r\n\r\n",
                   200, "", "content-length: 123\r\n") == 0);
    CHECK(exchange(port, REQUEST("/group/result"), 201, "after",
                   "Set-Cookie: one=1\r\nSet-Cookie: two=2\r\n") == 0);
    CHECK(exchange(port, REQUEST("/optional/end"), 200, "optional", NULL) == 0);
    CHECK(exchange(port, REQUEST("/regex/123"), 200, "regex", NULL) == 0);
    CHECK(exchange(port,
                   "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                   "1\r\nContent-Length: 1\r\nConnection: close\r\n\r\nx",
                   400, "", NULL) == 0);
    CHECK(exchange(port,
                   "GET /empty HTTP/1.1\r\nHost: localhost\r\nHost: "
                   "localhost\r\nConnection: close\r\n\r\n",
                   400, "", NULL) == 0);
    CHECK(exchange(port, REQUEST("/stop"), 200, "stopped", NULL) == 0);
    return 0;
}

static rotmos_status process_start(rotmos_server *server,
                                   const rotmos_process_context *process,
                                   void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    (void)server;
    (void)error;
    fixture = userdata;
    if (process->role == ROTMOS_PROCESS_PARENT) {
        if (fixture->phase != 0)
            return ROTMOS_INVALID;
        fixture->phase = 1;
    }
    return ROTMOS_OK;
}

static rotmos_status prepare(rotmos_server *server,
                             const rotmos_process_context *process,
                             void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    (void)server;
    (void)process;
    (void)error;
    fixture = userdata;
    if (fixture->phase != 1)
        return ROTMOS_INVALID;
    fixture->phase = 2;
    if (fixture->mode == 3)
        return rotmos_server_request_stop(server, error);
    return ROTMOS_OK;
}

static void *thread_stop(void *userdata)
{
    rotmos_error error;
    rotmos_server *server;
    server = userdata;
    return rotmos_server_request_stop(server, &error) == ROTMOS_OK ? NULL
                                                                   : userdata;
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process,
                           void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    rotmos_bound_listener bound;
    rotmos_server *other;
    int result;
    pthread_t thread;
    void *thread_result;
    (void)process;
    fixture = userdata;
    if (fixture->phase != 2)
        return ROTMOS_INVALID;
    fixture->phase = 3;
    if (fixture->mode == 1)
        return ROTMOS_INVALID;
    if (fixture->mode == 2) {
        (void)raise(SIGTERM);
        (void)raise(SIGCHLD);
        return ROTMOS_OK;
    }
    if (fixture->mode == 4) {
        if (pthread_create(&thread, NULL, thread_stop, server) != 0)
            return ROTMOS_IO;
        if (pthread_join(thread, &thread_result) != 0 || thread_result != NULL)
            return ROTMOS_INVALID;
        return ROTMOS_OK;
    }
    if (rotmos_server_new(&fixture->config, &other, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    if (rotmos_server_run(other, NULL, error) != ROTMOS_CONFLICT)
        return ROTMOS_INVALID;
    if (rotmos_server_destroy(other, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    if (rotmos_server_listener_at(server, 0, &bound, error) != ROTMOS_OK ||
        bound.port == 0)
        return ROTMOS_INVALID;
    if (rotmos_server_set_serving(server, 1, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    fixture->client = fork();
    if (fixture->client == -1)
        return ROTMOS_IO;
    if (fixture->client == 0) {
        result = client(bound.port);
        if (result != 0)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    return ROTMOS_OK;
}

static rotmos_status stopping(rotmos_server *server,
                              const rotmos_process_context *process,
                              void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    (void)server;
    (void)process;
    (void)error;
    fixture = userdata;
    if (fixture->phase != 3 && !(fixture->mode == 3 && fixture->phase == 2))
        return ROTMOS_INVALID;
    fixture->phase = 4;
    return ROTMOS_OK;
}

static rotmos_status stopped(rotmos_server *server,
                             const rotmos_process_context *process,
                             void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    (void)server;
    (void)process;
    (void)error;
    fixture = userdata;
    if (fixture->phase != 4)
        return ROTMOS_INVALID;
    fixture->phase = 5;
    return ROTMOS_OK;
}

static rotmos_status worker_note(rotmos_server *server,
                                 const rotmos_process_context *process,
                                 void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    unsigned char note;
    (void)server;
    (void)process;
    (void)error;
    fixture = userdata;
    note = (unsigned char)++fixture->worker_phase;
    return write(fixture->notes[1], &note, 1) == 1 ? ROTMOS_OK : ROTMOS_IO;
}

static int worker_exit(rotmos_server *server,
                       const rotmos_process_context *process, void *userdata)
{
    return worker_note(server, process, userdata, NULL) == ROTMOS_OK ? 0 : 1;
}

int main(void)
{
    struct fixture fixture;
    rotmos_listener_config listener;
    rotmos_route_config route;
    rotmos_route_group_config group_config;
    rotmos_route_group *group;
    rotmos_server *server;
    rotmos_router *router;
    rotmos_run_result result;
    rotmos_error error;
    rotmos_status status;
    struct sigaction before_signal;
    struct sigaction after_signal;
    const char *paths[] = {"/items/:id",
                           "/echo",
                           "/empty",
                           "/failure",
                           "/head",
                           "/stop",
                           "/optional/:first?/:last",
                           "^/regex/[0-9]+$"};
    const char *kinds[] = {"metadata", "echo", "empty",    "failure",
                           "head",     "stop", "optional", "regex"};
    unsigned char notes[3];
    int child_status;
    int iteration;
    size_t i;
    memset(&fixture, 0, sizeof(fixture));
    CHECK(pipe(fixture.notes) == 0);
    rotmos_server_config_init(&fixture.config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    fixture.config.listeners = &listener;
    fixture.config.listener_count = 1;
    fixture.config.workers.count = 1;
    fixture.config.workers.affinity_disabled = 1;
    fixture.config.workers.shutdown_timeout_ms = 1000;
    fixture.config.http.backend_quiet = 1;
    fixture.config.start_paused = 1;
    fixture.config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    fixture.config.callbacks.on_process_start = process_start;
    fixture.config.callbacks.on_parent_prepare = prepare;
    fixture.config.callbacks.on_parent_ready = ready;
    fixture.config.callbacks.on_parent_stopping = stopping;
    fixture.config.callbacks.on_parent_stopped = stopped;
    fixture.config.callbacks.on_worker_start = worker_note;
    fixture.config.callbacks.on_worker_stopping = worker_note;
    fixture.config.callbacks.on_worker_exit = worker_exit;
    fixture.config.callbacks.userdata = &fixture;
    CHECK(rotmos_server_new(&fixture.config, &server, &error) == ROTMOS_OK);
    router = rotmos_server_router(server);
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        rotmos_route_config_init(&route);
        route.path = paths[i];
        route.path_kind = i == 7 ? ROTMOS_PATH_REGEX
                                 : ((i == 0 || i == 6) ? ROTMOS_PATH_PARAMS
                                                       : ROTMOS_PATH_LITERAL);
        route.handler = handler;
        route.userdata = (void *)kinds[i];
        if (i == 1) {
            rotmos_body_policy_init_buffered(&route.body);
            route.body.max_bytes.low = 1024;
        }
        CHECK(rotmos_router_route(router, &route, NULL, &error) == ROTMOS_OK);
    }
    rotmos_route_group_config_init(&group_config);
    group_config.prefix = "/group";
    group_config.hooks.before = before;
    group_config.hooks.after = after;
    CHECK(rotmos_router_group(router, &group_config, &group, &error) ==
          ROTMOS_OK);
    rotmos_route_config_init(&route);
    route.path = "/result";
    route.handler = handler;
    route.userdata = "chain";
    CHECK(rotmos_route_group_route(group, &route, NULL, &error) == ROTMOS_OK);
    CHECK(sigaction(SIGTERM, NULL, &before_signal) == 0);
    for (iteration = 0; iteration < 6; ++iteration) {
        fixture.phase = fixture.worker_phase = 0;
        fixture.client = -1;
        fixture.mode = iteration == 0 ? 1 : (iteration == 1 ? 2 : 0);
        if (iteration == 4)
            fixture.mode = 3;
        if (iteration == 5)
            fixture.mode = 4;
        status = rotmos_server_run(server, &result, &error);
        if (status != ROTMOS_OK && fixture.mode != 1)
            (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
        CHECK(status == (fixture.mode == 1 ? ROTMOS_INVALID : ROTMOS_OK));
        CHECK(fixture.phase == 5);
        CHECK(result.escalated == 0);
        if (fixture.mode != 3) {
            CHECK(read(fixture.notes[0], notes, sizeof(notes)) ==
                  (ssize_t)sizeof(notes));
            CHECK(notes[0] == 1 && notes[1] == 2 && notes[2] == 3);
        }
        CHECK(result.reason ==
              (fixture.mode == 1
                   ? ROTMOS_RUN_STARTUP_FAILURE
                   : (fixture.mode == 2 ? ROTMOS_RUN_TERMINATING_SIGNAL
                                        : ROTMOS_RUN_REQUESTED_STOP)));
        if (fixture.mode == 2)
            CHECK(result.signal_number == SIGTERM);
        if (fixture.client > 0) {
            CHECK(waitpid(fixture.client, &child_status, 0) == fixture.client);
            CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
        }
        CHECK(sigaction(SIGTERM, NULL, &after_signal) == 0);
        CHECK(before_signal.sa_handler == after_signal.sa_handler);
    }
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    CHECK(close(fixture.notes[0]) == 0);
    CHECK(close(fixture.notes[1]) == 0);
    return 0;
}
