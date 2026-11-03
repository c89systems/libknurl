#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <rotmos/rotmos.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REQUIRE(test)                                                          \
    do {                                                                       \
        if (!(test)) {                                                         \
            (void)fprintf(stderr, "response line %d: %s\n", __LINE__, #test);  \
            return 1;                                                          \
        }                                                                      \
    } while (0)

struct fixture {
    pid_t client;
};
struct context {
    rotmos_request *request;
    rotmos_response *response;
    int reads;
    int mode;
    int resumed;
    int source_closed;
    int file;
    size_t sent;
};
static int failures;
static int completions;
static int pauses;

static void resume(rotmos_timer *timer, void *userdata)
{
    struct context *c;
    (void)timer;
    c = userdata;
    if (c->reads != 1)
        ++failures;
    c->resumed = 1;
    if (rotmos_response_producer_resume(c->response, NULL) != ROTMOS_OK ||
        rotmos_response_producer_resume(c->response, NULL) != ROTMOS_OK ||
        c->reads != 1)
        ++failures;
}

static rotmos_status pull(void *buffer, size_t capacity, size_t *produced,
                          void *userdata, rotmos_error *error)
{
    struct context *c;
    rotmos_timer *timer;
    (void)error;
    c = userdata;
    ++c->reads;
    *produced = 0;
    if (c->mode == 1 && c->reads == 1) {
        ++pauses;
        if (rotmos_loop_timer(rotmos_request_loop(c->request), 100, 0, resume,
                              NULL, c, &timer, error) != ROTMOS_OK)
            return ROTMOS_IO;
        return ROTMOS_AGAIN;
    }
    if (c->mode == 2 || (c->mode == 3 && c->reads == 3))
        return ROTMOS_IO;
    if (c->mode == 4)
        return ROTMOS_EOF;
    if (c->mode == 5) {
        *produced = capacity + 1;
        return ROTMOS_OK;
    }
    if ((c->mode == 1 && c->reads == 2) || (c->mode == 3 && c->reads < 3) ||
        (c->mode == 0 && c->reads == 1)) {
        if (capacity < 7)
            return ROTMOS_INVALID;
        memcpy(buffer, "payload", 7);
        *produced = 7;
        return ROTMOS_OK;
    }
    return ROTMOS_EOF;
}

static void source_closed(void *userdata, rotmos_status outcome)
{
    struct context *c;
    c = userdata;
    ++c->source_closed;
    if (c->source_closed != 1 || rotmos_request_target(c->request).length == 0)
        ++failures;
    if ((c->mode == 1 || c->mode == 0) && outcome != ROTMOS_OK)
        ++failures;
    if ((c->mode == 6) && (outcome != ROTMOS_CANCELLED || c->reads != 0))
        ++failures;
    if ((c->mode == 2 || c->mode == 4 || c->mode == 5) &&
        outcome != ROTMOS_IO && outcome != ROTMOS_INVALID)
        ++failures;
}

static void closed(rotmos_response *response,
                   const rotmos_response_outcome *out, void *userdata)
{
    struct context *c;
    c = userdata;
    ++completions;
    if (c->file != -1 && fcntl(c->file, F_GETFD) != -1)
        ++failures;
    if (rotmos_request_target(c->request).length == 0 ||
        rotmos_response_queued_bytes(response) > 4096 || !out->committed ||
        rotmos_response_state(response) < ROTMOS_RESPONSE_COMPLETE)
        ++failures;
    if (c->mode == 1 &&
        (!c->resumed || !c->source_closed || out->body_bytes.low != 7))
        ++failures;
    if (c->mode == 7 &&
        (out->body_bytes.high != 0 || out->body_bytes.low != 8192))
        ++failures;
    if (c->mode == 6 && (out->body_bytes.high != 0 || out->body_bytes.low != 0))
        ++failures;
    if (rotmos_response_abort(response, ROTMOS_IO, NULL) != ROTMOS_STATE)
        ++failures;
    free(c);
}

static void writable(rotmos_response *response, void *userdata)
{
    struct context *c;
    rotmos_status status;
    unsigned char data[257];
    size_t count;
    size_t offered;
    c = userdata;
    memset(data, 'x', sizeof(data));
    while (c->sent < 8192) {
        offered = 8192 - c->sent;
        if (offered > sizeof(data))
            offered = sizeof(data);
        status = rotmos_response_write(response, data, offered, &count, NULL);
        if (status == ROTMOS_AGAIN) {
            if (count != 0)
                ++failures;
            return;
        }
        if (status != ROTMOS_OK || count == 0) {
            ++failures;
            (void)rotmos_response_abort(response, ROTMOS_IO, NULL);
            return;
        }
        c->sent += count;
        if (rotmos_response_queued_bytes(response) > 4096)
            ++failures;
    }
    if (rotmos_response_end(response, NULL, 0, NULL) != ROTMOS_OK ||
        rotmos_response_end(response, NULL, 0, NULL) != ROTMOS_OK)
        ++failures;
}

static rotmos_status handler(rotmos_request *request, rotmos_reply *reply,
                             void *userdata, rotmos_error *error)
{
    struct context *c;
    const char *path;
    rotmos_response *response;
    rotmos_source_config config;
    rotmos_source *source;
    rotmos_u64 length;
    rotmos_u64 offset;
    rotmos_header trailer;
    rotmos_bytes bytes;
    rotmos_status status;
    size_t count;
    int fd;
    off_t position;
    char file[] = "response-file-XXXXXX";
    (void)userdata;
    path = rotmos_request_path(request).data;
    if (strcmp(path, "/stop") == 0) {
        (void)rotmos_server_request_stop(rotmos_request_server(request), NULL);
        if (failures || completions < 12 || pauses != 1)
            return rotmos_reply_text(reply, 500, "text/plain", "failed", error);
        return rotmos_reply_text(reply, 200, "text/plain", "stopped", error);
    }
    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return ROTMOS_NOMEM;
    c->request = request;
    c->response = response = rotmos_request_response(request);
    c->file = -1;
    if (rotmos_response_callbacks(response, writable, closed, c, error) !=
        ROTMOS_OK) {
        free(c);
        return ROTMOS_INVALID;
    }
    if (strcmp(path, "/manual") == 0) {
        c->mode = 7;
        length.high = 0;
        length.low = 8192;
        if (rotmos_response_header_add(response, "Content-Length", "1", NULL) !=
                ROTMOS_INVALID ||
            rotmos_response_header_add(response, "X-Bad", "x\r\ny", NULL) !=
                ROTMOS_INVALID ||
            rotmos_response_status(response, 101, NULL) != ROTMOS_INVALID ||
            rotmos_response_known_length(response, length, error) !=
                ROTMOS_OK ||
            rotmos_response_begin(response, error) != ROTMOS_OK)
            return ROTMOS_INVALID;
        count = 99;
        if (rotmos_response_write(response, "x", 8193, &count, NULL) !=
                ROTMOS_OVERFLOW ||
            count != 0)
            return ROTMOS_INVALID;
        writable(response, c);
        return ROTMOS_OK;
    }
    if (strcmp(path, "/replace") == 0) {
        if (rotmos_response_begin(response, error) != ROTMOS_OK ||
            rotmos_response_is_committed(response) ||
            rotmos_response_replace_uncommitted(response, error) != ROTMOS_OK)
            return ROTMOS_INVALID;
        bytes.data = "replaced";
        bytes.length = 8;
        return rotmos_response_bytes(response, 502, "text/plain", bytes, error);
    }
    if (strcmp(path, "/head-bytes") == 0) {
        void *memory;
        c->mode = 6;
        memory = malloc(102400);
        if (memory == NULL)
            return ROTMOS_NOMEM;
        bytes.data = memory;
        bytes.length = 102400;
        status =
            rotmos_response_bytes(response, 200, "text/plain", bytes, error);
        free(memory);
        return status;
    }
    if (strcmp(path, "/trailers") == 0) {
        if (rotmos_response_trailers_enable(response, NULL, 0, error) !=
                ROTMOS_OK ||
            rotmos_response_begin(response, error) != ROTMOS_OK ||
            rotmos_response_write(response, "abc", 3, &count, error) !=
                ROTMOS_OK ||
            count != 3)
            return ROTMOS_INVALID;
        trailer.name.data = "Authorization";
        trailer.name.length = 13;
        trailer.value.data = "yes";
        trailer.value.length = 3;
        if (rotmos_response_end(response, &trailer, 1, NULL) != ROTMOS_INVALID)
            return ROTMOS_INVALID;
        trailer.name.data = "X-End";
        trailer.name.length = 5;
        if (rotmos_response_end(response, &trailer, 1, error) != ROTMOS_OK ||
            rotmos_response_end(response, &trailer, 1, error) != ROTMOS_OK ||
            rotmos_response_end(response, NULL, 0, NULL) != ROTMOS_STATE)
            return ROTMOS_INVALID;
        return ROTMOS_OK;
    }
    if (strcmp(path, "/file") == 0) {
        fd = mkstemp(file);
        if (fd == -1)
            return ROTMOS_IO;
        (void)unlink(file);
        position = (off_t)5 * 1024 * 1024 * 1024;
        if (pwrite(fd, "file", 4, position) != 4) {
            (void)close(fd);
            return ROTMOS_IO;
        }
        length.high = 0;
        length.low = 4;
        offset.high = 1;
        offset.low = 1073741824UL;
        status = rotmos_response_file_take(response, fd, offset, length, error);
        if (status != ROTMOS_OK) {
            (void)close(fd);
            return status;
        }
        c->file = fd;
        return rotmos_response_begin(response, error);
    }
    if (strcmp(path, "/large") == 0) {
        char *body;
        body = malloc(16384);
        if (body == NULL)
            return ROTMOS_NOMEM;
        memset(body, 'z', 16384);
        bytes.data = body;
        bytes.length = 16384;
        status = rotmos_reply_bytes(reply, 200, "application/octet-stream",
                                    bytes, error);
        free(body);
        return status;
    }
    c->mode =
        strcmp(path, "/paused") == 0
            ? 1
            : (strcmp(path, "/error") == 0
                   ? 2
                   : (strcmp(path, "/late-error") == 0
                          ? 3
                          : (strcmp(path, "/short") == 0
                                 ? 4
                                 : (strcmp(path, "/invalid") == 0
                                        ? 5
                                        : (strcmp(path, "/head") == 0 ||
                                                   strcmp(path, "/204") == 0 ||
                                                   strcmp(path, "/205") == 0
                                               ? 6
                                               : 0)))));
    rotmos_source_config_init(&config);
    config.read = pull;
    config.close = source_closed;
    config.userdata = c;
    if (c->mode == 0 || c->mode == 4 || c->mode == 6) {
        config.has_length = 1;
        config.length.low = 7;
    }
    if (rotmos_source_new(&config, &source, error) != ROTMOS_OK)
        return ROTMOS_NOMEM;
    status = rotmos_reply_source_take(
        reply,
        strcmp(path, "/204") == 0 ? 204
                                  : (strcmp(path, "/205") == 0 ? 205 : 200),
        "text/plain", source, error);
    if (status != ROTMOS_OK)
        (void)rotmos_source_destroy(source, NULL);
    return status;
}

static int exchange(unsigned short port, const char *path, int expected,
                    const char *body, size_t repeated, const char *field,
                    int head)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char request[256];
    char *response;
    char *payload;
    char status[24];
    size_t used;
    size_t i;
    ssize_t count;
    int fd;
    response = malloc(32768);
    REQUIRE(response != NULL);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    REQUIRE(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    REQUIRE(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    REQUIRE(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    if (strcmp(path, "/pipeline") == 0)
        (void)snprintf(request, sizeof(request),
                       "GET /source HTTP/1.1\r\nHost: localhost\r\n\r\n"
                       "GET /source HTTP/1.1\r\nHost: localhost\r\n"
                       "Connection: close\r\n\r\n");
    else
        (void)snprintf(
            request, sizeof(request),
            "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
            head ? "HEAD" : "GET", path);
    REQUIRE(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
    used = 0;
    while (used < 32767) {
        count = read(fd, response + used, 32767 - used);
        if (count == -1 && errno == EINTR)
            continue;
        REQUIRE(count >= 0);
        if (count == 0)
            break;
        used += (size_t)count;
    }
    REQUIRE(close(fd) == 0);
    response[used] = '\0';
    (void)snprintf(status, sizeof(status), "HTTP/1.1 %d ", expected);
    if (strncmp(response, status, strlen(status)) != 0) {
        (void)fprintf(stderr, "%s: received %s\n", path, response);
        return 1;
    }
    payload = strstr(response, "\r\n\r\n");
    REQUIRE(payload != NULL);
    payload += 4;
    if (strcmp(path, "/pipeline") == 0) {
        REQUIRE(strncmp(payload, "payloadHTTP/1.1 200 ", 19) == 0);
        payload = strstr(payload + 7, "\r\n\r\n");
        REQUIRE(payload != NULL && strcmp(payload + 4, "payload") == 0);
    } else if (repeated != 0) {
        REQUIRE(strlen(payload) == repeated);
        for (i = 0; i < repeated; ++i)
            REQUIRE(payload[i] == body[0]);
    } else {
        REQUIRE(strcmp(payload, body) == 0);
    }
    if (field != NULL) {
        if (field[0] == '!')
            REQUIRE(strstr(response, field + 1) == NULL);
        else
            REQUIRE(strstr(response, field) != NULL);
    }
    free(response);
    return 0;
}

static int client(unsigned short port)
{
    REQUIRE(exchange(port, "/source", 200, "payload", 0,
                     "content-length: 7\r\n", 0) == 0);
    REQUIRE(exchange(port, "/pipeline", 200, "", 0, NULL, 0) == 0);
    REQUIRE(exchange(port, "/paused", 200, "7\r\npayload\r\n0\r\n\r\n", 0,
                     "transfer-encoding: chunked\r\n", 0) == 0);
    REQUIRE(exchange(port, "/head", 200, "", 0, "content-length: 7\r\n", 1) ==
            0);
    REQUIRE(exchange(port, "/head-bytes", 200, "", 0,
                     "content-length: 102400\r\n", 1) == 0);
    REQUIRE(exchange(port, "/204", 204, "", 0, "!content-length:", 0) == 0);
    REQUIRE(exchange(port, "/205", 205, "", 0, "content-length: 0\r\n", 0) ==
            0);
    REQUIRE(exchange(port, "/error", 500, "", 0, "content-length: 0\r\n", 0) ==
            0);
    REQUIRE(exchange(port, "/short", 500, "", 0, "content-length: 0\r\n", 0) ==
            0);
    REQUIRE(exchange(port, "/invalid", 500, "", 0, "content-length: 0\r\n",
                     0) == 0);
    REQUIRE(exchange(port, "/late-error", 200, "7\r\npayload\r\n", 0,
                     "!0\r\n\r\n", 0) == 0);
    REQUIRE(exchange(port, "/file", 200, "file", 0, "content-length: 4\r\n",
                     0) == 0);
    REQUIRE(exchange(port, "/large", 200, "z", 16384,
                     "content-length: 16384\r\n", 0) == 0);
    REQUIRE(exchange(port, "/manual", 200, "x", 8192,
                     "content-length: 8192\r\n", 0) == 0);
    REQUIRE(exchange(port, "/replace", 502, "replaced", 0, NULL, 0) == 0);
    REQUIRE(exchange(port, "/trailers", 200,
                     "3\r\nabc\r\n0\r\nX-End: yes\r\n\r\n", 0, NULL, 0) == 0);
    REQUIRE(exchange(port, "/stop", 200, "stopped", 0, NULL, 0) == 0);
    return 0;
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process,
                           void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    rotmos_bound_listener listener;
    int result;
    (void)process;
    fixture = userdata;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    fixture->client = fork();
    if (fixture->client == -1)
        return ROTMOS_IO;
    if (fixture->client == 0) {
        result = client(listener.port);
        if (result != 0)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    return ROTMOS_OK;
}

int main(void)
{
    struct fixture fixture;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_route_config route;
    rotmos_server *server;
    rotmos_error error;
    rotmos_run_result result;
    rotmos_status status;
    int child_status;
    fixture.client = -1;
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.workers.shutdown_timeout_ms = 100;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.io.chunk_bytes = 257;
    config.io.output_queue_bytes = 4096;
    config.timeouts.response_write_idle.milliseconds = 50;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.userdata = &fixture;
    REQUIRE(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    rotmos_route_config_init(&route);
    route.path = "^/.*$";
    route.path_kind = ROTMOS_PATH_REGEX;
    route.handler = handler;
    REQUIRE(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                                &error) == ROTMOS_OK);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
    REQUIRE(status == ROTMOS_OK && !result.escalated);
    REQUIRE(waitpid(fixture.client, &child_status, 0) == fixture.client);
    REQUIRE(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    REQUIRE(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    return 0;
}
