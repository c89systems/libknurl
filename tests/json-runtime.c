#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <knurl/lonejson.h>
#include "vectis-forward.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            (void)fprintf(stderr, "JSON wire %d: %s\n", __LINE__, #t);         \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct input {
    char *message;
};
struct output {
    char result[64];
};
struct problem {
    char reason[64];
};
static const lonejson_field input_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct input, message, "message")};
static const lonejson_field output_fields[] = {LONEJSON_FIELD_STRING_FIXED_REQ(
    struct output, result, "result", LONEJSON_OVERFLOW_FAIL)};
static const lonejson_field problem_fields[] = {LONEJSON_FIELD_STRING_FIXED_REQ(
    struct problem, reason, "reason", LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(input_map, struct input, input_fields);
static const lonejson_map cycle_map;
static const lonejson_field cycle_fields[] = {
    LONEJSON_FIELD_OBJECT_REQ(struct input, message, "child", &cycle_map)};
LONEJSON_MAP_DEFINE(cycle_map, struct input, cycle_fields);
LONEJSON_MAP_DEFINE(output_map, struct output, output_fields);
LONEJSON_MAP_DEFINE(problem_map, struct problem, problem_fields);
static lonejson_schema_view problem_schema;
static int called, typed_called, replaced, stopped, default_spool_directory;
static int array_called;
static int record_acquired, record_released;
struct external_record {
    lonejson *runtime;
    const lonejson_map *map;
    void *record;
};
static knurl_status acquire_record(knurl_request *request, int output,
                                   const lonejson_schema_view *schema,
                                   knurl_record_storage *storage, void *data,
                                   knurl_error *error)
{
    struct external_record *owner = calloc(1, sizeof(*owner));
    lonejson_error native;
    knurl_u64 now;
    knurl_bytes path = knurl_request_path(request);
    (void)data;
    ++record_acquired;
    storage->owner = owner;
    if (owner == NULL)
        return KNURL_NOMEM;
    if (knurl_loop_now(knurl_request_loop(request), &now, error) != KNURL_OK)
        return KNURL_STATE;
    if (path.length == sizeof("/provider-fail") - 1 &&
        memcmp(path.data, "/provider-fail", path.length) == 0)
        return KNURL_IO;
    owner->runtime = lonejson_new(NULL, &native);
    if (output && path.length == sizeof("/provider-output-fail") - 1 &&
        memcmp(path.data, "/provider-output-fail", path.length) == 0)
        return KNURL_IO;
    owner->map = schema->map;
    owner->record = calloc(1, schema->record_size);
    if (owner->runtime == NULL || owner->record == NULL)
        return KNURL_NOMEM;
    if (lonejson_init(owner->runtime, owner->map, owner->record, NULL) !=
        LONEJSON_STATUS_OK)
        abort();
    storage->view.size = sizeof(storage->view);
    storage->view.abi_version = LONEJSON_VIEW_ABI_VERSION;
    storage->view.schema = *schema;
    storage->view.schema.runtime = owner->runtime;
    storage->view.record = owner->record;
    if (path.length == sizeof("/provider-wrong") - 1 &&
        memcmp(path.data, "/provider-wrong", path.length) == 0)
        storage->view.size = 0;
    if ((output && schema->map != &output_map) ||
        (!output && schema->map != &input_map))
        return KNURL_INVALID;
    return KNURL_OK;
}
static void release_record(knurl_record_storage *storage, void *data)
{
    struct external_record *owner = storage->owner;
    (void)data;
    ++record_released;
    if (owner != NULL) {
        if (owner->record != NULL && owner->runtime != NULL)
            if (lonejson_cleanup(owner->map, owner->record, NULL) !=
                LONEJSON_STATUS_OK)
                abort();
        free(owner->record);
        lonejson_free(owner->runtime);
        free(owner);
    }
}
struct array_fixture {
    struct input *storage;
    size_t count;
    int fail;
};
static reference_status array_item(void *data, size_t index, void *record,
                                   reference_error *error)
{
    struct array_fixture *fixture = data;
    struct input *input = record;
    if (record != fixture->storage || index != fixture->count ||
        strcmp(input->message, index == 0 ? "one" : "two") != 0)
        return REFERENCE_STATE;
    ++fixture->count;
    if (fixture->fail) {
        reference_error_clear(error);
        error->code = REFERENCE_STATE;
        error->source = 1;
        error->dependency_code = 77;
        strcpy(error->message, "array stopped");
        return REFERENCE_STATE;
    }
    return REFERENCE_OK;
}
static knurl_status array_route(knurl_request *request, knurl_reply *reply,
                                void *data, knurl_error *error)
{
    struct input item;
    struct array_fixture fixture;
    reference_status status;
    reference_error host_error;
    char first;
    size_t count;
    memset(&item, 0, sizeof(item));
    memset(&fixture, 0, sizeof(fixture));
    fixture.storage = &item;
    fixture.fail = 1;
    if (data != NULL) {
        struct reference_request *host = knurl_request_userdata(request);
        if (knurl_request_body(request) != NULL ||
            reference_request_array_each(host, "items", &input_map, &item,
                                         array_item, &fixture,
                                         &host_error) != REFERENCE_INVALID ||
            host_error.source != 1 ||
            strstr(host_error.message, "reader") == NULL ||
            reference_request_json_into(host, &input_map, &item, &host_error) !=
                REFERENCE_INVALID ||
            reference_request_json_into(host, &input_map, &item, NULL) !=
                REFERENCE_INVALID)
            return KNURL_STATE;
        ++array_called;
        return knurl_reply_text(reply, 200, "text/plain", "missing reader",
                                error);
    }
    if (knurl_body_kind(knurl_request_body(request)) != KNURL_BODY_FILE)
        return KNURL_STATE;
    status = reference_request_array_each(knurl_request_userdata(request),
                                          "items", &input_map, &item,
                                          array_item, &fixture, &host_error);
    if (status != REFERENCE_STATE || host_error.dependency_code != 77 ||
        strcmp(host_error.message, "array stopped") || fixture.count != 1 ||
        item.message != NULL ||
        knurl_body_read(knurl_request_body(request), &first, 1, &count,
                        error) != KNURL_OK ||
        first != '{' || count != 1)
        return KNURL_STATE;
    fixture.fail = 0;
    fixture.count = 0;
    status = reference_request_array_each(knurl_request_userdata(request),
                                          "items", &input_map, &item,
                                          array_item, &fixture, &host_error);
    if (status != REFERENCE_OK || fixture.count != 2 || item.message != NULL ||
        knurl_body_read(knurl_request_body(request), &first, 1, &count,
                        error) != KNURL_OK ||
        first != '{' || count != 1)
        return KNURL_STATE;
    ++array_called;
    return knurl_reply_text(reply, 200, "text/plain", "array", error);
}
struct fixture {
    struct reference_app app;
    pid_t child;
    char directory[64];
};
static knurl_status echo(knurl_request *request,
                         const lonejson_record_view *input,
                         const lonejson_record_view *output, void *data,
                         knurl_error *error)
{
    struct input *in;
    struct output *out;
    size_t count;
    char first;
    in = input != NULL ? input->record : NULL;
    out = output != NULL ? output->record : NULL;
    if (in == NULL || out == NULL || input->schema.map != &input_map ||
        output->schema.map != &output_map || input->schema.runtime == NULL)
        return KNURL_INVALID;
    ++called;
    if (data != NULL && strcmp(data, "spilled") == 0 &&
        knurl_body_kind(knurl_request_body(request)) != KNURL_BODY_FILE)
        return KNURL_STATE;
    if (knurl_request_json_validate(request, NULL, error) != KNURL_OK ||
        knurl_body_read(knurl_request_body(request), &first, 1, &count,
                        error) != KNURL_OK ||
        count != 1 || first != '{' ||
        knurl_body_rewind(knurl_request_body(request), error) != KNURL_OK)
        return KNURL_STATE;
    (void)snprintf(out->result, sizeof(out->result), "%s", in->message);
    return KNURL_OK;
}
static knurl_status unexpected(knurl_request *request,
                               const lonejson_record_view *input,
                               const lonejson_record_view *output, void *data,
                               knurl_error *error)
{
    (void)request;
    (void)input;
    (void)output;
    (void)data;
    (void)error;
    return KNURL_BACKEND;
}
static reference_status host_echo(struct reference_app *app,
                                  struct reference_request *request,
                                  void *input, void *output, void *data,
                                  reference_error *error)
{
    const struct input *in = input;
    struct output *out = output;
    struct input repeated;
    if (app == NULL || reference_request_kore(request) == NULL || in == NULL ||
        out == NULL || data == NULL || strcmp(data, "forwarded") != 0)
        return REFERENCE_STATE;
    memset(&repeated, 0, sizeof(repeated));
    if (reference_request_json_into(request, &input_map, &repeated, error) !=
            REFERENCE_OK ||
        repeated.message == NULL || strcmp(repeated.message, in->message)) {
        if (lonejson_cleanup(&input_map, &repeated, NULL) != LONEJSON_STATUS_OK)
            abort();
        return REFERENCE_STATE;
    }
    if (lonejson_cleanup(&input_map, &repeated, NULL) != LONEJSON_STATUS_OK)
        abort();
    ++called;
    (void)snprintf(out->result, sizeof(out->result), "%s", in->message);
    return REFERENCE_OK;
}
static reference_status host_typed(struct reference_app *app,
                                   struct reference_request *request,
                                   void *input,
                                   struct reference_response *reply, void *data,
                                   reference_error *error)
{
    struct input *in = input;
    struct problem problem;
    if (app == NULL || reference_request_kore(request) == NULL || in == NULL)
        return REFERENCE_STATE;
    ++typed_called;
    memset(&problem, 0, sizeof(problem));
    (void)snprintf(problem.reason, sizeof(problem.reason), "%s", in->message);
    if (data != NULL)
        return reference_response_json_generated(reply, 422, &problem_map,
                                                 &problem, error);
    return reference_json_reply(reply, 422, &problem_map, &problem, error);
}
static knurl_status typed(knurl_request *request,
                          const lonejson_record_view *input, knurl_reply *reply,
                          void *data, knurl_error *error)
{
    struct input *in;
    struct problem problem;
    knurl_json_source_config source;
    (void)request;
    ++typed_called;
    if (data != NULL && strcmp(data, "empty") == 0)
        return input == NULL ? KNURL_OK : KNURL_INVALID;
    if (input == NULL)
        return KNURL_INVALID;
    in = input->record;
    if (data != NULL && strcmp(data, "borrow") == 0) {
        knurl_json_source_config_init(&source);
        source.record = *input;
        return knurl_reply_json(reply, 200, &source, error);
    }
    memset(&problem, 0, sizeof(problem));
    (void)snprintf(problem.reason, sizeof(problem.reason), "%s", in->message);
    return knurl_reply_json_materialized(reply, 422, &problem_schema, &problem,
                                         128, error);
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
    if (knurl_reply_payload_kind(reply) != KNURL_REPLY_BYTES ||
        knurl_reply_body(reply).length !=
            sizeof("{\"result\":\"hidden\"}") - 1 ||
        memcmp(knurl_reply_body(reply).data, "{\"result\":\"hidden\"}",
               sizeof("{\"result\":\"hidden\"}") - 1) != 0)
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
static knurl_status stop(knurl_request *request, knurl_reply *reply, void *data,
                         knurl_error *error)
{
    (void)data;
    stopped = called == 3 && typed_called == 4 && replaced == 1 &&
              array_called == 2 && record_acquired == 7 && record_released == 7;
    if (!stopped) {
        (void)fprintf(stderr, "JSON counts: echo=%d typed=%d replaced=%d\n",
                      called, typed_called, replaced);
        return KNURL_STATE;
    }
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
    if (large) {
        CHECK(total == 65550 && strncmp(result, "{\"message\":\"", 12) == 0 &&
              memcmp(result + total - 2, "\"}", 2) == 0);
        for (used = 12; used < total - 2; ++used)
            CHECK(result[used] == 'x');
    } else if (expected == NULL) {
        CHECK(code >= 400 && total > 0 &&
              total < sizeof(((knurl_error *)0)->message));
    } else {
        if (strcmp(result, expected) != 0)
            (void)fprintf(stderr, "%s body: %s\n", path, result);
        CHECK(strcmp(result, expected) == 0);
    }
    n = read(fd, ending, 1);
    /* A 413 can close with unread request bytes. Check the complete HTTP
     * response above before accepting its terminal reset, as in XML tests.
     */
    if (n != 0 && !(code == 413 && n == -1 && errno == ECONNRESET))
        (void)fprintf(stderr, "%s close: read %ld, errno %d\n", path, (long)n,
                      errno);
    CHECK(n == 0 || (code == 413 && n == -1 && errno == ECONNRESET));
    free(result);
    (void)close(fd);
    return 0;
}
static int client(unsigned short port, int ready_fd)
{
    char *large;
    int result;
    result = exchange(port, "/api/echo", "{\"message\":\"hello\"}", 200,
                      "{\"result\":\"hello\"}", 1, 0);
    CHECK(write_all(ready_fd, &result, sizeof(result)) == 0);
    (void)close(ready_fd);
    CHECK(result == 0);
    CHECK(exchange(port, "/api/echo",
                   "{\"message\":\"allocated\",\"message\":[]}", 400,
                   "duplicate field 'message'", 0, 0) == 0);
    CHECK(exchange(port, "/spilled", "{\"message\":\"file\"}", 200,
                   "{\"result\":\"file\"}", 0, 0) == 0);
    CHECK(exchange(port, "/init-input-fail", "{}", 400, NULL, 0, 0) == 0);
    CHECK(exchange(port, "/init-output-fail", "{\"message\":\"hidden\"}", 400,
                   NULL, 0, 0) == 0);
    CHECK(exchange(port, "/replace", "{\"message\":\"hidden\"}", 201,
                   "replaced", 0, 0) == 0);
    CHECK(exchange(port, "/provider-fail", "{\"message\":\"hidden\"}", 500, "",
                   0, 0) == 0);
    CHECK(
        exchange(port, "/provider-wrong", "{\"message\":\"hidden\"}", 400,
                 "provider must supply initialized storage matching its schema",
                 0, 0) == 0);
    CHECK(exchange(port, "/provider-malformed", "{\"message\":[]}", 400, NULL,
                   0, 0) == 0);
    CHECK(exchange(port, "/provider-output-fail", "{\"message\":\"hidden\"}",
                   500, "", 0, 0) == 0);
    CHECK(exchange(port, "/deny", "{", 403, "denied", 0, 0) == 0);
    CHECK(exchange(port, "/typed", "{\"message\":\"problem\"}", 422,
                   "{\"reason\":\"problem\"}", 0, 0) == 0);
    CHECK(exchange(port, "/generated", "{\"message\":\"generated\"}", 422,
                   "{\"reason\":\"generated\"}", 0, 0) == 0);
    CHECK(exchange(port, "/empty", "", 204, "", 0, 0) == 0);
    CHECK(exchange(port, "/missing-reader", "", 200, "missing reader", 0, 0) ==
          0);
    CHECK(exchange(port, "/array",
                   "{\"items\":[{\"message\":\"one\"},{\"message\":\"two\"}]}",
                   200, "array", 0, 0) == 0);
    CHECK(exchange(port, "/limit", "{\"message\":\"too much\"}", 413,
                   "request body exceeds route limit", 0, 0) == 0);
    CHECK(exchange(port, "/fixed",
                   "{\"result\":"
                   "\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                   "xxxxxxxxxxxxxxxxxxxxxxx\"}",
                   400, NULL, 0, 0) == 0);
    large = malloc(65551);
    CHECK(large != NULL);
    memcpy(large, "{\"message\":\"", 12);
    memset(large + 12, 'x', 65536);
    memcpy(large + 65548, "\"}", 3);
    CHECK(exchange(port, "/borrow", large, 200, "", 0, 1) == 0);
    free(large);
    CHECK(exchange(port, "/stop", "", 200, "stopped", 0, 0) == 0);
    return 0;
}
static knurl_status ready(knurl_server *server, const knurl_process_context *p,
                          void *data, knurl_error *error)
{
    struct fixture *f;
    knurl_bound_listener listener;
    struct pollfd probe;
    int result, response, gate[2];
    (void)p;
    f = data;
    if (knurl_server_listener_at(server, 0, &listener, error) != KNURL_OK)
        return KNURL_INVALID;
    if (knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return KNURL_STATE;
    if (pipe(gate) != 0)
        return KNURL_IO;
    f->child = fork();
    if (f->child == 0) {
        (void)close(gate[0]);
        (void)signal(SIGTERM, SIG_DFL);
        result = client(listener.port, gate[1]);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    (void)close(gate[1]);
    if (f->child < 0) {
        (void)close(gate[0]);
        return KNURL_IO;
    }
    /* Require an application response before this callback returns, so a
     * client racing the automatic serving transition cannot pass by chance.
     */
    memset(&probe, 0, sizeof(probe));
    probe.fd = gate[0];
    probe.events = POLLIN;
    result = poll(&probe, 1, 3000) == 1 &&
                     read_all(gate[0], &response, sizeof(response)) == 0 &&
                     response == 0
                 ? KNURL_OK
                 : KNURL_IO;
    (void)close(gate[0]);
    return result;
}
static int worker_exit(knurl_server *server, const knurl_process_context *p,
                       void *data)
{
    (void)server;
    (void)p;
    (void)data;
    return stopped ? 0 : 1;
}
int main(int argc, char **argv)
{
    struct fixture fixture;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_json_route_config json;
    knurl_json_route_config failed;
    knurl_record_provider provider;
    struct reference_typed_route forwarded, typed_forwarded, generated;
    struct reference_json_config host_json;
    struct reference_json_typed_config host_config;
    reference_error host_error;
    knurl_json_typed_route_config typed_config;
    knurl_route_group_config group_config;
    knurl_route_group *group;
    knurl_route_config ordinary;
    knurl_server *server;
    knurl_error error;
    knurl_run_result result;
    knurl_status status;
    int child_status;
    char path[16] = "/echo";
    (void)argv;
    default_spool_directory = argc > 1;
    memset(&fixture, 0, sizeof(fixture));
    fixture.child = -1;
    strcpy(fixture.directory, "json-spool-XXXXXX");
    CHECK(mkdtemp(fixture.directory) != NULL);
    (void)signal(SIGPIPE, SIG_IGN);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.io.output_queue_bytes = 4096;
    config.io.chunk_bytes = 128;
    config.limits.body_spool_directory =
        default_spool_directory ? NULL : fixture.directory;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_exit = worker_exit;
    config.callbacks.userdata = &fixture;
    reference_route_callbacks(&fixture.app, &config.callbacks);
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    fixture.app.server = server;
    CHECK(knurl_json_schema_from_map(NULL, &problem_map, &problem_schema,
                                     &error) == KNURL_OK);
    knurl_route_group_config_init(&group_config);
    group_config.prefix = "/api";
    group_config.hooks.before = before;
    CHECK(knurl_router_group(knurl_server_router(server), &group_config, &group,
                             &error) == KNURL_OK);
    knurl_json_route_config_init(&json);
    json.method = KNURL_HTTP_POST;
    json.path = path;
    CHECK(knurl_json_schema_from_map(NULL, &input_map, &json.input, &error) ==
              KNURL_OK &&
          knurl_json_schema_from_map(NULL, &output_map, &json.output, &error) ==
              KNURL_OK);
    json.handler = echo;
    CHECK(knurl_route_group_json_route(group, &json, NULL, &error) == KNURL_OK);
    strcpy(path, "/changed");
    json.path = "/spilled";
    json.body.memory_buffer_limit_bytes = 8;
    json.userdata = "spilled";
    CHECK(knurl_router_json_route(knurl_server_router(server), &json, NULL,
                                  &error) == KNURL_OK);
    failed = json;
    failed.path = "/init-input-fail";
    CHECK(knurl_json_schema_from_map(NULL, &cycle_map, &failed.input, &error) ==
          KNURL_OK);
    CHECK(knurl_router_json_route(knurl_server_router(server), &failed, NULL,
                                  &error) == KNURL_OK);
    failed = json;
    failed.path = "/init-output-fail";
    CHECK(knurl_json_schema_from_map(NULL, &cycle_map, &failed.output,
                                     &error) == KNURL_OK);
    CHECK(knurl_router_json_route(knurl_server_router(server), &failed, NULL,
                                  &error) == KNURL_OK);
    json.path = "/replace";
    json.hooks.after = after;
    json.userdata = NULL;
    json.body.memory_buffer_limit_bytes = 262144UL;
    provider.acquire = acquire_record;
    provider.release = release_record;
    json.records = &provider;
    memset(&forwarded, 0, sizeof(forwarded));
    forwarded.app = &fixture.app;
    forwarded.hooks.after = after;
    forwarded.records = &provider;
    memset(&host_json, 0, sizeof(host_json));
    host_json.method = KNURL_HTTP_POST;
    host_json.path = "/replace";
    host_json.body.mode = KNURL_BODY_BUFFERED;
    host_json.body.memory_buffer_limit_bytes = 262144UL;
    host_json.input_map = &input_map;
    host_json.input_size = sizeof(struct input);
    host_json.output_map = &output_map;
    host_json.output_size = sizeof(struct output);
    host_json.handler = host_echo;
    host_json.userdata = "forwarded";
    CHECK(reference_json_register(&forwarded, &host_json, &host_error) ==
          REFERENCE_OK);
    json.handler = unexpected;
    json.hooks.after = NULL;
    json.path = "/provider-fail";
    CHECK(knurl_router_json_route(knurl_server_router(server), &json, NULL,
                                  &error) == KNURL_OK);
    json.path = "/provider-wrong";
    CHECK(knurl_router_json_route(knurl_server_router(server), &json, NULL,
                                  &error) == KNURL_OK);
    json.path = "/provider-malformed";
    CHECK(knurl_router_json_route(knurl_server_router(server), &json, NULL,
                                  &error) == KNURL_OK);
    json.path = "/provider-output-fail";
    CHECK(knurl_router_json_route(knurl_server_router(server), &json, NULL,
                                  &error) == KNURL_OK);
    json.records = NULL;
    json.path = "/deny";
    json.hooks.after = NULL;
    json.hooks.auth = auth;
    CHECK(knurl_router_json_route(knurl_server_router(server), &json, NULL,
                                  &error) == KNURL_OK);
    json.path = "/limit";
    json.hooks.auth = NULL;
    json.body.max_bytes.low = 16;
    json.body.memory_buffer_limit_bytes = 16;
    CHECK(knurl_router_json_route(knurl_server_router(server), &json, NULL,
                                  &error) == KNURL_OK);
    json.path = "/fixed";
    json.body.max_bytes.low = 0;
    json.body.memory_buffer_limit_bytes = 262144UL;
    json.input = json.output;
    json.handler = unexpected;
    CHECK(knurl_router_json_route(knurl_server_router(server), &json, NULL,
                                  &error) == KNURL_OK);
    knurl_json_typed_route_config_init(&typed_config);
    typed_config.method = KNURL_HTTP_POST;
    CHECK(knurl_json_schema_from_map(NULL, &input_map, &typed_config.input,
                                     &error) == KNURL_OK);
    typed_config.path = "/typed";
    typed_config.handler = typed;
    memset(&typed_forwarded, 0, sizeof(typed_forwarded));
    typed_forwarded.app = &fixture.app;
    memset(&host_config, 0, sizeof(host_config));
    host_config.method = KNURL_HTTP_POST;
    host_config.path = "/typed";
    host_config.body.mode = KNURL_BODY_BUFFERED;
    host_config.body.memory_buffer_limit_bytes = 262144UL;
    host_config.input_map = &input_map;
    host_config.input_size = sizeof(struct input);
    host_config.handler = host_typed;
    CHECK(reference_json_typed_register(&typed_forwarded, &host_config,
                                        &host_error) == REFERENCE_OK);
    memset(&generated, 0, sizeof(generated));
    generated.app = &fixture.app;
    host_config.path = "/generated";
    host_config.userdata = "generated";
    CHECK(reference_json_typed_register(&generated, &host_config,
                                        &host_error) == REFERENCE_OK);
    typed_config.path = "/borrow";
    typed_config.userdata = "borrow";
    typed_config.body.memory_buffer_limit_bytes = 8;
    CHECK(knurl_router_json_typed_route(knurl_server_router(server),
                                        &typed_config, NULL,
                                        &error) == KNURL_OK);
    typed_config.path = "/empty";
    typed_config.method = KNURL_HTTP_GET;
    typed_config.input.map = NULL;
    typed_config.userdata = "empty";
    CHECK(knurl_router_json_typed_route(knurl_server_router(server),
                                        &typed_config, NULL,
                                        &error) == KNURL_OK);
    knurl_route_config_init(&ordinary);
    ordinary.method = KNURL_HTTP_POST;
    ordinary.path = "/array";
    ordinary.handler = array_route;
    knurl_body_policy_init_materialized(&ordinary.body);
    ordinary.body.memory_buffer_limit_bytes = 8;
    CHECK(knurl_router_route(knurl_server_router(server), &ordinary, NULL,
                             &error) == KNURL_OK);
    knurl_route_config_init(&ordinary);
    ordinary.method = KNURL_HTTP_POST;
    ordinary.path = "/missing-reader";
    ordinary.body.mode = KNURL_BODY_NONE;
    ordinary.handler = array_route;
    ordinary.userdata = "missing";
    CHECK(knurl_router_route(knurl_server_router(server), &ordinary, NULL,
                             &error) == KNURL_OK);
    knurl_route_config_init(&ordinary);
    ordinary.method = KNURL_HTTP_GET;
    ordinary.path = "/stop";
    ordinary.handler = stop;
    CHECK(knurl_router_route(knurl_server_router(server), &ordinary, NULL,
                             &error) == KNURL_OK);
    status = knurl_server_run(server, &result, &error);
    if (status != KNURL_OK) {
        (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
        if (fixture.child > 0) {
            (void)kill(fixture.child, SIGTERM);
            (void)waitpid(fixture.child, NULL, 0);
        }
        (void)knurl_server_destroy(server, NULL);
        (void)rmdir(fixture.directory);
        return 1;
    }
    CHECK(result.recovered_failures.high == 0 &&
          result.recovered_failures.low == 0 && !result.escalated);
    CHECK(waitpid(fixture.child, &child_status, 0) == fixture.child &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK &&
          rmdir(fixture.directory) == 0);
    return 0;
}
