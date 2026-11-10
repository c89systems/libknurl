#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pid_t child = -1;
static int handled;

static int client(unsigned short port)
{
    static const char message[] =
        "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    struct sockaddr_in address;
    struct timeval timeout;
    char response[2048];
    size_t used = 0;
    ssize_t count;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
        return 2;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
        write(fd, message, sizeof(message) - 1) !=
            (ssize_t)(sizeof(message) - 1))
        return 2;
    while ((count = read(fd, response + used, sizeof(response) - 1 - used)) >
           0) {
        used += (size_t)count;
        if (used == sizeof(response) - 1)
            break;
    }
    close(fd);
    response[used] = 0;
    return count < 0 || strstr(response, "HTTP/1.1 200 ") == NULL ||
           strstr(response, "server: knurl-smoke\r\n") == NULL ||
           strstr(response, "\r\n\r\nok") == NULL;
}

static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *data,
                          knurl_error *error)
{
    knurl_bound_listener listener;
    int status;
    (void)process;
    (void)data;
    if (knurl_server_listener_at(server, 0, &listener, error) != KNURL_OK)
        return KNURL_STATE;
    if (knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return KNURL_STATE;
    child = fork();
    if (child < 0)
        return KNURL_IO;
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        status = client(listener.port);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    return KNURL_OK;
}

static knurl_status handler(knurl_request *request, knurl_reply *reply,
                            void *data, knurl_error *error)
{
    (void)data;
    ++handled;
    if (knurl_server_request_stop(knurl_request_server(request), error) !=
        KNURL_OK)
        return KNURL_STATE;
    return knurl_reply_text(reply, 200, "text/plain", "ok", error);
}

static int exited(knurl_server *server, const knurl_process_context *process,
                  void *data)
{
    (void)server;
    (void)process;
    (void)data;
    return handled != 1;
}

int main(void)
{
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config route;
    knurl_server *server;
    knurl_error error;
    knurl_run_result result;
    knurl_u64 a, b, sum;
    knurl_status status;
    int child_status;
    a.high = 0;
    a.low = 0xffffffffUL;
    b.high = 0;
    b.low = 1;
    if (knurl_u64_add(a, b, &sum, NULL) != KNURL_OK || sum.high != 1 ||
        sum.low != 0 ||
        strcmp(knurl_static_content_type("readme.MD"),
               "text/markdown; charset=utf-8"))
        return 1;
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.http.server_header = "knurl-smoke";
    /* Rotation declarations retain large-file offsets on 32-bit targets. */
    config.http.access_log_max_bytes.high = 1;
    config.http.access_log_max_bytes.low = 0;
    config.limits.hard_body_bytes.low = 1024;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_exit = exited;
    status = knurl_server_new(&config, &server, &error);
    if (status != KNURL_OK)
        return 2;
    knurl_route_config_init(&route);
    route.path = "/";
    route.handler = handler;
    status =
        knurl_router_route(knurl_server_router(server), &route, NULL, &error);
    if (status == KNURL_OK)
        status = knurl_server_run(server, &result, &error);
    if (status != KNURL_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    if (child > 0 && status != KNURL_OK)
        kill(child, SIGTERM);
    if (child < 0 || waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
        status = KNURL_STATE;
    knurl_server_destroy(server, NULL);
    return status != KNURL_OK;
}
