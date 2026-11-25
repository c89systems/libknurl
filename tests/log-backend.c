#define _POSIX_C_SOURCE 200809L
#include "../src/kore_internal.h"
#include "acl-wire.h"
#include <knurl/pslog.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "log backend %d: %s\n", __LINE__, #t);             \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static knurl_status started(knurl_server *server,
                            const knurl_process_context *process, void *data,
                            knurl_error *error)
{
    char *large;
    (void)server;
    (void)process;
    (void)data;
    (void)error;
    large = malloc(10001);
    if (large == NULL)
        return KNURL_NOMEM;
    memset(large, 'x', 10000);
    large[10000] = 0;
    kore_log(LOG_INFO, "%s", large);
    free(large);
    /* An unfinished access record must not stall the parent drain or shutdown.
     */
    worker->lb.lock = 1;
    worker->lb.offset = 1;
    return KNURL_OK;
}
static void stop(knurl_timer *timer, void *data)
{
    (void)timer;
    knurl_server_request_stop(data, NULL);
}
static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *data,
                          knurl_error *error)
{
    knurl_timer *timer;
    (void)process;
    (void)data;
    return knurl_loop_timer(knurl_server_loop(server), 30, 0, stop, NULL,
                            server, &timer, error);
}

struct callback_fixture {
    pid_t parent, client;
    pthread_t owner;
    int notes[2];
    int runtime_seen, worker_index;
    const char *path, *agent, *peer;
};

static int read_note(int fd, char *note)
{
    struct pollfd ready;
    ready.fd = fd;
    ready.events = POLLIN;
    return poll(&ready, 1, 2500) == 1 && read(fd, note, 1) == 1;
}

static void collect(const knurl_log_record *record, void *data)
{
    struct callback_fixture *f = data;
    const knurl_log_access *access = record->access;
    char note;
    if (strcmp(record->message, "callback parent") == 0) {
        if (getpid() == f->parent && strcmp(record->process, "parent") == 0 &&
            pthread_equal(pthread_self(), f->owner))
            ++f->runtime_seen;
        return;
    }
    if (strcmp(record->message, "callback worker") == 0) {
        note = (char)('1' + f->worker_index);
        if (strncmp(record->process, "[wrk ", 5) != 0)
            note = 'x';
    } else if (access != NULL && strcmp(access->path, "/gate") == 0) {
        note = 'a';
        if (access->path != f->path || access->user_agent != f->agent ||
            access->client_ip != f->peer || record->level != KNURL_LOG_TRACE ||
            access->status != 200 || access->bytes.high != 0 ||
            access->bytes.low != 2 || strcmp(access->method, "GET") != 0 ||
            strcmp(access->client_ip, "127.0.0.1") != 0 ||
            strcmp(access->user_agent, "callback tester") != 0)
            note = 'x';
    } else
        return;
    if (getpid() == f->parent || !pthread_equal(pthread_self(), f->owner))
        note = 'x';
    (void)write(f->notes[1], &note, 1);
}

static knurl_status callback_worker(knurl_server *server,
                                    const knurl_process_context *process,
                                    void *data, knurl_error *error)
{
    struct callback_fixture *f = data;
    (void)server;
    (void)error;
    f->owner = pthread_self();
    f->worker_index = (int)process->worker_index;
    kore_log(LOG_INFO, "callback worker");
    return KNURL_OK;
}

