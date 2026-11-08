#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
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
           strstr(response, "server: rotmos-smoke\r\n") == NULL ||
           strstr(response, "\r\n\r\nok") == NULL;
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process, void *data,
                           rotmos_error *error)
{
    rotmos_bound_listener listener;
    int status;
    (void)process;
    (void)data;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_STATE;
    child = fork();
    if (child < 0)
        return ROTMOS_IO;
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        status = client(listener.port);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    return ROTMOS_OK;
}

static rotmos_status handler(rotmos_request *request, rotmos_reply *reply,
                             void *data, rotmos_error *error)
{
    (void)data;
    ++handled;
    if (rotmos_server_request_stop(rotmos_request_server(request), error) !=
        ROTMOS_OK)
        return ROTMOS_STATE;
    return rotmos_reply_text(reply, 200, "text/plain", "ok", error);
}

static int exited(rotmos_server *server, const rotmos_process_context *process,
                  void *data)
{
    (void)server;
    (void)process;
    (void)data;
    return handled != 1;
}

int main(void)
{
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_route_config route;
    rotmos_server *server;
    rotmos_error error;
    rotmos_run_result result;
    rotmos_u64 a, b, sum;
    rotmos_status status;
    int child_status;
    a.high = 0;
    a.low = 0xffffffffUL;
    b.high = 0;
    b.low = 1;
    if (rotmos_u64_add(a, b, &sum, NULL) != ROTMOS_OK || sum.high != 1 ||
        sum.low != 0 ||
        strcmp(rotmos_static_content_type("readme.MD"),
               "text/markdown; charset=utf-8"))
        return 1;
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.http.server_header = "rotmos-smoke";
    /* Rotation declarations retain large-file offsets on 32-bit targets. */
    config.http.access_log_max_bytes.high = 1;
    config.http.access_log_max_bytes.low = 0;
    config.limits.hard_body_bytes.low = 1024;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_exit = exited;
    status = rotmos_server_new(&config, &server, &error);
    if (status != ROTMOS_OK)
        return 2;
    rotmos_route_config_init(&route);
    route.path = "/";
    route.handler = handler;
    status =
        rotmos_router_route(rotmos_server_router(server), &route, NULL, &error);
    if (status == ROTMOS_OK)
        status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    if (child > 0 && status != ROTMOS_OK)
        kill(child, SIGTERM);
    if (child < 0 || waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
        status = ROTMOS_STATE;
    rotmos_server_destroy(server, NULL);
    return status != ROTMOS_OK;
}
