#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <knurl/lonejson.h>
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
static const lonejson_map cycle_map;
static const lonejson_field cycle_fields[] = {
    LONEJSON_FIELD_OBJECT_REQ(struct input, message, "child", &cycle_map)};
LONEJSON_MAP_DEFINE(cycle_map, struct input, cycle_fields);
static int called, replaced, stopped, live_called, empty_called;
static struct {
    pthread_t owner;
    int ready, notified, pipe[2];
} parsing;
static int record_acquired, record_released, record_wrong_thread;
static knurl_status acquire_record(knurl_request *request, int output,
                                   const lonejson_schema_view *schema,
                                   knurl_record_storage *storage, void *data,
                                   knurl_error *error)
{
    knurl_bytes path = knurl_request_path(request);
    (void)data;
    (void)error;
    ++record_acquired;
    if (!pthread_equal(parsing.owner, pthread_self()) || output)
        return KNURL_STATE;
    storage->view.size = sizeof(storage->view);
    storage->view.abi_version = LONEJSON_VIEW_ABI_VERSION;
    storage->view.schema = *schema;
    storage->view.record = calloc(1, schema->record_size);
    storage->owner = storage->view.record;
    if (storage->owner == NULL)
        return KNURL_NOMEM;
    if (lonejson_init(schema->runtime, schema->map, storage->owner, NULL) !=
        LONEJSON_STATUS_OK)
        abort();
    return path.length == sizeof("/provider-fail") - 1 &&
                   memcmp(path.data, "/provider-fail", path.length) == 0
               ? KNURL_IO
               : KNURL_OK;
}
static void release_record(knurl_record_storage *storage, void *data)
{
    (void)data;
    ++record_released;
    if (!pthread_equal(parsing.owner, pthread_self()))
        ++record_wrong_thread;
    if (storage->owner != NULL) {
        if (lonejson_cleanup(storage->view.schema.map, storage->owner, NULL) !=
            LONEJSON_STATUS_OK)
            abort();
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
static knurl_status live_before(knurl_request *request, knurl_reply *reply,
                                int *finish, void *data, knurl_error *error)
{
    (void)request;
    (void)reply;
    (void)finish;
    (void)data;
    (void)error;
    __atomic_store_n(&parsing.notified, 0, __ATOMIC_RELEASE);
    return KNURL_OK;
}
static knurl_status worker_start(knurl_server *server,
                                 const knurl_process_context *process,
                                 void *data, knurl_error *error)
{
    (void)server;
    (void)process;
    (void)data;
    (void)error;
    parsing.owner = pthread_self();
    parsing.ready = 1;
    return KNURL_OK;
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
static knurl_status echo(knurl_request *request,
                         const lonejson_record_view *input, knurl_reply *reply,
                         void *data, knurl_error *error)
{
    struct input *in;
    knurl_xml_source_config output;
    knurl_bytes bytes;
    knurl_u64 now;
    (void)data;
    if (input == NULL)
        return KNURL_STATE;
    in = input->record;
    ++called;
    if (knurl_body_bytes(knurl_request_body(request), &bytes, NULL) ==
            KNURL_OK ||
        knurl_loop_now(knurl_request_loop(request), &now, error) != KNURL_OK)
        return KNURL_STATE;
    if (in->message == NULL)
        return KNURL_STATE;
    knurl_xml_source_config_init(&output);
    output.record = input;
    output.xml.root_element = "doc";
    return knurl_reply_xml(reply, 200, &output, error);
}
static knurl_status before(knurl_request *r, knurl_reply *reply, int *finish,
                           void *data, knurl_error *error)
{
    (void)r;
    (void)finish;
    (void)data;
    return knurl_reply_header_add(reply, "X-Before", "yes", error);
}
static knurl_status after(knurl_request *r, knurl_reply *reply, int *finish,
                          void *data, knurl_error *error)
{
    (void)r;
    (void)finish;
    (void)data;
    if (knurl_reply_payload_kind(reply) != KNURL_REPLY_SOURCE)
        return KNURL_STATE;
    ++replaced;
    return knurl_reply_text(reply, 201, "text/plain", "replaced", error);
}
static knurl_status auth(knurl_request *r, knurl_reply *reply, int *finish,
                         void *data, knurl_error *error)
{
    (void)r;
    (void)data;
    *finish = 1;
    return knurl_reply_text(reply, 403, "text/plain", "denied", error);
}
static knurl_status control(knurl_request *request,
                            const lonejson_record_view *input,
                            knurl_reply *reply, void *data, knurl_error *error)
{
    knurl_u64 now;
    (void)reply;
    if (input == NULL ||
        knurl_loop_now(knurl_request_loop(request), &now, error) != KNURL_OK)
        return KNURL_STATE;
    if (data != NULL)
        return KNURL_NOT_SUPPORTED;
    ++empty_called;
    return KNURL_OK;
}
static knurl_status stop(knurl_request *request, knurl_reply *reply, void *data,
                         knurl_error *error)
{
    (void)data;
    stopped =
        called == 3 && replaced == 1 && live_called == 1 && empty_called == 1;
    if (!stopped)
        return KNURL_STATE;
    if (knurl_server_request_stop(knurl_request_server(request), error) !=
        KNURL_OK)
        return KNURL_STATE;
    return knurl_reply_text(reply, 200, "text/plain", "stopped", error);
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
                    int withheld, int late_body)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char metadata[4096], request[512], ending[2];
    char *result, *length;
    size_t used, total, count;
    ssize_t n;
    int fd, early_rejection;
    early_rejection = strcmp(path, "/deny") == 0 || strcmp(path, "/limit") == 0;
    CHECK(!late_body || early_rejection);
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
    } else if (!late_body)
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
    if (late_body) {
        /* Exercise a body arriving after rejection, without a timing delay.
         * The peer may already have closed, but its full reply is still
         * required. */
        n = write(fd, body, strlen(body));
        CHECK(n == (ssize_t)strlen(body) ||
              (n == -1 && (errno == EPIPE || errno == ECONNRESET)));
    }
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
    /* Header rejection can leave unread input even if the whole body was sent.
     * Status, framing and body have already been checked; only the terminal
     * reset is permitted here, not truncated replies or other socket errors. */
    CHECK(n == 0 || ((withheld > 0 || early_rejection) && n == -1 &&
                     errno == ECONNRESET));
    free(result);
    (void)close(fd);
    return 0;
}
static int client(unsigned short port)
{
    char *live = calloc(50000, 1);
    CHECK(live != NULL);
    CHECK(exchange(port, "/init-fail", "<doc/>", 500, "", 0, 0, 0) == 0);
    strcpy(live, "<doc><message>");
    memset(live + strlen(live), 'x', 32768);
    strcat(live, "</message><ignored>");
    memset(live + strlen(live), ' ', 8192);
    strcat(live, "</ignored></doc>");
    CHECK(exchange(port, "/live", live, 0, "", 0, -1, 0) == 0);
    CHECK(exchange(port, "/live", live, 200, "parsed live", 0, 0, 0) == 0);
    CHECK(exchange(port, "/provider-fail",
                   "<doc><message>unused</message></doc>", 500, "", 0, 0,
                   0) == 0);
    /* A wrong root is rejected without receiving the declared final byte. */
    memset(live, 0, 50000);
    strcpy(live, "<wrong>");
    memset(live + strlen(live), ' ', 8192);
    strcat(live, "</wrong>");
    CHECK(exchange(port, "/api/echo", live, 400, "", 1, 1, 0) == 0);
    free(live);
    CHECK(exchange(port, "/api/echo",
                   "<doc><message>hello &amp; XML</message></doc>", 200,
                   "<doc><message>hello &amp; XML</message></doc>", 1, 0,
                   0) == 0);
    CHECK(exchange(port, "/api/echo",
                   "<doc><message>allocated</message><message/></doc>", 400, "",
                   0, 0, 0) == 0);
    CHECK(exchange(port, "/small-queue", "<doc><message>file</message></doc>",
                   200, "<doc><message>file</message></doc>", 0, 0, 0) == 0);
    CHECK(exchange(port, "/replace", "<doc><message>hidden</message></doc>",
                   201, "replaced", 0, 0, 0) == 0);
    CHECK(exchange(port, "/deny", "<", 403, "denied", 0, 0, 0) == 0);
    CHECK(exchange(port, "/limit", "<doc><message>too large</message></doc>",
                   413, "request body exceeds route limit", 0, 0, 0) == 0);
    CHECK(exchange(port, "/deny", "<", 403, "denied", 0, 0, 1) == 0);
    CHECK(exchange(port, "/limit", "<doc><message>too large</message></doc>",
                   413, "request body exceeds route limit", 0, 0, 1) == 0);
    CHECK(exchange(port, "/api/echo",
                   "<!DOCTYPE doc [<!ENTITY x "
                   "'entity'>]><doc><message>&x;</message></doc>",
                   400, "", 0, 0, 0) == 0);
    CHECK(exchange(port, "/empty", "<doc><message>x</message></doc>", 204, "",
                   0, 0, 0) == 0);
    CHECK(exchange(port, "/unsupported", "<doc><message>x</message></doc>", 501,
                   "", 0, 0, 0) == 0);
    CHECK(exchange(port, "/stop", "", 200, "stopped", 0, 0, 0) == 0);
    return 0;
}
static knurl_status ready(knurl_server *server, const knurl_process_context *p,
                          void *data, knurl_error *error)
{
    struct fixture *f;
    knurl_bound_listener listener;
    int result;
    (void)p;
    f = data;
    if (knurl_server_listener_at(server, 0, &listener, error) != KNURL_OK)
        return KNURL_INVALID;
    if (knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return KNURL_STATE;
    f->child = fork();
    if (f->child == 0) {
        (void)signal(SIGTERM, SIG_DFL);
        result = client(listener.port);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    return f->child < 0 ? KNURL_IO : KNURL_OK;
}
static int worker_exit(knurl_server *server, const knurl_process_context *p,
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
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_xml_route_config xml;
    knurl_record_provider provider;
    struct reference_typed_route forward;
    struct reference_xml_config host_xml;
    reference_error host_error;
    lonejson_allocator allocator;
    lonejson_config policy;
    lonejson_error native;
    lonejson *runtime;
    knurl_route_group_config group_config;
    knurl_route_group *group;
    knurl_route_config ordinary;
    knurl_server *server;
    knurl_error error;
    knurl_run_result result;
    knurl_status status;
    lonejson_schema_view input, invalid;
    knurl_xml_route_config failed;
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
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.io.output_queue_bytes = 4096;
    config.io.chunk_bytes = 128;
    config.callbacks.on_worker_start = worker_start;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_exit = worker_exit;
    config.callbacks.userdata = &fixture;
    reference_route_callbacks(&fixture.app, &config.callbacks);
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    fixture.app.server = server;
    CHECK(knurl_json_schema_from_map(NULL, &input_map, &input, &error) ==
          KNURL_OK);
    knurl_route_group_config_init(&group_config);
    group_config.prefix = "/api";
    group_config.hooks.before = before;
    CHECK(knurl_router_group(knurl_server_router(server), &group_config, &group,
                             &error) == KNURL_OK);
    knurl_xml_route_config_init(&xml);
    CHECK(xml.body.mode == KNURL_BODY_STREAMING_UPLOAD);
    provider.acquire = acquire_record;
    provider.release = release_record;
    memset(&forward, 0, sizeof(forward));
    forward.app = &fixture.app;
    forward.runtime = runtime;
    forward.records = &provider;
    forward.hooks.before = live_before;
    memset(&host_xml, 0, sizeof(host_xml));
    host_xml.method = KNURL_HTTP_POST;
    host_xml.path = "/live";
    host_xml.body.mode = KNURL_BODY_STREAMING_UPLOAD;
    host_xml.body.memory_buffer_limit_bytes = 262144UL;
    host_xml.buffer_bytes = 2048;
    host_xml.input_map = &input_map;
    host_xml.input_size = sizeof(struct input);
    knurl_xml_config_init(&host_xml.config);
    host_xml.config.root_element = "doc";
    host_xml.handler = live_echo;
    CHECK(reference_xml_register(&forward, &host_xml, &host_error) ==
          REFERENCE_OK);
    host_xml.path = "/provider-fail";
    CHECK(reference_xml_register(&forward, &host_xml, &host_error) ==
          REFERENCE_OK);
    knurl_xml_route_config_init(&xml);
    xml.method = KNURL_HTTP_POST;
    xml.path = path;
    xml.input = &input;
    xml.handler = echo;
    xml.xml.root_element = root;
    CHECK(knurl_route_group_xml_route(group, &xml, NULL, &error) == KNURL_OK);
    strcpy(path, "/changed");
    strcpy(root, "changed");
    xml.xml.root_element = "doc";
    xml.path = "/small-queue";
    xml.buffer_bytes = 8;
    CHECK(knurl_router_xml_route(knurl_server_router(server), &xml, NULL,
                                 &error) == KNURL_OK);
    failed = xml;
    failed.path = "/init-fail";
    CHECK(knurl_json_schema_from_map(NULL, &cycle_map, &invalid, &error) ==
          KNURL_OK);
    failed.input = &invalid;
    CHECK(knurl_router_xml_route(knurl_server_router(server), &failed, NULL,
                                 &error) == KNURL_OK);
    xml.path = "/unsupported";
    xml.body.max_bytes.low = 0;
    xml.handler = control;
    xml.userdata = "unsupported";
    CHECK(knurl_router_xml_route(knurl_server_router(server), &xml, NULL,
                                 &error) == KNURL_OK);
    xml.path = "/empty";
    xml.method = KNURL_HTTP_POST;
    xml.userdata = NULL;
    CHECK(knurl_router_xml_route(knurl_server_router(server), &xml, NULL,
                                 &error) == KNURL_OK);
    xml.path = "/replace";
    xml.method = KNURL_HTTP_POST;
    xml.handler = echo;
    xml.buffer_bytes = 262144;
    xml.userdata = NULL;
    xml.hooks.after = after;
    CHECK(knurl_router_xml_route(knurl_server_router(server), &xml, NULL,
                                 &error) == KNURL_OK);
    xml.path = "/deny";
    xml.hooks.after = NULL;
    xml.hooks.auth = auth;
    CHECK(knurl_router_xml_route(knurl_server_router(server), &xml, NULL,
                                 &error) == KNURL_OK);
    xml.path = "/limit";
    xml.hooks.auth = NULL;
    xml.body.max_bytes.low = 16;
    CHECK(knurl_router_xml_route(knurl_server_router(server), &xml, NULL,
                                 &error) == KNURL_OK);
    knurl_route_config_init(&ordinary);
    ordinary.method = KNURL_HTTP_GET;
    ordinary.path = "/stop";
    ordinary.handler = stop;
    CHECK(knurl_router_route(knurl_server_router(server), &ordinary, NULL,
                             &error) == KNURL_OK);
    status = knurl_server_run(server, &result, &error);
    if (status != KNURL_OK) {
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
        if (fixture.child > 0) {
            kill(fixture.child, SIGTERM);
            waitpid(fixture.child, NULL, 0);
        }
        knurl_server_destroy(server, NULL);
        return 1;
    }
    CHECK(result.recovered_failures.high == 0 &&
          result.recovered_failures.low == 0 && !result.escalated);
    CHECK(waitpid(fixture.child, &child_status, 0) == fixture.child &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    lonejson_free(runtime);
    close(parsing.pipe[0]);
    close(parsing.pipe[1]);
    return 0;
}
