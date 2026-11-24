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
    unsigned char invalid[128];
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
    memset(invalid, 0, sizeof(invalid));
    kore_msg_send(KORE_MSG_PARENT, KORE_MSG_WORKER_LOG, invalid, 1);
    kore_msg_send(KORE_MSG_PARENT, KORE_MSG_WORKER_LOG, invalid,
                  sizeof(invalid));
    memset(invalid, 255, sizeof(invalid));
    kore_msg_send(KORE_MSG_PARENT, KORE_MSG_WORKER_LOG, invalid,
                  sizeof(invalid));
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
    int entered[2], release[2];
    int runtime_seen, workers_seen, access_seen, bad_record;
};

static int wait_byte(int fd)
{
    struct pollfd note;
    char byte;
    note.fd = fd;
    note.events = POLLIN;
    return poll(&note, 1, 2500) == 1 && read(fd, &byte, 1) == 1;
}

static void collect(const knurl_log_record *record, void *data)
{
    struct callback_fixture *f = data;
    const knurl_log_access *access = record->access;
    if (getpid() != f->parent || !pthread_equal(pthread_self(), f->owner))
        _exit(1);
    if (strcmp(record->message, "callback parent") == 0)
        ++f->runtime_seen;
    if (strcmp(record->message, "callback worker") == 0 &&
        strncmp(record->process, "[wrk ", 5) == 0)
        ++f->workers_seen;
    if (access != NULL && strcmp(access->path, "/gate") == 0) {
        ++f->access_seen;
        if (record->level != KNURL_LOG_TRACE || access->status != 200 ||
            access->bytes.high != 0 || access->bytes.low != 2 ||
            strcmp(access->method, "GET") != 0 ||
            strcmp(access->client_ip, "127.0.0.1") != 0 ||
            strcmp(access->user_agent, "callback tester") != 0 ||
            write(f->entered[1], "x", 1) != 1 || !wait_byte(f->release[0]))
            f->bad_record = 1;
    }
}

static knurl_status callback_worker(knurl_server *server,
                                    const knurl_process_context *process,
                                    void *data, knurl_error *error)
{
    int capacity = 4096;
    (void)server;
    (void)process;
    (void)data;
    if (setsockopt(worker->msg[1]->fd, SOL_SOCKET, SO_SNDBUF, &capacity,
                   sizeof(capacity)) != 0)
        return knurl_fail(error, KNURL_IO, "callback_worker",
                          "setting the fixture message buffer capacity");
    kore_log(LOG_INFO, "callback worker");
    return KNURL_OK;
}

static knurl_status callback_client(knurl_server *server,
                                    const knurl_process_context *process,
                                    void *data, knurl_error *error)
{
    struct callback_fixture *f = data;
    knurl_bound_listener listener;
    int ok, i;
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
                                     "ok") == 200 &&
             wait_byte(f->entered[0]);
        /* Access logging continues while the parent sink is blocked. */
        for (i = 0; ok && i < 32; ++i)
            ok = knurl_test_acl_exchange(listener.port, "GET", "/health", NULL,
                                         "ok") == 200;
        if (write(f->release[1], "x", 1) != 1)
            ok = 0;
        (void)kill(f->parent, SIGTERM);
        _exit(!ok);
    }
    return KNURL_OK;
}

static knurl_status callback_route(knurl_request *request, knurl_reply *reply,
                                   void *data, knurl_error *error)
{
    (void)request;
    (void)data;
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
    int status;
    memset(&f, 0, sizeof(f));
    f.parent = getpid();
    f.owner = pthread_self();
    CHECK(pipe(f.entered) == 0 && pipe(f.release) == 0);
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
    route.path = "/gate";
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    route.path = "/health";
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    CHECK(knurl_server_run(server, NULL, &error) == KNURL_OK);
    CHECK(waitpid(f.client, &status, 0) == f.client && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(f.runtime_seen == 1 && f.workers_seen == 2 && f.access_seen == 1 &&
          !f.bad_record);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    close(f.entered[0]);
    close(f.entered[1]);
    close(f.release[0]);
    close(f.release[1]);
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
    int bounded = 0, short_record = 0, invalid_length = 0, oversize = 0;
    CHECK(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/root.log", directory);
    snprintf(access_path, sizeof(access_path), "%s/access.log", directory);
    pslog_default_config(&logging);
    logging.mode = PSLOG_MODE_JSON;
    logging.timestamps = 0;
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
            CHECK(xs == 8192 && message[xs] == '"');
            ++bounded;
        }
        if (strstr(line, "too short worker log received"))
            ++short_record;
        if (strstr(line, "invalid worker log length"))
            ++invalid_length;
        if (strstr(line, "oversize worker log received"))
            ++oversize;
    }
    free(line);
    fclose(file);
    CHECK(bounded == 1 && short_record == 1 && invalid_length == 1 &&
          oversize == 1);
    CHECK(unlink(path) == 0 && unlink(access_path) == 0 &&
          rmdir(directory) == 0);
    return callback_run();
}
