#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <knurl/kore.h>

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
    knurl_server_config config;
    pid_t client;
    int mode;
    int phase;
    int worker_phase;
    int notes[2];
};

static int source_closed;
static int source_read;

static knurl_status unused_read(void *buffer, size_t capacity, size_t *produced,
                                void *userdata, knurl_error *error)
{
    (void)buffer;
    (void)capacity;
    (void)userdata;
    (void)error;
    ++source_read;
    *produced = 0;
    return KNURL_EOF;
}

static void source_close(void *userdata, knurl_status outcome)
{
    knurl_request *request;
    request = userdata;
    if (knurl_request_target(request).length == 0 ||
        knurl_response_state(knurl_request_response(request)) >=
            KNURL_RESPONSE_COMPLETE)
        source_closed += 100;
    source_closed += outcome == KNURL_CANCELLED ? 1 : 100;
}

static int equal(knurl_bytes value, const char *text)
{
    return value.length == strlen(text) &&
           (value.length == 0 || memcmp(value.data, text, value.length) == 0);
}

static knurl_status handler(knurl_request *request, knurl_reply *reply,
                            void *userdata, knurl_error *error)
{
    knurl_bytes value;
    knurl_bytes first;
    knurl_bytes second;
    knurl_header header;
    const char *kind;
    knurl_source_config source_config;
    knurl_source *source;
    knurl_status first_status;
    knurl_status second_status;
    kind = userdata;
    if (strcmp(kind, "optionals") == 0) {
        first_status =
            knurl_request_path_param(request, "first", &first, error);
        second_status =
            knurl_request_path_param(request, "second", &second, error);
        if ((first_status != KNURL_OK && first_status != KNURL_EOF) ||
            (second_status != KNURL_OK && second_status != KNURL_EOF) ||
            (first_status == KNURL_OK && second_status != KNURL_OK) ||
            (first_status == KNURL_OK && !equal(first, "a")) ||
            (second_status == KNURL_OK && !equal(second, "b")))
            return KNURL_INVALID;
        return knurl_reply_text(reply, 200, "text/plain",
                                first_status == KNURL_OK    ? "a|b"
                                : second_status == KNURL_OK ? "_|b"
                                                            : "_|_",
                                error);
    }
    if (strcmp(kind, "chain") == 0) {
        knurl_source_config_init(&source_config);
        source_config.read = unused_read;
        source_config.close = source_close;
        source_config.userdata = request;
        if (knurl_source_new(&source_config, &source, error) != KNURL_OK)
            return KNURL_NOMEM;
        if (knurl_reply_source_take(reply, 200, "text/plain", source, error) !=
            KNURL_OK) {
            (void)knurl_source_destroy(source, NULL);
            return KNURL_INVALID;
        }
        return KNURL_OK;
    }
    if (strcmp(kind, "optional") == 0) {
        if (knurl_request_path_param(request, "first", &value, error) !=
                KNURL_EOF ||
            knurl_request_path_param(request, "last", &value, error) !=
                KNURL_OK ||
            !equal(value, "end"))
            return KNURL_INVALID;
        return knurl_reply_text(reply, 200, "text/plain", kind, error);
    }
    if (strcmp(kind, "metadata") == 0) {
        if (knurl_request_method(request) != KNURL_HTTP_GET ||
            !equal(knurl_request_method_token(request), "gEt") ||
            !equal(knurl_request_target(request),
                   "/items/a%2Bb?x=first&x=second&bare&empty=&=skip&&z=a+b") ||
            !equal(knurl_request_raw_path(request), "/items/a%2Bb") ||
            !equal(knurl_request_path(request), "/items/a+b") ||
            !equal(knurl_request_authority(request), "localhost") ||
            knurl_request_query_count(request) != 5 ||
            knurl_request_header_at(request, 1, &header, error) != KNURL_OK ||
            !equal(header.name, "host") ||
            knurl_request_header_first(request, "X-REPEAT", &value, error) !=
                KNURL_OK ||
            !equal(value, "first") ||
            knurl_request_query_first(request, "x", &value, error) !=
                KNURL_OK ||
            !equal(value, "first") ||
            knurl_request_query_first(request, "X", &value, error) !=
                KNURL_EOF ||
            knurl_request_query_first(request, "empty", &value, error) !=
                KNURL_OK ||
            value.length != 0 ||
            knurl_request_query_first(request, "z", &value, error) !=
                KNURL_OK ||
            !equal(value, "a b") ||
            knurl_request_path_param(request, "id", &value, error) !=
                KNURL_OK ||
            !equal(value, "a+b") || knurl_request_peer(request).port == 0 ||
            knurl_request_local(request).port == 0 ||
            knurl_request_is_tls(request) ||
            knurl_kore_request(request) == NULL ||
            knurl_server_get_state(knurl_request_server(request)) !=
                KNURL_SERVER_SERVING)
            return KNURL_INVALID;
        knurl_request_set_userdata(request, userdata);
        if (knurl_request_userdata(request) != userdata)
            return KNURL_INVALID;
        return knurl_reply_text(reply, 200, "text/plain", "metadata", error);
    }
    if (strcmp(kind, "echo") == 0) {
        if (knurl_request_body_bytes(request, &value, error) != KNURL_OK)
            return KNURL_INVALID;
        return knurl_reply_bytes(reply, 200, "application/octet-stream", value,
                                 error);
    }
    if (strcmp(kind, "failure") == 0) {
        (void)knurl_reply_text(reply, 200, "text/plain", "must be discarded",
                               error);
        return KNURL_INVALID;
    }
    if (strcmp(kind, "empty") == 0)
        return KNURL_OK;
    if (strcmp(kind, "head") == 0) {
        if (knurl_reply_header_add(reply, "content-length", "123", error) !=
            KNURL_OK)
            return KNURL_INVALID;
        return knurl_reply_text(reply, 200, "text/plain", "hidden", error);
    }
    if (strcmp(kind, "stop") == 0) {
        if (source_closed != 1 || source_read != 0)
            return KNURL_INVALID;
        if (knurl_server_request_stop(knurl_request_server(request), error) !=
            KNURL_OK)
            return KNURL_INVALID;
        return knurl_reply_text(reply, 200, "text/plain", "stopped", error);
    }
    return knurl_reply_text(reply, 200, "text/plain", kind, error);
}

