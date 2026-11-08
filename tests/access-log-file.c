#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "../src/access_log_file.h"
#include <rotmos/rotmos.h>
#include <rotmos/pslog.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Faults are restricted to access.log. HTTP I/O and diagnostics are unaffected;
 * the shipped Core has no test hooks. */
#undef write
#undef rename
#undef open
extern ssize_t write(int, const void *, size_t);
extern int rename(const char *, const char *);
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "access log %d: %s\n", __LINE__, #t);              \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static int write_mode, write_calls, rename_mode, last_file_fd = -1;
static int open_failures;
static int notice_fd = -1;
static char active_path[256];
int rotmos_test_open(const char *path, int flags, ...)
{
    va_list args;
    unsigned int mode = 0;
    if (flags & O_CREAT) {
        va_start(args, flags);
        mode = va_arg(args, unsigned int);
        va_end(args);
    }
    if (open_failures != 0 && strcmp(path, active_path) == 0) {
        --open_failures;
        errno = ENOSPC;
        return -1;
    }
    return openat(AT_FDCWD, path, flags, mode);
}
static int contains(const void *data, size_t length, const char *text)
{
    const char *p = data;
    size_t i, n = strlen(text);
    for (i = 0; i <= length && n <= length - i; ++i)
        if (memcmp(p + i, text, n) == 0)
            return 1;
    return 0;
}
ssize_t rotmos_test_write(int fd, const void *bytes, size_t length)
{
    ++write_calls;
    last_file_fd = fd;
    if (write_mode == 1 || (write_mode == 2 && write_calls > 1) ||
        (write_mode == 5 && contains(bytes, length, "/disk-full"))) {
        if (write_mode == 5 && notice_fd >= 0 && write(notice_fd, "e", 1) != 1)
            abort();
        errno = ENOSPC;
        return -1;
    }
    if (write_mode == 3 && write_calls == 1) {
        errno = EINTR;
        return -1;
    }
    if ((write_mode == 2 || write_mode == 4) && length > 2)
        length = 2;
    return write(fd, bytes, length);
}
int rotmos_test_rename(const char *from, const char *to)
{
    int result;
    if (rename_mode == 1 && strcmp(from, active_path) == 0) {
        errno = EACCES;
        return -1;
    }
    result = rename(from, to);
    if (result == 0 && rename_mode == 2 && strcmp(from, active_path) == 0) {
        if (mkdir(from, 0700) != 0)
            abort();
    }
    return result;
}
static void path_for(char *out, size_t capacity, unsigned int index)
{
    if (index == 0)
        snprintf(out, capacity, "%s", active_path);
    else
        snprintf(out, capacity, "%s.%u", active_path, index);
}
static int put(unsigned int index, const char *text)
{
    char path[288];
    FILE *file;
    path_for(path, sizeof(path), index);
    file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(text, 1, strlen(text), file) == strlen(text));
    CHECK(fclose(file) == 0);
    return 0;
}
static int is(unsigned int index, const char *text)
{
    char path[288], data[256];
    FILE *file;
    size_t n;
    path_for(path, sizeof(path), index);
    file = fopen(path, "rb");
    CHECK(file != NULL);
    n = fread(data, 1, sizeof(data), file);
    CHECK(n == strlen(text) && memcmp(data, text, n) == 0);
    CHECK(fclose(file) == 0);
    return 0;
}
static int clear(void)
{
    unsigned int i;
    char path[288];
    for (i = 0; i <= 3; ++i) {
        path_for(path, sizeof(path), i);
        CHECK(unlink(path) == 0 || errno == ENOENT);
    }
    return 0;
}
static int absent(unsigned int index)
{
    char path[288];
    struct stat st;
    path_for(path, sizeof(path), index);
    return stat(path, &st) == -1 && errno == ENOENT;
}
static int file_cases(void)
{
    rotmos_server_config config;
    struct rotmos_access_file *file;
    rotmos_error error;
    struct stat st;
    char path[288];
    rotmos_server_config_init(&config);
    CHECK(config.http.access_log_max_bytes.high == 0 &&
          config.http.access_log_max_bytes.low == 10485760UL &&
          config.http.access_log_file_count == 3 &&
          !config.http.access_log_rotation_disabled);
    config.http.access_log_path = active_path;
    config.http.access_log_max_bytes.low = 8;
    CHECK(put(0, "") == 0 && chmod(active_path, 0600) == 0);
    CHECK(rotmos_access_file_open(&config.http, &file, &error) == ROTMOS_OK);
#define RECORD(text)                                                           \
    CHECK(rotmos_access_file_write(file, text, sizeof(text) - 1, &error) ==    \
          ROTMOS_OK)
    RECORD("one\n");
    RECORD("two\n");
    CHECK(is(0, "one\ntwo\n") == 0 && absent(1));
    RECORD("tri\n");
    RECORD("for\n");
    RECORD("fiv\n");
    RECORD("six\n");
    RECORD("sev\n");
    CHECK(is(0, "sev\n") == 0 && is(1, "fiv\nsix\n") == 0 &&
          is(2, "tri\nfor\n") == 0 && absent(3));
    CHECK(stat(active_path, &st) == 0 && (st.st_mode & 0777) == 0600);
    RECORD("0123456789\n");
    CHECK(is(0, "0123456789\n") == 0);
    RECORD("end\n");
    CHECK(is(0, "end\n") == 0 && is(1, "0123456789\n") == 0);
    CHECK(rotmos_access_file_close(file) == 0);
    CHECK(rotmos_access_file_open(&config.http, &file, &error) == ROTMOS_OK);
    RECORD("new\n");
    CHECK(is(0, "end\nnew\n") == 0);
    CHECK(rotmos_access_file_close(file) == 0 && clear() == 0);
    config.http.access_log_file_count = 1;
    CHECK(rotmos_access_file_open(&config.http, &file, &error) == ROTMOS_OK);
    RECORD("keep\n");
    RECORD("next\n");
    CHECK(is(0, "next\n") == 0 && absent(1));
    CHECK(rotmos_access_file_close(file) == 0 && clear() == 0);
    config.http.access_log_rotation_disabled = 1;
    CHECK(rotmos_access_file_open(&config.http, &file, &error) == ROTMOS_OK);
    RECORD("0123456789\n");
    RECORD("0123456789\n");
    CHECK(is(0, "0123456789\n0123456789\n") == 0 && absent(1));
    CHECK(rotmos_access_file_close(file) == 0 && clear() == 0);
    config.http.access_log_rotation_disabled = 0;
    config.http.access_log_file_count = 3;
    config.http.access_log_max_bytes.low = 4;
    CHECK(put(0, "aaaa") == 0 && put(1, "old1") == 0);
    CHECK(rotmos_access_file_open(&config.http, &file, &error) == ROTMOS_OK);
    rename_mode = 1;
    CHECK(rotmos_access_file_write(file, "bbbb", 4, &error) == ROTMOS_IO);
    CHECK(is(0, "aaaa") == 0 && absent(1) && is(2, "old1") == 0);
    CHECK(rotmos_access_file_write(file, "bbbb", 4, &error) == ROTMOS_IO);
    CHECK(is(2, "old1") == 0);
    rename_mode = 0;
    RECORD("bbbb");
    CHECK(is(0, "bbbb") == 0 && is(1, "aaaa") == 0 && is(2, "old1") == 0);
    rename_mode = 2;
    CHECK(rotmos_access_file_write(file, "cccc", 4, &error) == ROTMOS_IO);
    CHECK(is(1, "bbbb") == 0 && is(2, "aaaa") == 0);
    CHECK(rmdir(active_path) == 0);
    rename_mode = 0;
    RECORD("cccc");
    CHECK(is(1, "bbbb") == 0 && is(2, "aaaa") == 0);
    CHECK(rotmos_access_file_close(file) == 0 && clear() == 0);
    CHECK(put(0, "aaaa") == 0 && put(1, "old1") == 0);
    path_for(path, sizeof(path), 2);
    CHECK(mkdir(path, 0700) == 0);
    CHECK(rotmos_access_file_open(&config.http, &file, &error) == ROTMOS_OK);
    CHECK(rotmos_access_file_write(file, "bbbb", 4, &error) == ROTMOS_IO);
    CHECK(is(0, "aaaa") == 0 && is(1, "old1") == 0);
    CHECK(rmdir(path) == 0);
    RECORD("bbbb");
    CHECK(rotmos_access_file_close(file) == 0 && clear() == 0);
    config.http.access_log_max_bytes.low = 64;
    CHECK(rotmos_access_file_open(&config.http, &file, &error) == ROTMOS_OK);
    write_mode = 1;
    CHECK(rotmos_access_file_write(file, "full\n", 5, &error) == ROTMOS_IO &&
          error.native_code == ENOSPC);
    CHECK(is(0, "") == 0);
    write_mode = 2;
    write_calls = 0;
    CHECK(rotmos_access_file_write(file, "partial\n", 8, &error) == ROTMOS_IO &&
          error.native_code == ENOSPC);
    CHECK(is(0, "") == 0);
    write_mode = 3;
    write_calls = 0;
    RECORD("interrupted\n");
    write_mode = 4;
    RECORD("short\n");
    CHECK(is(0, "interrupted\nshort\n") == 0);
    write_mode = 0;
    CHECK(rotmos_access_file_close(file) == 0 && clear() == 0);
    CHECK(mkfifo(active_path, 0600) == 0);
    CHECK(rotmos_access_file_open(&config.http, &file, &error) == ROTMOS_IO &&
          file == NULL);
    CHECK(unlink(active_path) == 0);
    CHECK(symlink("missing", active_path) == 0);
    CHECK(rotmos_access_file_open(&config.http, &file, &error) == ROTMOS_IO &&
          file == NULL);
    CHECK(unlink(active_path) == 0);
    return 0;
}
struct fixture {
    pid_t child;
    int fault[2];
};
static rotmos_status route(rotmos_request *request, rotmos_reply *reply,
                           void *data, rotmos_error *error)
{
    (void)data;
    if (strcmp(rotmos_request_path(request).data, "/stop") == 0)
        rotmos_server_request_stop(rotmos_request_server(request), NULL);
    return rotmos_reply_text(reply, 200, "text/plain", "ok", error);
}
static int request(unsigned short port, const char *path)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char input[256], output[512];
    size_t used = 0;
    ssize_t count;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
          0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    snprintf(input, sizeof(input),
             "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
             path);
    if (strcmp(path, "/large-record") == 0) {
        char *agent = malloc(20000);
        size_t sent = 0;
        CHECK(agent != NULL);
        memset(agent, 'x', 20000);
        snprintf(input, sizeof(input),
                 "GET %s HTTP/1.1\r\nHost: localhost\r\nUser-Agent: ", path);
        CHECK(write(fd, input, strlen(input)) == (ssize_t)strlen(input));
        while (sent < 20000) {
            count = write(fd, agent + sent, 20000 - sent);
            CHECK(count > 0);
            sent += (size_t)count;
        }
        free(agent);
        CHECK(write(fd, "\r\nConnection: close\r\n\r\n", 23) == 23);
    } else
        CHECK(write(fd, input, strlen(input)) == (ssize_t)strlen(input));
    for (;;) {
        count = read(fd, output + used, sizeof(output) - used - 1);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        used += (size_t)count;
    }
    CHECK(count == 0);
    output[used] = 0;
    CHECK(strncmp(output, "HTTP/1.1 200 ", 13) == 0 &&
          strstr(output, "\r\n\r\nok") != NULL);
    CHECK(close(fd) == 0);
    return 0;
}
static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process, void *data,
                           rotmos_error *error)
{
    struct fixture *fixture = data;
    rotmos_bound_listener listeners[2];
    int i, result = 0;
    char path[32];
    char notified;
    struct pollfd event;
    (void)process;
    if (rotmos_server_listener_at(server, 0, &listeners[0], error) !=
            ROTMOS_OK ||
        rotmos_server_listener_at(server, 1, &listeners[1], error) != ROTMOS_OK)
        return ROTMOS_IO;
    if (pipe(fixture->fault) != 0)
        return ROTMOS_IO;
    notice_fd = fixture->fault[1];
    fixture->child = fork();
    if (fixture->child == -1)
        return ROTMOS_IO;
    if (fixture->child == 0) {
        close(fixture->fault[1]);
        notice_fd = -1;
        /* Enough bounded records to exercise non-forced parent drains too. */
        for (i = 0; i < 40 && result == 0; ++i)
            result = request(listeners[i % 2].port, "/large-record");
        for (i = 0; i < 20 && result == 0; ++i) {
            snprintf(path, sizeof(path), "/entry-%02d", i);
            result = request(listeners[i % 2].port, path);
        }
        if (result == 0)
            result = request(listeners[0].port, "/disk-full");
        if (result == 0) {
            event.fd = fixture->fault[0];
            event.events = POLLIN;
            event.revents = 0;
            if (poll(&event, 1, 3000) != 1 || read(event.fd, &notified, 1) != 1)
                result = 1;
        }
        if (result == 0)
            result = request(listeners[1].port, "/recovered");
        if (result == 0)
            result = request(listeners[0].port, "/stop");
        if (result != 0)
            kill(getppid(), SIGTERM);
        _exit(result);
    }
    close(fixture->fault[0]);
    return ROTMOS_OK;
}
static int live_case(const char *directory)
{
    rotmos_server_config config;
    rotmos_listener_config listeners[2];
    rotmos_route_config routes;
    rotmos_access_log_config policy;
    rotmos_server *server;
    rotmos_run_result result;
    rotmos_error error;
    pslog_config log_config;
    pslog_logger *root;
    struct fixture fixture;
    struct stat st;
    char path[288], diagnostic[4096], *line = NULL;
    FILE *input;
    size_t capacity = 0, length, records = 0;
    ssize_t count;
    int i, status, lines;
    snprintf(path, sizeof(path), "%s/diagnostic.log", directory);
    pslog_default_config(&log_config);
    log_config.timestamps = 0;
    CHECK(pslog_output_init_file(&log_config.output, path, "a") == 0);
    root = pslog_new(&log_config);
    CHECK(root != NULL);
    rotmos_server_config_init(&config);
    for (i = 0; i < 2; ++i) {
        rotmos_listener_config_init(&listeners[i]);
        listeners[i].bind = "127.0.0.1";
    }
    config.listeners = listeners;
    config.listener_count = config.workers.count = 2;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.http.access_log_path = active_path;
    config.http.access_log_max_bytes.low = 512;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.userdata = &fixture;
    fixture.child = -1;
    fixture.fault[0] = fixture.fault[1] = -1;
    write_mode = 5;
    open_failures = 1;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    rotmos_access_log_config_init(&policy);
    policy.logger_disabled = 1;
    CHECK(rotmos_server_set_pslog(server, root, &policy, &error) == ROTMOS_OK);
    rotmos_route_config_init(&routes);
    routes.path = "^/";
    routes.path_kind = ROTMOS_PATH_REGEX;
    routes.handler = route;
    CHECK(rotmos_router_route(rotmos_server_router(server), &routes, NULL,
                              &error) == ROTMOS_OK);
    CHECK(rotmos_server_run(server, &result, &error) == ROTMOS_OK &&
          !result.escalated);
    CHECK(waitpid(fixture.child, &status, 0) == fixture.child &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(close(fixture.fault[1]) == 0);
    notice_fd = -1;
    CHECK(last_file_fd >= 0 && fcntl(last_file_fd, F_GETFD) == -1 &&
          errno == EBADF);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    root->destroy(root);
    write_mode = 0;
    input = fopen(path, "rb");
    CHECK(input != NULL);
    length = fread(diagnostic, 1, sizeof(diagnostic) - 1, input);
    diagnostic[length] = 0;
    CHECK(fclose(input) == 0 &&
          strstr(diagnostic, "access_log_write") != NULL &&
          strstr(diagnostic, "No space left on device") != NULL &&
          strstr(diagnostic, "record dropped") != NULL &&
          strstr(diagnostic, "HTTP service remains available") != NULL);
    CHECK(unlink(path) == 0);
    for (i = 0; i < 3; ++i) {
        path_for(path, sizeof(path), (unsigned int)i);
        CHECK(stat(path, &st) == 0 && st.st_size > 0);
        input = fopen(path, "rb");
        CHECK(input != NULL);
        lines = 0;
        while ((count = getline(&line, &capacity, input)) >= 0) {
            CHECK(count > 0 && line[count - 1] == '\n' &&
                  strncmp(line, "127.0.0.1 -", 11) == 0 &&
                  strstr(line, "/disk-full") == NULL);
            ++records;
            ++lines;
            if (st.st_size > 512)
                CHECK(lines == 1 && strstr(line, "/large-record") != NULL);
        }
        CHECK(fclose(input) == 0);
    }
    free(line);
    CHECK(records > 0 && absent(3));
    CHECK(clear() == 0);
    return 0;
}
int main(void)
{
    char directory[] = "rotmos-access-log-XXXXXX";
    signal(SIGPIPE, SIG_IGN);
    CHECK(mkdtemp(directory) != NULL);
    snprintf(active_path, sizeof(active_path), "%s/access.log", directory);
    CHECK(file_cases() == 0);
    CHECK(live_case(directory) == 0);
    CHECK(rmdir(directory) == 0);
    return 0;
}
