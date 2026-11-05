#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "vectis-host.h"
#include "vectis-server.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            fprintf(stderr, "host adapter %d: %s\n", __LINE__, #test);         \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct application {
    struct reference_app app;
    struct reference_kore_host host;
    long original;
    int *ready, notes, exit_failure, abandoned;
};
static reference_status reply(struct reference_app *app,
                              struct reference_request *request,
                              struct reference_response *response, void *data,
                              reference_error *error)
{
    (void)app;
    (void)request;
    (void)data;
    return reference_response_text(response, 200, "text/plain", "ok", error);
}
/* Fixture providers stand in for Vectis's existing READY/STOP codec. The
 * adapter only forwards FD/host callbacks; that protocol stays host-owned. */
static void notify(struct reference_app *app, int fd)
{
    rotmos_bound_listener listener;
    unsigned char bytes[3];
    if (rotmos_server_get_state(app->server) != ROTMOS_SERVER_READY_PAUSED ||
        rotmos_server_listener_at(app->server, 0, &listener, NULL) != ROTMOS_OK)
        _exit(81);
    bytes[0] = 'R';
    bytes[1] = (unsigned char)(listener.port >> 8);
    bytes[2] = (unsigned char)listener.port;
    if (write(fd, bytes, sizeof(bytes)) != sizeof(bytes))
        _exit(82);
}
static reference_status control(struct reference_app *app, int fd, int *stop,
                                reference_error *error)
{
    char byte;
    (void)app;
    reference_error_clear(error);
    if (read(fd, &byte, 1) != 1 || byte != 'S')
        return REFERENCE_INVALID;
    *stop = 1;
    return REFERENCE_OK;
}
static reference_status fork_exit(struct reference_app *app, const char *role,
                                  void *data, reference_error *error)
{
    struct application *application = (struct application *)app;
    int failed = application->exit_failure, notes = application->notes;
    char byte = !strcmp(role, "kore_worker") ? 'W' : 'P';
    (void)data;
    if (getpid() == application->original ||
        (byte == 'W' &&
         rotmos_server_get_state(app->server) != ROTMOS_SERVER_STOPPED) ||
        rotmos_server_loop(app->server) != NULL)
        return REFERENCE_STATE;
    if (rotmos_server_destroy(app->server, NULL) != ROTMOS_OK)
        return REFERENCE_STATE;
    reference_route_cleanup(app);
    (void)munmap(application->ready, sizeof(*application->ready));
    free(application);
    if (write(notes, &byte, 1) != 1)
        return REFERENCE_STATE;
    if (byte == 'W' && failed == 2)
        (void)poll(NULL, 0, 5000);
    reference_error_clear(error);
    /* Failure after reclaiming host/server must still reach the parent. */
    return byte == 'W' && failed ? REFERENCE_STATE : REFERENCE_OK;
}
static void observed(rotmos_server *server, const rotmos_worker_event *event,
                     void *data)
{
    struct reference_kore_host *host = data;
    struct application *application = (struct application *)host->app;
    (void)server;
    if (event->kind == ROTMOS_WORKER_EXITED)
        application->abandoned = event->abandoned;
}
static int request(unsigned short port)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char bytes[512];
    const char query[] =
        "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ssize_t count;
    int fd = socket(AF_INET, SOCK_STREAM, 0), status = 0;
    if (fd < 0)
        return 0;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0 &&
        write(fd, query, sizeof(query) - 1) == sizeof(query) - 1) {
        count = read(fd, bytes, sizeof(bytes) - 1);
        if (count > 0) {
            bytes[count] = 0;
            (void)sscanf(bytes, "HTTP/1.1 %d", &status);
        }
    }
    close(fd);
    return status;
}
static int run(int scenario)
{
    struct application *application;
    struct reference_server_config source;
    struct reference_route_config route;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_error native;
    reference_error error;
    struct pollfd poller;
    unsigned char ready[3];
    char exited[2];
    int pair[2], notes[2], status, tries, served = 0;
    unsigned short port;
    pid_t child;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0 && pipe(notes) == 0);
    fprintf(stderr, "host scenario %d starting\n", scenario);
    application = calloc(1, sizeof(*application));
    CHECK(application != NULL);
    application->ready =
        mmap(NULL, sizeof(*application->ready), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(application->ready != MAP_FAILED);
    application->original = (long)getpid();
    application->notes = notes[1];
    application->exit_failure = scenario == 4 ? 2 : scenario == 1;
    application->host.app = &application->app;
    application->host.control_fd = pair[1];
    application->host.app_ready = application->ready;
    application->host.notify_ready = notify;
    application->host.control = control;
    application->host.fork_exit = fork_exit;
    reference_server_config_init(&source);
    source.worker_count = 1;
    source.worker_set_affinity_disabled = source.kore_quiet = 1;
    source.access_log.logger_disabled = 1;
    CHECK(reference_server_translate(&source, &config, &native) == ROTMOS_OK);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    if (scenario == 4)
        config.workers.shutdown_timeout_ms = 40;
    reference_host_configure(&application->host, &config);
    config.callbacks.on_worker_event = observed;
    CHECK(rotmos_server_new(&config, &application->app.server, &native) ==
          ROTMOS_OK);
    memset(&route, 0, sizeof(route));
    route.path = "/";
    route.method = ROTMOS_HTTP_GET;
    route.handler = reply;
    CHECK(reference_route_register(&application->app, &route, &error) ==
          REFERENCE_OK);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        reference_status result;
        close(pair[0]);
        close(notes[0]);
        result = reference_host_run(&application->host, &error);
        status =
            result == (scenario == 1 ? REFERENCE_STATE : REFERENCE_OK) ? 0 : 1;
        if (status != 0)
            fprintf(stderr, "host scenario %d run failed: %s (%s)\n", scenario,
                    error.message, error.detail);
        if (application->abandoned != (scenario == 4))
            status = 1;
        if (fork_exit(&application->app, "kore_parent", NULL, &error) !=
            REFERENCE_OK)
            status = 1;
        _exit(status);
    }
    poller.fd = pair[0];
    poller.events = POLLIN;
    poller.revents = 0;
    CHECK(poll(&poller, 1, 3000) == 1 &&
          read(pair[0], ready, sizeof(ready)) == 3 && ready[0] == 'R');
    port = (unsigned short)(((unsigned int)ready[1] << 8) | ready[2]);
    CHECK(request(port) == 503);
    __sync_lock_test_and_set(application->ready, 1);
    for (tries = 0; tries < 100 && !served; ++tries) {
        served = request(port) == 200;
        if (!served)
            (void)poll(NULL, 0, 10);
    }
    CHECK(served);
    if (scenario == 2)
        CHECK(write(pair[0], "?", 1) == 1);
    else if (scenario == 3)
        CHECK(shutdown(pair[0], SHUT_WR) == 0);
    else
        CHECK(write(pair[0], "S", 1) == 1);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(read(notes[0], exited, 2) == 2 && !memcmp(exited, "WP", 2));
    CHECK(*application->ready == 1);
    CHECK(rotmos_server_destroy(application->app.server, &native) == ROTMOS_OK);
    reference_route_cleanup(&application->app);
    CHECK(munmap(application->ready, sizeof(*application->ready)) == 0);
    free(application);
    close(notes[0]);
    close(notes[1]);
    close(pair[0]);
    return 0;
}
int main(void) { return run(0) || run(1) || run(2) || run(3) || run(4); }
