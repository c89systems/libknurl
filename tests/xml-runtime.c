#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <rotmos/lonejson.h>
#include "vectis-forward.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
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
static int called, replaced, stopped, live_called, empty_called;
static struct {
    pthread_t owner;
    int ready, notified, pipe[2];
} parsing;
static int record_acquired, record_released, record_wrong_thread;
static rotmos_status acquire_record(rotmos_request *request, int output,
                                    const lonejson_schema_view *schema,
                                    rotmos_record_storage *storage, void *data,
                                    rotmos_error *error)
{
    rotmos_bytes path = rotmos_request_path(request);
    (void)data;
    (void)error;
    ++record_acquired;
    if (!pthread_equal(parsing.owner, pthread_self()) || output)
        return ROTMOS_STATE;
    storage->view.size = sizeof(storage->view);
    storage->view.abi_version = LONEJSON_VIEW_ABI_VERSION;
    storage->view.schema = *schema;
    storage->view.record = calloc(1, schema->record_size);
    storage->owner = storage->view.record;
    if (storage->owner == NULL)
        return ROTMOS_NOMEM;
    lonejson_init(schema->runtime, schema->map, storage->owner);
    return path.length == sizeof("/provider-fail") - 1 &&
                   memcmp(path.data, "/provider-fail", path.length) == 0
               ? ROTMOS_IO
               : ROTMOS_OK;
}
static void release_record(rotmos_record_storage *storage, void *data)
{
    (void)data;
    ++record_released;
    if (!pthread_equal(parsing.owner, pthread_self()))
        ++record_wrong_thread;
    if (storage->owner != NULL) {
        lonejson_cleanup(storage->view.schema.map, storage->owner);
        free(storage->owner);
    }
}
static void note_allocation(size_t size)
{
    int expected = 0;
    if (size > 32768 && parsing.ready &&
        !pthread_equal(parsing.owner, pthread_self()) &&
        __atomic_compare_exchange_n(&parsing.notified, &expected, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        (void)write(parsing.pipe[1], "p", 1);
}
static void *allocate(void *context, size_t size)
{
    void *result = malloc(size);
    (void)context;
    if (result != NULL)
        note_allocation(size);
    return result;
}
static void *resize(void *context, void *pointer, size_t size)
{
    void *result = realloc(pointer, size);
    (void)context;
    if (result != NULL)
        note_allocation(size);
    return result;
}
static void deallocate(void *context, void *pointer)
{
    (void)context;
    free(pointer);
}
static rotmos_status live_before(rotmos_request *request, rotmos_reply *reply,
                                 int *finish, void *data, rotmos_error *error)
{
    (void)request;
    (void)reply;
    (void)finish;
    (void)data;
    (void)error;
    __atomic_store_n(&parsing.notified, 0, __ATOMIC_RELEASE);
    return ROTMOS_OK;
}
static rotmos_status worker_start(rotmos_server *server,
                                  const rotmos_process_context *process,
                                  void *data, rotmos_error *error)
{
    (void)server;
    (void)process;
    (void)data;
    (void)error;
    parsing.owner = pthread_self();
    parsing.ready = 1;
    return ROTMOS_OK;
}
static reference_status live_echo(struct reference_app *app,
                                  struct reference_request *request,
                                  void *input, struct reference_response *reply,
                                  void *data, reference_error *error)
{
    struct input *in = input;
    (void)data;
    if (app == NULL || reference_request_kore(request) == NULL || in == NULL ||
        in->message == NULL || strlen(in->message) != 32768 ||
        !pthread_equal(parsing.owner, pthread_self()))
        return REFERENCE_STATE;
    ++live_called;
    return reference_response_text(reply, 200, "text/plain", "parsed live",
                                   error);
}
struct fixture {
    struct reference_app app;
    pid_t child;
};
static rotmos_status echo(rotmos_request *request,
                          const lonejson_record_view *input,
                          rotmos_reply *reply, void *data, rotmos_error *error)
{
    struct input *in;
    rotmos_xml_source_config output;
    rotmos_bytes bytes;
    rotmos_u64 now;
    (void)data;
    if (input == NULL)
        return ROTMOS_STATE;
    in = input->record;
    ++called;
    if (rotmos_body_bytes(rotmos_request_body(request), &bytes, NULL) ==
            ROTMOS_OK ||
        rotmos_loop_now(rotmos_request_loop(request), &now, error) != ROTMOS_OK)
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
static rotmos_status control(rotmos_request *request,
                             const lonejson_record_view *input,
                             rotmos_reply *reply, void *data,
                             rotmos_error *error)
{
    rotmos_u64 now;
    (void)reply;
    if (input == NULL ||
        rotmos_loop_now(rotmos_request_loop(request), &now, error) != ROTMOS_OK)
        return ROTMOS_STATE;
    if (data != NULL)
        return ROTMOS_NOT_SUPPORTED;
    ++empty_called;
    return ROTMOS_OK;
}
static rotmos_status stop(rotmos_request *request, rotmos_reply *reply,
                          void *data, rotmos_error *error)
{
    (void)data;
    stopped =
        called == 3 && replaced == 1 && live_called == 1 && empty_called == 1;
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
                    int withheld)
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
                   strcmp(path, "/stop") == 0 ? "GET" : "POST", path,
                   (unsigned long)strlen(body));
    CHECK(write_all(fd, request, strlen(request)) == 0);
    if (strcmp(path, "/live") == 0) {
        struct pollfd observed;
        char acknowledged;
        size_t prefix = strlen(body) - strlen("</ignored></doc>");
        CHECK(write_all(fd, body, prefix) == 0);
        observed.fd = parsing.pipe[0];
        observed.events = POLLIN;
        observed.revents = 0;
        CHECK(poll(&observed, 1, 1500) == 1 &&
              read(parsing.pipe[0], &acknowledged, 1) == 1 &&
              acknowledged == 'p');
        if (withheld < 0) {
            close(fd);
            return 0;
        }
        CHECK(write_all(fd, body + prefix, strlen(body) - prefix) == 0);
    } else
        CHECK(write_all(fd, body, strlen(body) - (withheld ? 1 : 0)) == 0);
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
    if (strcmp(result, expected) != 0)
        fprintf(stderr, "%s body: %s\n", path, result);
    CHECK(strcmp(result, expected) == 0);
    n = read(fd, ending, 1);
    /* Rejecting an unfinished upload may reset the unread TCP input. */
    CHECK(n == 0 || (withheld > 0 && n == -1 && errno == ECONNRESET));
    free(result);
    (void)close(fd);
    return 0;
}
static int client(unsigned short port)
{
    char *live = calloc(50000, 1);
    CHECK(live != NULL);
    strcpy(live, "<doc><message>");
    memset(live + strlen(live), 'x', 32768);
    strcat(live, "</message><ignored>");
    memset(live + strlen(live), ' ', 8192);
    strcat(live, "</ignored></doc>");
    CHECK(exchange(port, "/live", live, 0, "", 0, -1) == 0);
    CHECK(exchange(port, "/live", live, 200, "parsed live", 0, 0) == 0);
    CHECK(exchange(port, "/provider-fail",
                   "<doc><message>unused</message></doc>", 500, "", 0, 0) == 0);
    /* A wrong root is rejected without receiving the declared final byte. */
    memset(live, 0, 50000);
    strcpy(live, "<wrong>");
    memset(live + strlen(live), ' ', 8192);
    strcat(live, "</wrong>");
    CHECK(exchange(port, "/api/echo", live, 400, "", 1, 1) == 0);
    free(live);
    CHECK(exchange(port, "/api/echo",
                   "<doc><message>hello &amp; XML</message></doc>", 200,
                   "<doc><message>hello &amp; XML</message></doc>", 1, 0) == 0);
    CHECK(exchange(port, "/api/echo",
                   "<doc><message>allocated</message><message/></doc>", 400, "",
                   0, 0) == 0);
    CHECK(exchange(port, "/small-queue", "<doc><message>file</message></doc>",
                   200, "<doc><message>file</message></doc>", 0, 0) == 0);
    CHECK(exchange(port, "/replace", "<doc><message>hidden</message></doc>",
                   201, "replaced", 0, 0) == 0);
    CHECK(exchange(port, "/deny", "<", 403, "denied", 0, 0) == 0);
    CHECK(exchange(port, "/limit", "<doc><message>too large</message></doc>",
                   413, "request body exceeds route limit", 0, 0) == 0);
    CHECK(exchange(port, "/api/echo",
                   "<!DOCTYPE doc [<!ENTITY x "
                   "'entity'>]><doc><message>&x;</message></doc>",
                   400, "", 0, 0) == 0);
    CHECK(exchange(port, "/empty", "<doc><message>x</message></doc>", 204, "",
                   0, 0) == 0);
    CHECK(exchange(port, "/unsupported", "<doc><message>x</message></doc>", 501,
                   "", 0, 0) == 0);
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
    return stopped && record_acquired == 3 && record_released == 3 &&
                   record_wrong_thread == 0
               ? 0
               : 1;
}
int main(void)
{
    struct fixture fixture;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_xml_route_config xml;
    rotmos_record_provider provider;
    struct reference_typed_route forward;
    struct reference_xml_config host_xml;
    reference_error host_error;
    lonejson_allocator allocator;
    lonejson_config policy;
    lonejson_error native;
    lonejson *runtime;
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
    CHECK(pipe(parsing.pipe) == 0);
    allocator = lonejson_default_allocator();
    allocator.malloc_fn = allocate;
    allocator.realloc_fn = resize;
    allocator.free_fn = deallocate;
    policy = lonejson_default_config();
    policy.allocator = &allocator;
    runtime = lonejson_new(&policy, &native);
    CHECK(runtime != NULL);
    fixture.child = -1;
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
    config.callbacks.on_worker_start = worker_start;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_exit = worker_exit;
    config.callbacks.userdata = &fixture;
    reference_route_callbacks(&fixture.app, &config.callbacks);
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    fixture.app.server = server;
    CHECK(rotmos_json_schema_from_map(NULL, &input_map, &input, &error) ==
          ROTMOS_OK);
    rotmos_route_group_config_init(&group_config);
    group_config.prefix = "/api";
    group_config.hooks.before = before;
    CHECK(rotmos_router_group(rotmos_server_router(server), &group_config,
                              &group, &error) == ROTMOS_OK);
    rotmos_xml_route_config_init(&xml);
    CHECK(xml.body.mode == ROTMOS_BODY_STREAMING_UPLOAD);
    provider.acquire = acquire_record;
    provider.release = release_record;
    memset(&forward, 0, sizeof(forward));
    forward.app = &fixture.app;
    forward.runtime = runtime;
    forward.records = &provider;
    forward.hooks.before = live_before;
    memset(&host_xml, 0, sizeof(host_xml));
    host_xml.method = ROTMOS_HTTP_POST;
    host_xml.path = "/live";
    host_xml.body.mode = ROTMOS_BODY_STREAMING_UPLOAD;
    host_xml.body.memory_buffer_limit_bytes = 262144UL;
    host_xml.buffer_bytes = 2048;
    host_xml.input_map = &input_map;
    host_xml.input_size = sizeof(struct input);
    rotmos_xml_config_init(&host_xml.config);
    host_xml.config.root_element = "doc";
    host_xml.handler = live_echo;
    CHECK(reference_xml_register(&forward, &host_xml, &host_error) ==
          REFERENCE_OK);
    host_xml.path = "/provider-fail";
    CHECK(reference_xml_register(&forward, &host_xml, &host_error) ==
          REFERENCE_OK);
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
    xml.path = "/small-queue";
    xml.buffer_bytes = 8;
    CHECK(rotmos_router_xml_route(rotmos_server_router(server), &xml, NULL,
                                  &error) == ROTMOS_OK);
    xml.path = "/unsupported";
    xml.body.max_bytes.low = 0;
    xml.handler = control;
    xml.userdata = "unsupported";
    CHECK(rotmos_router_xml_route(rotmos_server_router(server), &xml, NULL,
                                  &error) == ROTMOS_OK);
    xml.path = "/empty";
    xml.method = ROTMOS_HTTP_POST;
    xml.userdata = NULL;
    CHECK(rotmos_router_xml_route(rotmos_server_router(server), &xml, NULL,
                                  &error) == ROTMOS_OK);
    xml.path = "/replace";
    xml.method = ROTMOS_HTTP_POST;
    xml.handler = echo;
    xml.buffer_bytes = 262144;
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
    xml.body.max_bytes.low = 16;
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
        return 1;
    }
    CHECK(result.recovered_failures.high == 0 &&
          result.recovered_failures.low == 0 && !result.escalated);
    CHECK(waitpid(fixture.child, &child_status, 0) == fixture.child &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    lonejson_free(runtime);
    close(parsing.pipe[0]);
    close(parsing.pipe[1]);
    return 0;
}
