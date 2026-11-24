#define _POSIX_C_SOURCE 200809L
#include "../acl-wire.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t runner;

static void failure(const char *expression, int line)
{
    fprintf(stderr, "knurl runner test:%d: %s\n", line, expression);
    if (runner > 0) {
        (void)kill(runner, SIGTERM);
        (void)waitpid(runner, NULL, 0);
    }
    exit(1);
}

#define CHECK(expression)                                                      \
    ((expression) ? (void)0 : failure(#expression, __LINE__))

static void pause_briefly(void)
{
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
}

static void path(char *out, size_t capacity, const char *root, const char *file)
{
    int length = snprintf(out, capacity, "%s/%s", root, file);
    CHECK(length > 0 && (size_t)length < capacity);
}

static int events(const char *file, const char *message)
{
    char line[128];
    FILE *stream = fopen(file, "r");
    int count = 0;
    if (stream == NULL)
        return 0;
    while (fgets(line, sizeof(line), stream) != NULL)
        if (strcmp(line, message) == 0)
            ++count;
    fclose(stream);
    return count;
}

static void milestone(const char *file, const char *message, int count)
{
    int i;
    for (i = 0; i < 400; ++i) {
        if (events(file, message) == count)
            return;
        pause_briefly();
    }
    fprintf(stderr, "missing lifecycle event %s", message);
    CHECK(0);
}

static unsigned short available_port(void)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(fd, (struct sockaddr *)&address, length) == 0);
    CHECK(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
    close(fd);
    return ntohs(address.sin_port);
}

static void wait_ok(pid_t child)
{
    int status;
    pid_t finished = waitpid(child, &status, 0);
    if (finished == runner)
        runner = 0;
    CHECK(finished == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void available(unsigned short port)
{
    int i;
    for (i = 0; i < 400; ++i) {
        if (knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
            200)
            return;
        pause_briefly();
    }
    CHECK(0);
}

static void upload(unsigned short port, const char *log)
{
    static const char request[] =
        "POST /app/alice/upload HTTP/1.1\r\nHost: localhost\r\n"
        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n1\r\na\r\n";
    static const char remaining[] = "2\r\nbc\r\n0\r\n\r\n";
    struct sockaddr_in address;
    struct timeval timeout = {3, 0};
    char response[1024], *body;
    size_t used = 0;
    int fd;
    ssize_t n;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
          0);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(write(fd, request, sizeof(request) - 1) ==
          (ssize_t)sizeof(request) - 1);
    milestone(log, "upload prefix\n", 1);
    CHECK(write(fd, remaining, sizeof(remaining) - 1) ==
          (ssize_t)sizeof(remaining) - 1);
    while (used < sizeof(response) - 1 &&
           (n = read(fd, response + used, sizeof(response) - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    response[used] = '\0';
    body = strstr(response, "\r\n\r\n");
    CHECK(strncmp(response, "HTTP/1.1 200", 12) == 0);
    CHECK(body != NULL && strcmp(body + 4, "abc") == 0);
}

static void exercise(unsigned short port, const char *log, const char *gate)
{
    int fd, i, status;
    pid_t pending;
    pending = fork();
    CHECK(pending >= 0);
    if (pending == 0)
        _exit(knurl_test_acl_exchange(port, "GET", "/app/ali%63e/hold", NULL,
                                      "alice:41") == 200
                  ? 0
                  : 1);
    milestone(log, "handler waiting\n", 1);
    CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
          200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/asset", NULL, "asset\n") ==
          200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/count", NULL, NULL) ==
          503);
    CHECK(waitpid(pending, &status, WNOHANG) == 0);
    for (i = 0; i < 400; ++i) {
        fd = open(gate, O_WRONLY | O_NONBLOCK);
        if (fd >= 0)
            break;
        CHECK(errno == ENXIO);
        pause_briefly();
    }
    CHECK(i < 400 && write(fd, "x", 1) == 1);
    close(fd);
    wait_ok(pending);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/count", NULL,
                                  "bob:42") == 200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/json", NULL,
                                  "[\"bob\",{\"count\":42},true]") == 200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/cycle", NULL, NULL) ==
          500);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/error", NULL, NULL) ==
          500);
    upload(port, log);

    /* Abandon a worker. Its replacement starts from the same parent snapshot.
     */
    (void)knurl_test_acl_exchange(port, "GET", "/app/bob/crash", NULL, NULL);
    milestone(log, "worker start\n", 2);
    available(port);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/count", NULL,
                                  "bob:41") == 200);
}

int main(int argc, char **argv)
{
    char log[4096], gate[4096], asset[4096], port[16];
    unsigned short selected;
    pid_t child;
    FILE *file;
    int normal, init_failure;
    CHECK(argc == 5);
    normal = strcmp(argv[4], "normal") == 0;
    init_failure = strcmp(argv[4], "init-failure") == 0;
    path(log, sizeof(log), argv[3], "events");
    path(gate, sizeof(gate), argv[3], "gate");
    path(asset, sizeof(asset), argv[3], "asset");
    (void)unlink(log);
    (void)unlink(gate);
    CHECK(mkfifo(gate, 0600) == 0);
    file = fopen(asset, "w");
    CHECK(file != NULL && fputs("asset\n", file) >= 0 && fclose(file) == 0);
    selected = available_port();
    snprintf(port, sizeof(port), "%u", (unsigned int)selected);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        execl(argv[1], argv[1], argv[2], argv[3], port, argv[4], (char *)NULL);
        _exit(126);
    }
    runner = child;
    if (!init_failure) {
        milestone(log, "worker start\n", 1);
        available(selected);
        if (normal)
            exercise(selected, log, gate);
        CHECK(kill(child, SIGTERM) == 0);
    }
    wait_ok(child);
    runner = 0;
    CHECK(events(log, "parent start\n") == 1 &&
          events(log, "parent end\n") == 1);
    CHECK(events(log, "worker start\n") == (normal ? 2 : init_failure ? 0 : 1));
    CHECK(events(log, "worker stop\n") == !init_failure);
    if (init_failure)
        CHECK(events(log, "init failure reported\n") == 1);
    else if (!normal)
        CHECK(events(log, "stop failure reported\n") == 1);
    CHECK(unlink(gate) == 0);
    return 0;
}
