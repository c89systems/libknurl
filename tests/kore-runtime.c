#include "library.h"
#include "http.h"

#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static jmp_buf failure;
static pid_t parent_pid;
static int cleaning;
static int saved_fd;
static char diagnostic[384];
static struct kore_native_callbacks callbacks = {0};

static void backend_fatal(void *userdata, const char *message)
{
    (void)userdata;
    if (getpid() != parent_pid || cleaning) {
        (void)write(STDERR_FILENO, message, strlen(message));
        _exit(72);
    }
    (void)snprintf(diagnostic, sizeof(diagnostic), "%s", message);
    longjmp(failure, 1);
}

static int serve(struct http_request *request)
{
    http_response_header(request, "content-type", "text/plain");
    http_response(request, 200, "alive\n", 6);
    kore_shutdown();
    return KORE_RESULT_OK;
}

static int client(unsigned short port)
{
    static const char request[] =
        "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    struct sockaddr_in address;
    char response[2048];
    size_t used;
    ssize_t count;
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
        return 2;
    if (write(fd, request, sizeof(request) - 1) !=
        (ssize_t)(sizeof(request) - 1))
        return 3;
    used = 0;
    while (used < sizeof(response) - 1) {
        count = read(fd, response + used, sizeof(response) - 1 - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return 4;
        if (count == 0)
            break;
        used += (size_t)count;
    }
    (void)close(fd);
    response[used] = '\0';
    if (strstr(response, "HTTP/1.1 200") == NULL ||
        strstr(response, "\r\n\r\nalive\n") == NULL)
        return 5;
    return 0;
}

static struct kore_server *prepare(void)
{
    struct kore_library_hooks hooks = {0};
    struct kore_server *server;
    hooks.fatal = backend_fatal;
    kore_library_init(&hooks);
    kore_quiet = 1;
    worker_count = 1;
    worker_set_affinity = 0;
    worker_shutdown_timeout_ms = 2000;
    server = kore_server_create("runtime-fixture");
    server->tls = 0;
    if (!kore_server_bind(server, "127.0.0.1", "0", NULL))
        fatal("fixture bind failed");
    return server;
}

static int run_once(void)
{
    struct kore_server *server;
    struct kore_domain *domain;
    struct kore_route *route;
    struct listener *listener;
    struct sockaddr_in address;
    socklen_t length;
    pid_t child;
    u_int64_t deadline;
    int status;
    server = prepare();
    domain = kore_domain_new("*");
    if (!kore_domain_attach(domain, server))
        return 1;
    route = kore_route_create(domain, "/health", HANDLER_TYPE_STATIC);
    if (route == NULL)
        return 2;
    callbacks.http_request = serve;
    route->rcall = kore_runtime_native(&callbacks);
    route->func = kore_strdup("typed fixture");
    listener = LIST_FIRST(&server->listeners);
    length = sizeof(address);
    if (getsockname(listener->fd, (struct sockaddr *)&address, &length) != 0)
        return 3;
    if (!kore_library_parent_start())
        return 4;
    child = fork();
    if (child == -1)
        return 5;
    if (child == 0)
        _exit(client(ntohs(address.sin_port)));
    deadline = kore_time_ms() + 5000;
    while (kore_quit == KORE_QUIT_NONE && kore_time_ms() < deadline)
        kore_library_parent_step(50);
    cleaning = 1;
    kore_library_cleanup();
    cleaning = 0;
    /* The client is an unrelated child in the same process group. Kore must
     * leave it for this caller's waitpid, including after the client exits. */
    if (waitpid(child, &status, 0) != child)
        return 6;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 7;
    return kore_quit == KORE_QUIT_NORMAL ? 0 : 8;
}

int main(void)
{
    struct kore_server *server;
    int result;
    parent_pid = getpid();
    cleaning = 0;
    if (setjmp(failure) == 0) {
        server = prepare();
        saved_fd = LIST_FIRST(&server->listeners)->fd;
        fatal("expected fixture error after bind");
    }
    if (strcmp(diagnostic, "expected fixture error after bind") != 0)
        return 10;
    cleaning = 1;
    kore_library_cleanup();
    cleaning = 0;
    if (fcntl(saved_fd, F_GETFD) != -1)
        return 11;
    if (setjmp(failure) != 0) {
        (void)fprintf(stderr, "backend failure: %s\n", diagnostic);
        cleaning = 1;
        kore_library_cleanup();
        return 12;
    }
    result = run_once();
    if (result == 0)
        result = run_once();
    if (result != 0) {
        cleaning = 1;
        kore_library_cleanup();
        (void)fprintf(stderr, "runtime fixture failed: %d\n", result);
    }
    return result;
}
