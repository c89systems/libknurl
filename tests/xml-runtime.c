#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <rotmos/lonejson.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            (void)fprintf(stderr, "XML wire %d: %s\n", __LINE__, #t);          \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct input {
    char *message;
};
static const lonejson_field input_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct input, message, "message")};
LONEJSON_MAP_DEFINE(input_map, struct input, input_fields);
static int called, replaced, stopped;
struct fixture {
    pid_t child;
    char directory[64];
};
static rotmos_status echo(rotmos_request *request,
                          const lonejson_record_view *input,
                          rotmos_reply *reply, void *data, rotmos_error *error)
{
    struct input *in;
    rotmos_xml_source_config output;
    size_t count;
    char first;
    (void)error;
    if (input == NULL)
        return ROTMOS_STATE;
    in = input->record;
    ++called;
    if (data != NULL &&
        rotmos_body_kind(rotmos_request_body(request)) != ROTMOS_BODY_FILE)
        return ROTMOS_STATE;
    if (rotmos_body_read(rotmos_request_body(request), &first, 1, &count,
                         error) != ROTMOS_OK ||
        count != 1 || first != '<' ||
        rotmos_body_rewind(rotmos_request_body(request), error) != ROTMOS_OK)
        return ROTMOS_STATE;
    if (in->message == NULL)
        return ROTMOS_STATE;
    rotmos_xml_source_config_init(&output);
    output.record = input;
    output.xml.root_element = "doc";
    return rotmos_reply_xml(reply, 200, &output, error);
}
static rotmos_status before(rotmos_request *r, rotmos_reply *reply, int *finish,
                            void *data, rotmos_error *error)
{
    (void)r;
    (void)finish;
    (void)data;
    return rotmos_reply_header_add(reply, "X-Before", "yes", error);
}
static rotmos_status after(rotmos_request *r, rotmos_reply *reply, int *finish,
                           void *data, rotmos_error *error)
{
    (void)r;
    (void)finish;
    (void)data;
    if (rotmos_reply_payload_kind(reply) != ROTMOS_REPLY_SOURCE)
        return ROTMOS_STATE;
    ++replaced;
    return rotmos_reply_text(reply, 201, "text/plain", "replaced", error);
}
static rotmos_status auth(rotmos_request *r, rotmos_reply *reply, int *finish,
                          void *data, rotmos_error *error)
{
    (void)r;
    (void)data;
    *finish = 1;
    return rotmos_reply_text(reply, 403, "text/plain", "denied", error);
}
static rotmos_status stop(rotmos_request *request, rotmos_reply *reply,
                          void *data, rotmos_error *error)
{
    (void)data;
    stopped = called == 3 && replaced == 1;
    if (!stopped)
        return ROTMOS_STATE;
    if (rotmos_server_request_stop(rotmos_request_server(request), error) !=
        ROTMOS_OK)
        return ROTMOS_STATE;
    return rotmos_reply_text(reply, 200, "text/plain", "stopped", error);
}
static int write_all(int fd, const void *bytes, size_t count)
{
    const unsigned char *p;
    ssize_t n;
    p = bytes;
    while (count != 0) {
        n = write(fd, p, count);
        CHECK(n > 0);
        p += n;
        count -= (size_t)n;
    }
    return 0;
}
static int read_all(int fd, void *bytes, size_t count)
{
    unsigned char *p;
    ssize_t n;
    p = bytes;
    while (count != 0) {
        n = read(fd, p, count);
        CHECK(n > 0);
        p += n;
        count -= (size_t)n;
    }
    return 0;
}
static int line(int fd, char *out, size_t bound)
{
    size_t used = 0;
    do {
        CHECK(used + 1 < bound && read_all(fd, out + used, 1) == 0);
        out[++used] = 0;
    } while (used < 2 || memcmp(out + used - 2, "\r\n", 2) != 0);
    return 0;
}
static int exchange(unsigned short port, const char *path, const char *body,
                    int code, const char *expected, int before_header,
                    int large)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char metadata[4096], request[512], ending[2];
    char *result, *length;
    size_t used, total, count;
    ssize_t n;
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
          0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    (void)snprintf(request, sizeof(request),
                   "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: "
                   "close\r\nContent-Length: %lu\r\n\r\n",
                   strcmp(path, "/empty") == 0 || strcmp(path, "/stop") == 0
                       ? "GET"
                       : "POST",
                   path, (unsigned long)strlen(body));
    CHECK(write_all(fd, request, strlen(request)) == 0 &&
          write_all(fd, body, strlen(body)) == 0);
    used = 0;
    metadata[0] = 0;
    do {
        CHECK(used + 1 < sizeof(metadata) &&
              read_all(fd, metadata + used, 1) == 0);
        metadata[++used] = 0;
    } while (strstr(metadata, "\r\n\r\n") == NULL);
    (void)snprintf(request, sizeof(request), " %d ", code);
    if (strstr(metadata, request) == NULL)
        (void)fprintf(stderr, "%s: %s\n", path, metadata);
    CHECK(strstr(metadata, request) != NULL);
    if (before_header)
        CHECK(strstr(metadata, "X-Before: yes") != NULL);
    result = malloc(131100);
    CHECK(result != NULL);
    total = 0;
    if (strstr(metadata, "transfer-encoding: chunked") != NULL) {
        do {
            CHECK(line(fd, request, sizeof(request)) == 0);
            count = (size_t)strtoul(request, NULL, 16);
            CHECK(count <= 131099 - total);
            if (count == 0) {
                CHECK(line(fd, request, sizeof(request)) == 0 &&
                      strcmp(request, "\r\n") == 0);
                break;
            }
            CHECK(read_all(fd, result + total, count) == 0 &&
                  read_all(fd, ending, 2) == 0 &&
                  memcmp(ending, "\r\n", 2) == 0);
            total += count;
        } while (1);
    } else {
        length = strstr(metadata, "content-length: ");
        count = length != NULL ? (size_t)strtoul(length + 16, NULL, 10) : 0;
        CHECK(count <= 131099 && read_all(fd, result, count) == 0);
        total = count;
    }
    result[total] = 0;
    (void)large;
    if (strcmp(result, expected) != 0)
        fprintf(stderr, "%s body: %s\n", path, result);
    CHECK(strcmp(result, expected) == 0);
    n = read(fd, ending, 1);
    CHECK(n == 0);
    free(result);
    (void)close(fd);
    return 0;
}
static int client(unsigned short port)
{
    CHECK(exchange(port, "/api/echo",
                   "<doc><message>hello &amp; XML</message></doc>", 200,
                   "<doc><message>hello &amp; XML</message></doc>", 1, 0) == 0);
    CHECK(exchange(port, "/api/echo",
                   "<doc><message>allocated</message><message/></doc>", 400, "",
                   0, 0) == 0);
    CHECK(exchange(port, "/spilled", "<doc><message>file</message></doc>", 200,
                   "<doc><message>file</message></doc>", 0, 0) == 0);
    CHECK(exchange(port, "/replace", "<doc><message>hidden</message></doc>",
                   201, "replaced", 0, 0) == 0);
    CHECK(exchange(port, "/deny", "<", 403, "denied", 0, 0) == 0);
    CHECK(exchange(port, "/limit", "<doc><message>too large</message></doc>",
                   413, "", 0, 0) == 0);
    CHECK(exchange(port, "/api/echo",
                   "<!DOCTYPE doc [<!ENTITY x "
                   "'entity'>]><doc><message>&x;</message></doc>",
                   400, "", 0, 0) == 0);
    CHECK(exchange(port, "/stop", "", 200, "stopped", 0, 0) == 0);
    return 0;
}
static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *p, void *data,
                           rotmos_error *error)
{
    struct fixture *f;
    rotmos_bound_listener listener;
    int result;
    (void)p;
    f = data;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    f->child = fork();
    if (f->child == 0) {
        (void)signal(SIGTERM, SIG_DFL);
        result = client(listener.port);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    return f->child < 0 ? ROTMOS_IO : ROTMOS_OK;
}
static int worker_exit(rotmos_server *server, const rotmos_process_context *p,
                       void *data)
{
    (void)server;
    (void)p;
    (void)data;
    return stopped ? 0 : 1;
}
int main(void)
{
    struct fixture fixture;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_xml_route_config xml;
    rotmos_route_group_config group_config;
    rotmos_route_group *group;
    rotmos_route_config ordinary;
    rotmos_server *server;
    rotmos_error error;
    rotmos_run_result result;
    rotmos_status status;
    lonejson_schema_view input;
    int child_status;
    char path[16] = "/echo", root[16] = "doc";
    memset(&fixture, 0, sizeof(fixture));
    fixture.child = -1;
    strcpy(fixture.directory, "xml-spool-XXXXXX");
    CHECK(mkdtemp(fixture.directory) != NULL);
    signal(SIGPIPE, SIG_IGN);
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.io.output_queue_bytes = 4096;
    config.io.chunk_bytes = 128;
    config.limits.body_spool_directory = fixture.directory;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_exit = worker_exit;
    config.callbacks.userdata = &fixture;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    CHECK(rotmos_json_schema_from_map(NULL, &input_map, &input, &error) ==
          ROTMOS_OK);
    rotmos_route_group_config_init(&group_config);
    group_config.prefix = "/api";
    group_config.hooks.before = before;
    CHECK(rotmos_router_group(rotmos_server_router(server), &group_config,
                              &group, &error) == ROTMOS_OK);
    rotmos_xml_route_config_init(&xml);
    xml.method = ROTMOS_HTTP_POST;
    xml.path = path;
    xml.input = &input;
    xml.handler = echo;
    xml.xml.root_element = root;
    CHECK(rotmos_route_group_xml_route(group, &xml, NULL, &error) == ROTMOS_OK);
    strcpy(path, "/changed");
    strcpy(root, "changed");
    xml.xml.root_element = "doc";
    xml.path = "/spilled";
    xml.body.memory_buffer_limit_bytes = 8;
    xml.userdata = "spilled";
    CHECK(rotmos_router_xml_route(rotmos_server_router(server), &xml, NULL,
                                  &error) == ROTMOS_OK);
    xml.path = "/replace";
    xml.body.memory_buffer_limit_bytes = 262144;
    xml.userdata = NULL;
    xml.hooks.after = after;
    CHECK(rotmos_router_xml_route(rotmos_server_router(server), &xml, NULL,
                                  &error) == ROTMOS_OK);
    xml.path = "/deny";
    xml.hooks.after = NULL;
    xml.hooks.auth = auth;
    CHECK(rotmos_router_xml_route(rotmos_server_router(server), &xml, NULL,
                                  &error) == ROTMOS_OK);
    xml.path = "/limit";
    xml.hooks.auth = NULL;
    xml.body.max_bytes.low = xml.body.memory_buffer_limit_bytes = 16;
    CHECK(rotmos_router_xml_route(rotmos_server_router(server), &xml, NULL,
                                  &error) == ROTMOS_OK);
    rotmos_route_config_init(&ordinary);
    ordinary.method = ROTMOS_HTTP_GET;
    ordinary.path = "/stop";
    ordinary.handler = stop;
    CHECK(rotmos_router_route(rotmos_server_router(server), &ordinary, NULL,
                              &error) == ROTMOS_OK);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK) {
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
        if (fixture.child > 0) {
            kill(fixture.child, SIGTERM);
            waitpid(fixture.child, NULL, 0);
        }
        rotmos_server_destroy(server, NULL);
        rmdir(fixture.directory);
        return 1;
    }
    CHECK(result.recovered_failures.high == 0 &&
          result.recovered_failures.low == 0 && !result.escalated);
    CHECK(waitpid(fixture.child, &child_status, 0) == fixture.child &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK &&
          rmdir(fixture.directory) == 0);
    return 0;
}