static knurl_status callback_client(knurl_server *server,
                                    const knurl_process_context *process,
                                    void *data, knurl_error *error)
{
    struct callback_fixture *f = data;
    knurl_bound_listener listener;
    int ok;
    (void)process;
    if (knurl_server_set_log_callback(server, NULL, NULL, NULL) !=
            KNURL_STATE ||
        knurl_server_listener_at(server, 0, &listener, error) != KNURL_OK ||
        knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return KNURL_STATE;
    kore_log(LOG_INFO, "callback parent");
    f->client = fork();
    if (f->client < 0)
        return KNURL_IO;
    if (f->client == 0) {
        ok = knurl_test_acl_exchange(listener.port, "GET", "/gate",
                                     "User-Agent: callback tester\r\n",
                                     "ok") == 200;
        (void)kill(f->parent, SIGTERM);
        _exit(!ok);
    }
    return KNURL_OK;
}

static knurl_status callback_route(knurl_request *request, knurl_reply *reply,
                                   void *data, knurl_error *error)
{
    struct callback_fixture *f = data;
    struct http_request *native = request->impl->native;
    f->path = native->path;
    f->agent = native->agent;
    f->peer = request->impl->peer.address.data;
    return knurl_reply_text(reply, 200, "text/plain", "ok", error);
}

static int callback_run(void)
{
    struct callback_fixture f;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config route;
    knurl_server *server;
    knurl_error error;
    int status, i, seen = 0;
    char note;
    memset(&f, 0, sizeof(f));
    f.parent = getpid();
    f.owner = pthread_self();
    CHECK(pipe(f.notes) == 0);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    config.listeners = &listener;
    config.listener_count = 1;
    config.workers.count = 2;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.callbacks.on_worker_start = callback_worker;
    config.callbacks.on_parent_ready = callback_client;
    config.callbacks.userdata = &f;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    CHECK(server->set_log_callback(server, collect, &f, &error) == KNURL_OK);
    knurl_route_config_init(&route);
    route.handler = callback_route;
    route.userdata = &f;
    route.path = "/gate";
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    CHECK(knurl_server_run(server, NULL, &error) == KNURL_OK);
    CHECK(waitpid(f.client, &status, 0) == f.client && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    for (i = 0; i < 3; ++i) {
        CHECK(read_note(f.notes[0], &note));
        CHECK(note == '1' || note == '2' || note == 'a');
        seen |= note == 'a' ? 4 : 1 << (note - '1');
    }
    CHECK(f.runtime_seen == 1 && seen == 7);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    close(f.notes[0]);
    close(f.notes[1]);
    return 0;
}

int main(void)
{
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_server *server;
    knurl_error error;
    knurl_run_result result;
    pslog_config logging;
    pslog_logger *root;
    knurl_status status;
    char directory[] = "knurl-log-backend-XXXXXX";
    char path[96], access_path[96], *line = NULL;
    size_t capacity = 0, xs;
    FILE *file;
    int full_record = 0;
    CHECK(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/root.log", directory);
    snprintf(access_path, sizeof(access_path), "%s/access.log", directory);
    pslog_default_config(&logging);
    logging.mode = PSLOG_MODE_JSON;
    logging.timestamps = 0;
    logging.line_buffer_capacity = 128;
    CHECK(pslog_output_init_file(&logging.output, path, "a") == 0);
    root = pslog_new(&logging);
    CHECK(root != NULL);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.http.access_log_path = access_path;
    config.callbacks.on_worker_start = started;
    config.callbacks.on_parent_ready = ready;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    CHECK(knurl_server_set_pslog(server, root, NULL, &error) == KNURL_OK);
    status = knurl_server_run(server, &result, &error);
    if (status != KNURL_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(status == KNURL_OK && !result.escalated);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    root->destroy(root);
    file = fopen(path, "r");
    CHECK(file != NULL);
    while (getline(&line, &capacity, file) >= 0) {
        char *message = strstr(line, "\"msg\":\"xxx");
        if (message != NULL) {
            message += 7;
            for (xs = 0; message[xs] == 'x'; ++xs) {
            }
            CHECK(xs == 10000 && message[xs] == '"');
            ++full_record;
        }
    }
    free(line);
    fclose(file);
    CHECK(full_record == 1);
    CHECK(unlink(path) == 0 && unlink(access_path) == 0 &&
          rmdir(directory) == 0);
    return callback_run();
}