static knurl_status before(knurl_request *request, knurl_reply *reply,
                           int *finish, void *userdata, knurl_error *error)
{
    (void)request;
    (void)finish;
    (void)userdata;
    if (knurl_reply_header_add(reply, "Set-Cookie", "one=1", error) != KNURL_OK)
        return KNURL_INVALID;
    return knurl_reply_header_add(reply, "Set-Cookie", "two=2", error);
}

static knurl_status after(knurl_request *request, knurl_reply *reply,
                          int *finish, void *userdata, knurl_error *error)
{
    (void)request;
    (void)finish;
    (void)userdata;
    if (knurl_reply_payload_kind(reply) != KNURL_REPLY_SOURCE)
        return KNURL_INVALID;
    if (knurl_reply_text(reply, 201, "text/plain", "after", error) != KNURL_OK)
        return KNURL_INVALID;
    return source_closed == 0 && source_read == 0 ? KNURL_OK : KNURL_INVALID;
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
                   200, "metadata", "server: fixture-http\r\n") == 0);
    CHECK(exchange(port,
                   "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                   "7\r\nConnection: close\r\n\r\npayload",
                   200, "payload", NULL) == 0);
    CHECK(exchange(port, REQUEST("/empty"), 204, "", NULL) == 0);
    CHECK(exchange(port,
                   "GET /empty HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                   "0\r\nConnection: close\r\n\r\n",
                   204, "", NULL) == 0);
    CHECK(exchange(port,
                   "POST /empty HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                   "1\r\nConnection: close\r\n\r\nx",
                   413, "request body is not allowed for this route",
                   NULL) == 0);
    CHECK(exchange(port,
                   "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                   "1025\r\nConnection: close\r\n\r\n",
                   413, "request body exceeds route limit", NULL) == 0);
    CHECK(exchange(port, REQUEST("/failure"), 400, "", NULL) == 0);
    CHECK(exchange(port, REQUEST("/absent"), 404, "", NULL) == 0);
    CHECK(exchange(port,
                   "HEAD /head HTTP/1.1\r\nHost: localhost\r\nConnection: "
                   "close\r\n\r\n",
                   200, "", "content-length: 123\r\n") == 0);
    CHECK(exchange(port, REQUEST("/group/result"), 201, "after",
                   "Set-Cookie: one=1\r\nSet-Cookie: two=2\r\n") == 0);
    CHECK(exchange(port, REQUEST("/optional/end"), 200, "optional", NULL) == 0);
    CHECK(exchange(port, REQUEST("/optionals"), 200, "_|_", NULL) == 0);
    CHECK(exchange(port, REQUEST("/optionals/b"), 200, "_|b", NULL) == 0);
    CHECK(exchange(port, REQUEST("/optionals/a/b"), 200, "a|b", NULL) == 0);
    CHECK(exchange(port, REQUEST("/optionals/a/b/extra"), 404, "", NULL) == 0);
    CHECK(exchange(port, REQUEST("/literal-tail/end"), 200, "_|_", NULL) == 0);
    CHECK(exchange(port, REQUEST("/literal-tail/b/end"), 200, "_|b", NULL) ==
          0);
    CHECK(exchange(port, REQUEST("/literal-tail/a/b/end"), 200, "a|b", NULL) ==
          0);
    CHECK(exchange(port, REQUEST("/literal-tail"), 404, "", NULL) == 0);
    CHECK(exchange(port, REQUEST("/literal-tail/a/b"), 404, "", NULL) == 0);
    CHECK(exchange(port, REQUEST("/optional"), 404, "", NULL) == 0);
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

