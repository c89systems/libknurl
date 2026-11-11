#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <knurl/pslog.h>
#include "vectis-server.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "policy/logs %d: %s\n", __LINE__, #t);             \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    pid_t child;
    int mode;
    int failed;
    char directory[64], access[96], root[96], override[96];
};
static void request_closed(knurl_server *server, knurl_request *request,
                           const knurl_request_outcome *outcome, void *data)
{
    struct fixture *f = data;
    knurl_bytes path = knurl_request_path(request);
    (void)server;
    if (path.length == 9 && !memcmp(path.data, "/continue", 9) &&
        (outcome->response.interim_wire_bytes.low != 25 ||
         outcome->response.interim_wire_bytes.high != 0 ||
         outcome->response.body_bytes.low != 3 ||
         outcome->response.wire_bytes.low <= 28))
        f->failed = 1;
}
static int worker_exit(knurl_server *server,
                       const knurl_process_context *process, void *data)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    return f->failed;
}
static void block_notification(knurl_server *server,
                               const knurl_autoblock_event *event, void *data)
{
    struct fixture *f = data;
    (void)server;
    if (event->process.role != KNURL_PROCESS_HTTP ||
        event->process.process_id <= 0 ||
        strcmp(event->peer_address, "127.0.0.1") || event->peer_port == 0 ||
        event->local_port == 0 || event->count < event->threshold)
        f->failed = 1;
}
static int check_notifications(knurl_server *server)
{
    knurl_autoblock_event event;
    knurl_u64 dropped;
    knurl_error error;
    knurl_status status;
    int seen = 0;
    while ((status = server->autoblock_next(server, &event, &dropped,
                                            &error)) == KNURL_OK) {
        CHECK(dropped.high == 0 && dropped.low == 0);
        CHECK(event.change == KNURL_AUTOBLOCK_BLOCKED);
        CHECK(event.process.role == KNURL_PROCESS_HTTP &&
              event.process.process_id > 0);
        CHECK(event.expires_at_ms.high > event.occurred_at_ms.high ||
              (event.expires_at_ms.high == event.occurred_at_ms.high &&
               event.expires_at_ms.low > event.occurred_at_ms.low));
        if (!strcmp(event.address, "198.51.100.10")) {
            CHECK(event.reason == KNURL_AUTOBLOCK_HTTP_STATUS &&
                  event.status == 404);
            CHECK(event.address_source == KNURL_CLIENT_ADDRESS_FORWARDED_FOR);
            CHECK(event.via_count == 2 && !event.via_truncated);
            CHECK(!strcmp(event.via[0], "10.0.0.1") &&
                  !strcmp(event.via[1], "127.0.0.1"));
            seen |= 1;
        } else if (!strcmp(event.address, "198.51.100.12")) {
            CHECK(event.reason == KNURL_AUTOBLOCK_APPLICATION &&
                  !strcmp(event.name, "auth.failed"));
            seen |= 2;
        } else if (!strcmp(event.address, "198.51.100.30")) {
            CHECK(event.reason == KNURL_AUTOBLOCK_HTTP_STATUS &&
                  event.status == 100);
            seen |= 4;
        } else if (!strcmp(event.address, "127.0.0.1")) {
            CHECK(event.reason == KNURL_AUTOBLOCK_TCP_STALL ||
                  (event.reason == KNURL_AUTOBLOCK_HTTP_STATUS &&
                   event.status == 400));
            CHECK(event.via_count == 0 &&
                  event.address_source == KNURL_CLIENT_ADDRESS_PEER);
            seen |= 8;
        } else
            CHECK(0);
    }
    CHECK(status == KNURL_EOF && seen == 15 && dropped.low == 0);
    return 0;
}
static knurl_status identifier(knurl_request *request, char *buffer,
                               size_t capacity, size_t *length, void *data,
                               knurl_error *error)
{
    knurl_bytes value;
    (void)data;
    (void)error;
    *length = 0;
    if (knurl_request_header_first(request, "x-id", &value, NULL) == KNURL_OK) {
        if (value.length >= capacity)
            return KNURL_LIMIT;
        memcpy(buffer, value.data, value.length);
        *length = value.length;
    }
    return KNURL_OK;
}
static knurl_status waiting_source(void *buffer, size_t capacity,
                                   size_t *produced, void *data,
                                   knurl_error *error)
{
    (void)buffer;
    (void)capacity;
    (void)data;
    (void)error;
    *produced = 0;
    return KNURL_AGAIN;
}
static knurl_status handler(knurl_request *request, knurl_reply *reply,
                            void *data, knurl_error *error)
{
    const char *path = knurl_request_path(request).data;
    struct fixture *f = data;
    knurl_bytes empty = {0};
    int status = 200;
    (void)f;
    if (!strcmp(path, "/abandon")) {
        knurl_source_config config;
        knurl_source *source;
        knurl_status status;
        knurl_source_config_init(&config);
        config.read = waiting_source;
        status = knurl_source_new(&config, &source, error);
        if (status == KNURL_OK) {
            status = knurl_reply_source_take(reply, 200, "text/plain", source,
                                             error);
            if (status != KNURL_OK)
                knurl_source_destroy(source, NULL);
        }
        return status;
    }
    if (!strcmp(path, "/event")) {
        if (knurl_request_event(request, "auth.failed", error) != KNURL_OK)
            return KNURL_STATE;
        return knurl_reply_status(reply, 204, error);
    }
    if (!strcmp(path, "/replace")) {
        if (knurl_response_begin(knurl_request_response(request), error) !=
                KNURL_OK ||
            knurl_response_replace_uncommitted(knurl_request_response(request),
                                               error) != KNURL_OK)
            return KNURL_STATE;
        return knurl_response_bytes(knurl_request_response(request), 418,
                                    "text/plain", empty, error);
    }
    if (!strcmp(path, "/ok") || !strcmp(path, "/continue"))
        status = 201;
    if (!strcmp(path, "/disabled"))
        status = 202;
    if (!strcmp(path, "/warn"))
        status = 409;
    if (!strcmp(path, "/error"))
        status = 503;
    if (!strcmp(path, "/missing"))
        status = 404;
    return knurl_reply_text(reply, status, "text/plain", "one", error);
}
static int connect_to(unsigned short port)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static int request(unsigned short port, const char *path, const char *ip,
                   int expected)
{
    char input[1024], output[1024], prefix[32], id[101], *start;
    size_t used = 0;
    ssize_t n;
    struct linger reset;
    int fd = connect_to(port);
    CHECK(fd >= 0);
    memset(id, 'r', 100);
    id[100] = 0;
    snprintf(input, sizeof(input),
             "GET %s HTTP/1.1\r\nHost: localhost\r\nX-Forwarded-For: "
             "%s\r\nX-ID: %s\r\nCookie: secret-cookie\r\nAuthorization: "
             "secret-auth\r\nConnection: close\r\n\r\n",
             path, ip, id);
    if (!strcmp(path, "/continue"))
        snprintf(input, sizeof(input),
                 "POST /continue HTTP/1.1\r\nHost: localhost\r\n"
                 "X-Forwarded-For: %s\r\nContent-Length: 3\r\n"
                 "Expect: 100-continue\r\nConnection: close\r\n\r\nabc",
                 ip);
    if (!strcmp(path, "/old-continue"))
        snprintf(input, sizeof(input),
                 "POST /old-continue HTTP/1.0\r\nHost: localhost\r\n"
                 "Content-Length: 3\r\nExpect: 100-continue\r\n\r\nabc");
    n = write(fd, input, strlen(input));
    CHECK(n == (ssize_t)strlen(input) || expected == 0);
    if (!strcmp(path, "/abandon")) {
        n = read(fd, output, sizeof(output));
        CHECK(n > 0);
        reset.l_onoff = 1;
        reset.l_linger = 0;
        CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) ==
              0);
        close(fd);
        return 0;
    }
    while (used < sizeof(output) - 1 &&
           (n = read(fd, output + used, sizeof(output) - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    output[used] = 0;
    if (expected == 0) {
        CHECK(used == 0);
        return 0;
    }
    snprintf(prefix, sizeof(prefix), "%s %d ",
             !strcmp(path, "/old-continue") ? "HTTP/1.0" : "HTTP/1.1",
             expected);
    start = output;
    if (!strncmp(start, "HTTP/1.1 100 ", 13)) {
        start = strstr(start, "\r\n\r\n");
        CHECK(start != NULL);
        start += 4;
    }
    if (strncmp(start, prefix, strlen(prefix)))
        fprintf(stderr, "reply %s\n", output);
    CHECK(!strncmp(start, prefix, strlen(prefix)));
    return 0;
}
static int client(unsigned short port, int mode)
{
    int fd, i;
    char bytes[1024];
    ssize_t n;
    CHECK(request(port, "/ok", "198.51.100.1", 201) == 0);
    CHECK(request(port, "/trace", "198.51.100.1", 200) == 0);
    CHECK(request(port, "/disabled", "198.51.100.1", 202) == 0);
    CHECK(request(port, "/warn", "198.51.100.1", 409) == 0);
    CHECK(request(port, "/error", "198.51.100.1", 503) == 0);
    CHECK(request(port, "/replace", "198.51.100.1", 418) == 0);
    CHECK(request(port, "/missing", "198.51.100.10, 10.0.0.1", 404) == 0);
    CHECK(request(port, "/missing", "198.51.100.10, 10.0.0.1", 404) == 0);
    CHECK(request(port, "/ok", "198.51.100.10", 403) == 0);
    CHECK(request(port, "/ok", "198.51.100.11", 201) == 0);
    CHECK(request(port, "/event", "198.51.100.12", 204) == 0);
    CHECK(request(port, "/event", "198.51.100.12", 204) == 0);
    CHECK(request(port, "/ok", "198.51.100.12", 403) == 0);
    CHECK(request(port, "/continue", "198.51.100.30", 201) == 0);
    CHECK(request(port, "/continue", "198.51.100.30", 201) == 0);
    CHECK(request(port, "/ok", "198.51.100.30", 403) == 0);
    CHECK(request(port, "/old-continue", "198.51.100.30", 417) == 0);
    CHECK(request(port, "/abandon", "198.51.100.40", 200) == 0);
    for (i = 0; i < 2; ++i) {
        fd = connect_to(port);
        CHECK(fd >= 0);
        if (mode == 1) {
            CHECK(shutdown(fd, SHUT_WR) == 0);
            n = read(fd, bytes, sizeof(bytes));
            CHECK(n == 0 || (n < 0 && errno == ECONNRESET));
            close(fd);
        } else {
            const char *bad = "GET / HTTP/1.1\r\nHost: localhost\r\nHost: "
                              "bad\r\nConnection: close\r\n\r\n";
            CHECK(write(fd, bad, strlen(bad)) == (ssize_t)strlen(bad));
            n = read(fd, bytes, sizeof(bytes));
            CHECK(n > 0);
            close(fd);
        }
    }
    CHECK(request(port, "/ok", "198.51.100.20", 0) == 0);
    return 0;
}
static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *data,
                          knurl_error *error)
{
    struct fixture *f = data;
    knurl_bound_listener bound;
    int status;
    (void)process;
    if (knurl_server_set_pslog(server, NULL, NULL, NULL) != KNURL_STATE)
        return KNURL_INVALID;
    if (knurl_server_listener_at(server, 0, &bound, error) != KNURL_OK)
        return KNURL_STATE;
    if (knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return KNURL_STATE;
    f->child = fork();
    if (f->child < 0)
        return KNURL_IO;
    if (f->child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        status = client(bound.port, f->mode);
        kill(getppid(), SIGTERM);
        _exit(status);
    }
    return KNURL_OK;
}
static pslog_logger *logger(const char *path)
{
    pslog_config config;
    pslog_default_config(&config);
    config.mode = PSLOG_MODE_JSON;
    config.min_level = PSLOG_LEVEL_TRACE;
    config.timestamps = 0;
    if (pslog_output_init_file(&config.output, path, "a") != 0)
        return NULL;
    return pslog_new(&config);
}
static int logs(struct fixture *f)
{
    FILE *file;
    char line[1024], id[101];
    int info = 0, trace = 0, warn = 0, error = 0, replaced = 0, file_lines = 0;
    int runs = f->mode == 0 ? 2 : 1;
    int abandoned = 0;
    const char *path = f->mode == 2 ? f->override : f->root;
    memset(id, 'r', 100);
    id[100] = 0;
    file = fopen(path, "r");
    CHECK(file != NULL);
    while (fgets(line, sizeof(line), file) != NULL) {
        CHECK(strstr(line, "secret-cookie") == NULL &&
              strstr(line, "secret-auth") == NULL);
        if (!strstr(line, "\"msg\":\"http request\""))
            continue;
        CHECK(f->mode != 1 && strstr(line, "\"status\":202") == NULL);
        if (strstr(line, "\"path\":\"/ok\"") &&
            strstr(line, "\"status\":201")) {
            CHECK(strstr(line, "\"lvl\":\"info\"") &&
                  strstr(line, "\"bytes\":3"));
            CHECK(strstr(line, "\"client_ip\":\"127.0.0.1\"") &&
                  strstr(line, "\"real_ip\":\"198.51.100.") &&
                  strstr(line, id));
            ++info;
        }
        if (strstr(line, "\"path\":\"/trace\"")) {
            CHECK(strstr(line, "\"lvl\":\"trace\""));
            ++trace;
        }
        if (strstr(line, "\"path\":\"/warn\"")) {
            CHECK(strstr(line, "\"lvl\":\"warn\""));
            ++warn;
        }
        if (strstr(line, "\"path\":\"/error\"")) {
            CHECK(strstr(line, "\"lvl\":\"error\""));
            ++error;
        }
        if (strstr(line, "\"path\":\"/replace\"")) {
            CHECK(strstr(line, "\"status\":418") &&
                  strstr(line, "\"lvl\":\"info\""));
            ++replaced;
        }
        if (strstr(line, "\"path\":\"/abandon\"")) {
            CHECK(strstr(line, "\"client_ip\":\"127.0.0.1\"") &&
                  strstr(line, "\"real_ip\":\"198.51.100.40\"") &&
                  strstr(line, "\"bytes\":0"));
            ++abandoned;
        }
    }
    fclose(file);
    CHECK(abandoned == (f->mode == 1 ? 0 : runs));
    CHECK(f->mode == 1 ? info + trace + warn + error + replaced == 0
                       : info == 2 * runs && trace == runs && warn == runs &&
                             error == runs && replaced == runs);
    file = fopen(f->access, "r");
    CHECK(file != NULL);
    while (fgets(line, sizeof(line), file) != NULL) {
        CHECK(strstr(line, "secret-cookie") == NULL &&
              strstr(line, "secret-auth") == NULL);
        ++file_lines;
    }
    fclose(file);
    CHECK(file_lines == 17 * runs);
    return 0;
}
static int run(int mode)
{
    struct fixture f;
    struct reference_server_config host;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config route;
    knurl_access_log_config access_config;
    knurl_access_log_status_level_rule rules[3] = {{201, PSLOG_LEVEL_INFO},
                                                   {202, PSLOG_LEVEL_DISABLED},
                                                   {418, PSLOG_LEVEL_INFO}};
    knurl_autoblock_status_rule statuses[3] = {{404, 2}, {400, 2}, {100, 2}};
    knurl_autoblock_event_rule event = {"auth.failed", 2};
    const char *trusted[] = {"127.0.0.1", "10.0.0.1"};
    knurl_server *server;
    knurl_run_result result;
    knurl_autoblock_notifications_config notifications;
    knurl_error error;
    knurl_status status;
    pslog_logger *root, *override = NULL;
    char missing[128];
    int child;
    memset(&f, 0, sizeof(f));
    f.child = -1;
    f.mode = mode;
    strcpy(f.directory, "knurl-policy-logs-XXXXXX");
    CHECK(mkdtemp(f.directory) != NULL);
    snprintf(f.root, sizeof(f.root), "%s/root.log", f.directory);
    snprintf(f.access, sizeof(f.access), "%s/access.log", f.directory);
    snprintf(f.override, sizeof(f.override), "%s/override.log", f.directory);
    root = logger(f.root);
    CHECK(root != NULL);
    if (mode == 2) {
        override = logger(f.override);
        CHECK(override != NULL);
    }
    reference_server_config_init(&host);
    host.worker_count = 1;
    host.worker_set_affinity_disabled = host.kore_quiet = 1;
    host.client_ip.trusted_proxies = trusted;
    host.client_ip.trusted_proxy_count = 2;
    host.access_log_path = f.access;
    host.autoblock.enabled = 1;
    host.autoblock.tcp_stall_threshold = 2;
    memcpy(host.autoblock.status_rules, statuses, sizeof(statuses));
    host.autoblock.status_rule_count = 3;
    host.autoblock.event_rules[0] = event;
    host.autoblock.event_rule_count = 1;
    host.access_log.logger_disabled = mode == 1;
    host.access_log.logger = override;
    memcpy(host.access_log.status_level_rules, rules, sizeof(rules));
    host.access_log.status_level_rule_count = 3;
    CHECK(reference_server_translate(&host, &config, &error) == KNURL_OK);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_request_close = request_closed;
    config.callbacks.on_worker_exit = worker_exit;
    config.callbacks.userdata = &f;
    config.correlation_id.resolve = identifier;
    if (mode == 0) {
        snprintf(missing, sizeof(missing), "%s/missing/access.log",
                 f.directory);
        config.http.access_log_path = missing;
        CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
        CHECK(knurl_server_set_pslog(server, root, NULL, &error) == KNURL_OK);
        CHECK(knurl_server_validate(server, &error) == KNURL_OK);
        status = knurl_server_run(server, &result, &error);
        if (status != KNURL_BACKEND)
            fprintf(stderr, "startup status=%d %s: %s\n", status,
                    error.operation, error.message);
        CHECK(status == KNURL_BACKEND &&
              result.reason == KNURL_RUN_STARTUP_FAILURE && f.child == -1);
        CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
        config.http.access_log_path = f.access;
    }
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    knurl_autoblock_notifications_config_init(&notifications);
    notifications.queue_capacity = 32;
    notifications.callback = block_notification;
    notifications.userdata = &f;
    CHECK(server->set_autoblock_notifications(server, &notifications, &error) ==
          KNURL_OK);
    knurl_access_log_config_init(&access_config);
    access_config.logger_disabled = host.access_log.logger_disabled;
    access_config.logger = host.access_log.logger;
    access_config.status_level_rules = host.access_log.status_level_rules;
    access_config.status_level_rule_count =
        host.access_log.status_level_rule_count;
    CHECK(knurl_server_set_pslog(server, root, &access_config, &error) ==
          KNURL_OK);
    host.access_log.status_level_rules[0].status = 202;
    CHECK(knurl_server_set_pslog(server, root, &access_config, &error) ==
          KNURL_CONFLICT);
    host.access_log.status_level_rules[0].status = 999;
    CHECK(knurl_server_set_pslog(server, root, &access_config, &error) ==
          KNURL_INVALID);
    knurl_route_config_init(&route);
    route.path = "^/.*$";
    route.path_kind = KNURL_PATH_REGEX;
    route.handler = handler;
    route.userdata = &f;
    route.body.mode = KNURL_BODY_BUFFERED;
    route.body.max_bytes.low = route.body.memory_buffer_limit_bytes = 16;
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    CHECK(knurl_server_validate(server, &error) == KNURL_OK &&
          access(f.access, F_OK) != 0);
    status = knurl_server_run(server, &result, &error);
    if (status != KNURL_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(waitpid(f.child, &child, 0) == f.child && WIFEXITED(child) &&
          WEXITSTATUS(child) == 0);
    CHECK(status == KNURL_OK && !result.escalated);
    CHECK(check_notifications(server) == 0);
    if (mode == 0) {
        pslog(root, PSLOG_LEVEL_INFO, "between runs", NULL);
        CHECK(knurl_server_run(server, &result, &error) == KNURL_OK);
        CHECK(waitpid(f.child, &child, 0) == f.child && WIFEXITED(child) &&
              WEXITSTATUS(child) == 0 && !result.escalated);
        CHECK(check_notifications(server) == 0);
    }
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    pslog(root, PSLOG_LEVEL_INFO, "after destroy", NULL);
    root->destroy(root);
    if (override != NULL)
        override->destroy(override);
    CHECK(logs(&f) == 0);
    CHECK(unlink(f.root) == 0 && unlink(f.access) == 0);
    if (mode == 2)
        CHECK(unlink(f.override) == 0);
    CHECK(rmdir(f.directory) == 0);
    return 0;
}
int main(void)
{
    CHECK(run(0) == 0);
    CHECK(run(1) == 0);
    CHECK(run(2) == 0);
    return 0;
}
