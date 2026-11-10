#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #test);                 \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static knurl_status ready(knurl_server *server, const knurl_process_context *p,
                          void *data, knurl_error *error)
{
    knurl_bound_listener listener;
    knurl_status status;
    (void)p;
    ++*(int *)data;
    status = knurl_server_listener_at(server, 0, &listener, error);
    if (status != KNURL_OK)
        return status;
    if (listener.port == 0)
        return KNURL_INVALID;
    return knurl_server_request_stop(server, error);
}

static void setup(knurl_server_config *config, knurl_listener_config *listener,
                  int *calls)
{
    knurl_server_config_init(config);
    knurl_listener_config_init(listener);
    listener->bind = "127.0.0.1";
    config->listeners = listener;
    config->listener_count = config->workers.count = 1;
    config->workers.affinity_disabled = config->http.backend_quiet = 1;
    config->callbacks.on_parent_ready = ready;
    config->callbacks.userdata = calls;
}

static int listener_probe(void)
{
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_server *server;
    knurl_run_result result;
    knurl_error error;
    int calls = 0, fd;
    char port[16];

    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(listen(fd, 1) == 0);
    CHECK(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    setup(&config, &listener, &calls);
    listener.port = ntohs(address.sin_port);
    (void)snprintf(port, sizeof(port), "%u", (unsigned int)listener.port);
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    CHECK(knurl_server_validate(server, &error) == KNURL_OK);
    CHECK(knurl_server_preflight(server, &error) == KNURL_BACKEND);
    CHECK(error.source == KNURL_ERROR_SYSTEM &&
          error.native_code == EADDRINUSE);
    CHECK(strcmp(error.operation, "listener_bind") == 0 &&
          strstr(error.detail, port) != NULL &&
          strstr(error.detail, listener.bind) != NULL);
    CHECK(calls == 0 &&
          knurl_server_get_state(server) == KNURL_SERVER_DECLARED);
    CHECK(close(fd) == 0);
    CHECK(knurl_server_preflight(server, &error) == KNURL_OK);
    CHECK(error.status == KNURL_OK && calls == 0);

    /* Occupancy can change after the probe. Run still reports its own bind. */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0 &&
          bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(listen(fd, 1) == 0);
    CHECK(knurl_server_run(server, &result, &error) == KNURL_BACKEND);
    CHECK(result.reason == KNURL_RUN_STARTUP_FAILURE && calls == 0);
    CHECK(error.native_code == EADDRINUSE &&
          strcmp(error.operation, "listener_bind") == 0);
    CHECK(close(fd) == 0);
    CHECK(knurl_server_preflight(server, &error) == KNURL_OK);
    CHECK(knurl_server_run(server, &result, &error) == KNURL_OK && calls == 1);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    return 0;
}

static knurl_status reply(knurl_request *request, knurl_reply *out, void *data,
                          knurl_error *error)
{
    (void)request;
    (void)data;
    return knurl_reply_text(out, 200, "text/plain", "ok", error);
}

static int limited_spool(knurl_server *server, const char *directory)
{
    struct rlimit limit;
    struct sigaction action;
    knurl_error error;
    pid_t child;
    int status;

    child = fork();
    CHECK(child != -1);
    if (child == 0) {
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        if (sigemptyset(&action.sa_mask) != 0 ||
            sigaction(SIGXFSZ, &action, NULL) != 0 ||
            sigaddset(&action.sa_mask, SIGXFSZ) != 0 ||
            sigprocmask(SIG_UNBLOCK, &action.sa_mask, NULL) != 0 ||
            getrlimit(RLIMIT_FSIZE, &limit) != 0)
            _exit(1);
        limit.rlim_cur = 0;
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0)
            _exit(1);
        if (knurl_server_preflight(server, &error) != KNURL_IO ||
            error.source != KNURL_ERROR_SYSTEM || error.native_code != EFBIG ||
            strcmp(error.operation, "body_spool") != 0 ||
            strcmp(error.detail, directory) != 0)
            _exit(2);
        if (sigaction(SIGXFSZ, NULL, &action) != 0 ||
            action.sa_handler != SIG_DFL ||
            sigprocmask(SIG_SETMASK, NULL, &action.sa_mask) != 0 ||
            sigismember(&action.sa_mask, SIGXFSZ) != 0)
            _exit(3);
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

static int file_probes(void)
{
    char directory[] = "preflight-XXXXXX";
    char log[64], spool[64];
    FILE *file;
    struct stat info;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config route;
    knurl_server *server;
    knurl_error error;
    int calls = 0;

    CHECK(mkdtemp(directory) != NULL);
    (void)snprintf(log, sizeof(log), "%s/access.log", directory);
    (void)snprintf(spool, sizeof(spool), "%s/spool", directory);
    CHECK(mkdir(log, 0700) == 0);
    file = fopen(spool, "w");
    CHECK(file != NULL && fclose(file) == 0);
    setup(&config, &listener, &calls);
    config.http.access_log_path = log;
    config.limits.body_spool_directory = spool;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    CHECK(knurl_server_validate(server, &error) == KNURL_OK);
    CHECK(knurl_server_preflight(server, &error) == KNURL_IO);
    CHECK(strcmp(error.operation, "access_log_open") == 0 &&
          error.source == KNURL_ERROR_SYSTEM && error.native_code != 0 &&
          strcmp(error.detail, log) == 0);
    CHECK(rmdir(log) == 0 && symlink("spool", log) == 0);
    CHECK(knurl_server_preflight(server, &error) == KNURL_IO &&
          error.native_code == ELOOP);
    CHECK(unlink(log) == 0);

    knurl_route_config_init(&route);
    route.method = KNURL_HTTP_POST;
    route.path = "/memory";
    route.handler = reply;
    route.body.mode = KNURL_BODY_BUFFERED;
    route.body.max_bytes.low = 16;
    route.body.memory_buffer_limit_bytes = 16;
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    route.path = "/disabled";
    route.body.max_bytes.low = 32;
    route.body.disk_spool_disabled = 1;
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    CHECK(knurl_server_preflight(server, &error) == KNURL_OK);
    CHECK(stat(log, &info) == 0 && S_ISREG(info.st_mode) && info.st_size == 0);

    route.path = "/get-only";
    route.method = KNURL_HTTP_GET;
    route.body.disk_spool_disabled = 0;
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    CHECK(knurl_server_preflight(server, &error) == KNURL_OK);

    route.path = "/spill";
    route.methods = KNURL_HTTP_METHODS_GET | KNURL_HTTP_METHODS_POST;
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    CHECK(knurl_server_validate(server, &error) == KNURL_OK);
    CHECK(knurl_server_preflight(server, &error) == KNURL_IO);
    CHECK(strcmp(error.operation, "body_spool") == 0 &&
          error.source == KNURL_ERROR_SYSTEM && error.native_code == ENOTDIR &&
          strcmp(error.detail, spool) == 0 && calls == 0);
    CHECK(unlink(spool) == 0 && mkdir(spool, 0700) == 0);
    CHECK(limited_spool(server, spool) == 0);
    CHECK(knurl_server_preflight(server, &error) == KNURL_OK && calls == 0);
    CHECK(knurl_server_run(server, NULL, &error) == KNURL_OK && calls == 1);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    CHECK(rmdir(spool) == 0 && unlink(log) == 0 && rmdir(directory) == 0);
    return 0;
}

int main(void)
{
    CHECK(knurl_server_preflight(NULL, NULL) == KNURL_INVALID);
    CHECK(listener_probe() == 0);
    CHECK(file_probes() == 0);
    return 0;
}