static knurl_status process_start(knurl_server *server,
                                  const knurl_process_context *process,
                                  void *userdata, knurl_error *error)
{
    struct fixture *fixture;
    (void)server;
    (void)error;
    fixture = userdata;
    if (process->role == KNURL_PROCESS_PARENT) {
        if (fixture->phase != 0)
            return KNURL_INVALID;
        fixture->phase = 1;
    }
    return KNURL_OK;
}

static knurl_status prepare(knurl_server *server,
                            const knurl_process_context *process,
                            void *userdata, knurl_error *error)
{
    struct fixture *fixture;
    (void)server;
    (void)process;
    (void)error;
    fixture = userdata;
    if (fixture->phase != 1)
        return KNURL_INVALID;
    fixture->phase = 2;
    if (fixture->mode == 3)
        return knurl_server_request_stop(server, error);
    return KNURL_OK;
}

static void *thread_stop(void *userdata)
{
    knurl_error error;
    knurl_server *server;
    server = userdata;
    return knurl_server_request_stop(server, &error) == KNURL_OK ? NULL
                                                                 : userdata;
}

static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *userdata,
                          knurl_error *error)
{
    struct fixture *fixture;
    knurl_bound_listener bound;
    knurl_server *other;
    int result;
    pthread_t thread;
    void *thread_result;
    (void)process;
    fixture = userdata;
    if (fixture->phase != 2)
        return KNURL_INVALID;
    fixture->phase = 3;
    if (fixture->mode == 1)
        return KNURL_INVALID;
    if (fixture->mode == 2) {
        (void)raise(SIGTERM);
        (void)raise(SIGCHLD);
        return KNURL_OK;
    }
    if (fixture->mode == 4) {
        if (pthread_create(&thread, NULL, thread_stop, server) != 0)
            return KNURL_IO;
        if (pthread_join(thread, &thread_result) != 0 || thread_result != NULL)
            return KNURL_INVALID;
        return KNURL_OK;
    }
    if (knurl_server_new(&fixture->config, &other, error) != KNURL_OK)
        return KNURL_INVALID;
    if (knurl_server_run(other, NULL, error) != KNURL_CONFLICT)
        return KNURL_INVALID;
    if (knurl_server_destroy(other, error) != KNURL_OK)
        return KNURL_INVALID;
    if (knurl_server_listener_at(server, 0, &bound, error) != KNURL_OK ||
        bound.port == 0)
        return KNURL_INVALID;
    if (knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return KNURL_INVALID;
    fixture->client = fork();
    if (fixture->client == -1)
        return KNURL_IO;
    if (fixture->client == 0) {
        result = client(bound.port);
        if (result != 0)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    return KNURL_OK;
}

static knurl_status stopping(knurl_server *server,
                             const knurl_process_context *process,
                             void *userdata, knurl_error *error)
{
    struct fixture *fixture;
    (void)server;
    (void)process;
    (void)error;
    fixture = userdata;
    if (fixture->phase != 3 && !(fixture->mode == 3 && fixture->phase == 2))
        return KNURL_INVALID;
    fixture->phase = 4;
    return KNURL_OK;
}

static knurl_status stopped(knurl_server *server,
                            const knurl_process_context *process,
                            void *userdata, knurl_error *error)
{
    struct fixture *fixture;
    (void)server;
    (void)process;
    (void)error;
    fixture = userdata;
    if (fixture->phase != 4)
        return KNURL_INVALID;
    fixture->phase = 5;
    return KNURL_OK;
}

static knurl_status worker_note(knurl_server *server,
                                const knurl_process_context *process,
                                void *userdata, knurl_error *error)
{
    struct fixture *fixture;
    unsigned char note;
    (void)server;
    (void)process;
    (void)error;
    fixture = userdata;
    note = (unsigned char)++fixture->worker_phase;
    return write(fixture->notes[1], &note, 1) == 1 ? KNURL_OK : KNURL_IO;
}

static int worker_exit(knurl_server *server,
                       const knurl_process_context *process, void *userdata)
{
    return worker_note(server, process, userdata, NULL) == KNURL_OK ? 0 : 1;
}

int main(void)
{
    struct fixture fixture;
    knurl_listener_config listener;
    knurl_route_config route;
    knurl_route_group_config group_config;
    knurl_route_group *group;
    knurl_server *server;
    knurl_router *router;
    knurl_run_result result;
    knurl_error error;
    knurl_status status;
    struct sigaction before_signal;
    struct sigaction after_signal;
    const char *paths[] = {"/items/:id",
                           "/echo",
                           "/empty",
                           "/failure",
                           "/head",
                           "/stop",
                           "/optional/:first?/:last",
                           "/optionals/:first?/:second?",
                           "/literal-tail/:first?/:second?/end",
                           "^/regex/[0-9]+$"};
    const char *kinds[] = {"metadata",  "echo", "empty",    "failure",
                           "head",      "stop", "optional", "optionals",
                           "optionals", "regex"};
    unsigned char notes[3];
    int child_status;
    int iteration;
    size_t i;
    memset(&fixture, 0, sizeof(fixture));
    CHECK(pipe(fixture.notes) == 0);
    knurl_server_config_init(&fixture.config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    fixture.config.listeners = &listener;
    fixture.config.listener_count = 1;
    fixture.config.workers.count = 1;
    fixture.config.workers.affinity_disabled = 1;
    fixture.config.workers.shutdown_timeout_ms = 1000;
    fixture.config.http.backend_quiet = 1;
    fixture.config.name = "fixture-instance";
    fixture.config.http.server_header = "fixture-http";
    fixture.config.limits.hard_body_bytes.low = 1024;
    fixture.config.start_paused = 1;
    fixture.config.signals.mode = KNURL_SIGNALS_LIBRARY;
    fixture.config.callbacks.on_process_start = process_start;
    fixture.config.callbacks.on_parent_prepare = prepare;
    fixture.config.callbacks.on_parent_ready = ready;
    fixture.config.callbacks.on_parent_stopping = stopping;
    fixture.config.callbacks.on_parent_stopped = stopped;
    fixture.config.callbacks.on_worker_start = worker_note;
    fixture.config.callbacks.on_worker_stopping = worker_note;
    fixture.config.callbacks.on_worker_exit = worker_exit;
    fixture.config.callbacks.userdata = &fixture;
    CHECK(knurl_server_new(&fixture.config, &server, &error) == KNURL_OK);
    router = knurl_server_router(server);
    knurl_route_config_init(&route);
    route.path = "/over-hard-limit";
    route.handler = handler;
    route.userdata = "echo";
    knurl_body_policy_init_buffered(&route.body);
    route.body.max_bytes.low = 1025;
    CHECK(knurl_router_route(router, &route, NULL, &error) == KNURL_INVALID &&
          knurl_router_route_count(router) == 0);
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        knurl_route_config_init(&route);
        route.path = paths[i];
        route.path_kind = paths[i][0] == '^'              ? KNURL_PATH_REGEX
                          : strchr(paths[i], ':') != NULL ? KNURL_PATH_PARAMS
                                                          : KNURL_PATH_LITERAL;
        route.handler = handler;
        route.userdata = (void *)kinds[i];
        if (i == 1) {
            knurl_body_policy_init_buffered(&route.body);
            route.body.max_bytes.low = 1024;
        }
        CHECK(knurl_router_route(router, &route, NULL, &error) == KNURL_OK);
    }
    knurl_route_group_config_init(&group_config);
    group_config.prefix = "/group";
    group_config.hooks.before = before;
    group_config.hooks.after = after;
    CHECK(knurl_router_group(router, &group_config, &group, &error) ==
          KNURL_OK);
    knurl_route_config_init(&route);
    route.path = "/result";
    route.handler = handler;
    route.userdata = "chain";
    CHECK(knurl_route_group_route(group, &route, NULL, &error) == KNURL_OK);
    CHECK(knurl_server_validate(server, &error) == KNURL_OK);
    CHECK(knurl_server_preflight(server, &error) == KNURL_OK);
    CHECK(sigaction(SIGTERM, NULL, &before_signal) == 0);
    for (iteration = 0; iteration < 6; ++iteration) {
        fixture.phase = fixture.worker_phase = 0;
        fixture.client = -1;
        fixture.mode = iteration == 0 ? 1 : (iteration == 1 ? 2 : 0);
        if (iteration == 4)
            fixture.mode = 3;
        if (iteration == 5)
            fixture.mode = 4;
        status = knurl_server_run(server, &result, &error);
        if (status != KNURL_OK && fixture.mode != 1)
            (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
        CHECK(status == (fixture.mode == 1 ? KNURL_INVALID : KNURL_OK));
        CHECK(fixture.phase == 5);
        CHECK(result.escalated == 0);
        if (fixture.mode != 3) {
            CHECK(read(fixture.notes[0], notes, sizeof(notes)) ==
                  (ssize_t)sizeof(notes));
            CHECK(notes[0] == 1 && notes[1] == 2 && notes[2] == 3);
        }
        CHECK(result.reason ==
              (fixture.mode == 1
                   ? KNURL_RUN_STARTUP_FAILURE
                   : (fixture.mode == 2 ? KNURL_RUN_TERMINATING_SIGNAL
                                        : KNURL_RUN_REQUESTED_STOP)));
        if (fixture.mode == 2)
            CHECK(result.signal_number == SIGTERM);
        if (fixture.client > 0) {
            CHECK(waitpid(fixture.client, &child_status, 0) == fixture.client);
            CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
        }
        CHECK(sigaction(SIGTERM, NULL, &after_signal) == 0);
        CHECK(before_signal.sa_handler == after_signal.sa_handler);
    }
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    CHECK(close(fixture.notes[0]) == 0);
    CHECK(close(fixture.notes[1]) == 0);
    return 0;
}
