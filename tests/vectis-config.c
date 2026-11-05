#define _POSIX_C_SOURCE 200809L
#include "vectis-server.h"
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            fprintf(stderr, "Vectis config %d: %s\n", __LINE__, #test);        \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static int settings(void)
{
    struct reference_server_config source;
    rotmos_server *server;
    rotmos_server_effective effective;
    rotmos_error error;
    char header[] = "compat", event[] = "host.event", proxy[] = "127.0.0.1";
    const char *trusted[1];
    int scenario;
    trusted[0] = proxy;
    reference_server_config_init(&source);
    CHECK(reference_server_new(&source, NULL, 0, NULL, NULL, &server, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_server_effective_config(server, &effective, &error) ==
              ROTMOS_OK &&
          effective.config.timeouts.deferred_response.disabled);
    CHECK(rotmos_server_effective_config(server, &effective, &error) ==
          ROTMOS_OK);
    CHECK(effective.requested_worker_count == 0 &&
          effective.config.workers.count >= 1 &&
          effective.config.workers.count <= 253);
    CHECK(effective.config.limits.max_connections == 1024 &&
          effective.config.limits.request_limit == 1024);
    CHECK(effective.config.workers.accept_threshold == 16 &&
          effective.config.workers.rlimit_nofiles == 768 &&
          effective.config.workers.shutdown_timeout_ms == 5000);
    CHECK(effective.config.limits.max_request_header_bytes == 65536UL &&
          effective.config.limits.hard_body_bytes.low == 0 &&
          effective.config.limits.default_body_bytes.low == 10485760UL);
    CHECK(effective.config.timeouts.request_header.milliseconds == 5000 &&
          effective.config.timeouts.request_body_idle.milliseconds == 15000 &&
          effective.config.timeouts.response_write_idle.milliseconds == 15000);
    CHECK(effective.config.limits.body_min_rate_bytes_per_second.low == 1024 &&
          effective.config.timeouts.body_min_rate_grace.milliseconds == 10000 &&
          effective.config.timeouts.connection_idle.milliseconds == 30000);
    CHECK(effective.config.http.keepalive_max_requests == 100 &&
          effective.config.timeouts.keepalive.milliseconds == 5000 &&
          effective.config.http.backend_curl_timeout_seconds == 60 &&
          effective.config.http.backend_curl_recv_max_bytes.low == 2097152UL);
    CHECK(effective.config.http.socket_backlog == 5000 &&
          effective.config.http.request_process_budget_ms == 100 &&
          effective.config.http.hsts_max_age_seconds == 0);
    CHECK(effective.config.limits.websocket_max_frame_bytes == 16384 &&
          effective.config.timeouts.websocket_idle.milliseconds == 120000 &&
          !strcmp(effective.config.http.server_header, "vectis"));
    CHECK(!effective.config.http.pretty_error_pages &&
          effective.config.workers.death_policy == ROTMOS_WORKER_RESTART &&
          effective.config.autoblock.window_seconds == 1800 &&
          effective.config.autoblock.block_seconds == 3600 &&
          effective.config.autoblock.max_entries == 8192);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    memset(&source, 0, sizeof(source));
    for (scenario = 0; scenario < 2; ++scenario) {
        CHECK(reference_server_new(scenario ? &source : NULL, NULL, 0, NULL,
                                   NULL, &server, &error) == ROTMOS_OK);
        CHECK(rotmos_server_effective_config(server, &effective, &error) ==
                  ROTMOS_OK &&
              effective.config.limits.max_connections == 1024 &&
              effective.config.limits.request_limit == 1024 &&
              effective.config.timeouts.request_header.milliseconds == 5000 &&
              !strcmp(effective.config.http.server_header, "vectis"));
        CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    }
    reference_server_config_init(&source);
    source.max_connections = 7;
    source.request_limit = 11;
    source.worker_count = 2;
    source.worker_accept_threshold = 5;
    source.worker_rlimit_nofiles = 256;
    source.worker_set_affinity_disabled = 1;
    source.worker_shutdown_timeout_ms = 1234;
    source.max_request_header_bytes = 32768;
    source.max_request_body_bytes = 33554432UL;
    source.request_body_spool_dir = "reference-spool";
    source.request_header_timeout_ms = 5001;
    source.request_body_idle_timeout_ms = 6001;
    source.response_write_idle_timeout_ms = 8764;
    source.request_body_min_rate_bytes_per_sec = 4096;
    source.request_body_min_rate_grace_ms = 8765;
    source.idle_timeout_ms = 8763;
    source.keepalive_timeout_ms = 7001;
    source.keepalive_max_requests = 8;
    source.kore_curl_timeout_seconds = 42;
    source.kore_curl_recv_max_bytes = 92160;
    source.kore_quiet = 1;
    source.socket_backlog = 128;
    source.request_process_budget_ms = 17;
    source.hsts_max_age_seconds = 997;
    source.websocket_max_frame_bytes = 32768;
    source.websocket_timeout_ms = 9876;
    source.server_header = header;
    source.access_log_path = "reference-access.log";
    source.pretty_error_pages = 1;
    source.worker_death_policy = ROTMOS_WORKER_TERMINATE;
    source.client_ip.trusted_proxies = trusted;
    source.client_ip.trusted_proxy_count = 1;
    source.autoblock.enabled = 1;
    source.autoblock.window_seconds = 37;
    source.autoblock.block_seconds = 73;
    source.autoblock.max_entries = 64;
    source.autoblock.tcp_stall_threshold = 3;
    source.autoblock.tls_failure_threshold = 7;
    source.autoblock.status_rule_count = 1;
    source.autoblock.status_rules[0].status = 418;
    source.autoblock.status_rules[0].threshold = 2;
    source.autoblock.event_rule_count = 1;
    source.autoblock.event_rules[0].name = event;
    source.autoblock.event_rules[0].threshold = 4;
    CHECK(reference_server_new(&source, NULL, 0, NULL, NULL, &server, &error) ==
          ROTMOS_OK);
    header[0] = event[0] = proxy[0] = 'X';
    source.autoblock.status_rules[0].status = 500;
    CHECK(rotmos_server_effective_config(server, &effective, &error) ==
          ROTMOS_OK);
    CHECK(effective.config.limits.max_connections == 7 &&
          effective.config.limits.request_limit == 11 &&
          effective.requested_worker_count == 2 &&
          effective.config.workers.count == 2);
    CHECK(effective.config.workers.accept_threshold == 5 &&
          effective.config.workers.rlimit_nofiles == 256 &&
          effective.config.workers.affinity_disabled &&
          effective.config.workers.shutdown_timeout_ms == 1234);
    CHECK(effective.config.limits.max_request_header_bytes == 32768 &&
          effective.config.limits.hard_body_bytes.low == 33554432UL &&
          effective.config.limits.default_body_bytes.low == 33554432UL &&
          !strcmp(effective.config.limits.body_spool_directory,
                  "reference-spool"));
    CHECK(effective.config.timeouts.request_header.milliseconds == 6000 &&
          effective.requested_timeouts.request_header.milliseconds == 5001 &&
          effective.config.timeouts.request_body_idle.milliseconds == 7000 &&
          effective.config.timeouts.response_write_idle.milliseconds == 8764);
    CHECK(effective.config.limits.body_min_rate_bytes_per_second.low == 4096 &&
          effective.config.timeouts.body_min_rate_grace.milliseconds == 8765 &&
          effective.config.timeouts.connection_idle.milliseconds == 8763);
    CHECK(effective.config.timeouts.keepalive.milliseconds == 8000 &&
          effective.config.http.keepalive_max_requests == 8 &&
          effective.config.http.backend_curl_timeout_seconds == 42 &&
          effective.config.http.backend_curl_recv_max_bytes.low == 92160);
    CHECK(effective.config.http.backend_quiet &&
          effective.config.http.socket_backlog == 128 &&
          effective.config.http.request_process_budget_ms == 17 &&
          effective.config.http.hsts_max_age_seconds == 997);
    CHECK(
        effective.config.limits.websocket_max_frame_bytes == 32768 &&
        effective.config.timeouts.websocket_idle.milliseconds == 9876 &&
        !strcmp(effective.config.http.server_header, "compat") &&
        !strcmp(effective.config.http.access_log_path, "reference-access.log"));
    CHECK(effective.config.http.pretty_error_pages &&
          effective.config.workers.death_policy == ROTMOS_WORKER_TERMINATE &&
          !strcmp(effective.config.client_ip.trusted_proxies[0], "127.0.0.1"));
    CHECK(effective.config.autoblock.enabled &&
          effective.config.autoblock.window_seconds == 37 &&
          effective.config.autoblock.block_seconds == 73 &&
          effective.config.autoblock.max_entries == 64 &&
          effective.config.autoblock.tcp_stall_threshold == 3 &&
          effective.config.autoblock.tls_failure_threshold == 7);
    CHECK(
        effective.config.autoblock.status_rules[0].status == 418 &&
        effective.config.autoblock.status_rules[0].threshold == 2 &&
        !strcmp(effective.config.autoblock.event_rules[0].name, "host.event") &&
        effective.config.autoblock.event_rules[0].threshold == 4);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    reference_server_config_init(&source);
    source.request_header_timeout_ms = 65535001L;
    source.request_body_idle_timeout_ms = LONG_MAX;
    source.keepalive_timeout_ms = 65534001L;
    CHECK(reference_server_new(&source, NULL, 0, NULL, NULL, &server, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_server_effective_config(server, &effective, &error) ==
              ROTMOS_OK &&
          effective.config.timeouts.request_header.milliseconds == 65535000L &&
          effective.config.timeouts.request_body_idle.milliseconds ==
              65535000L &&
          effective.config.timeouts.keepalive.milliseconds == 65535000L &&
          effective.requested_timeouts.request_body_idle.milliseconds ==
              LONG_MAX);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    source.keepalive_disabled = 1;
    source.keepalive_timeout_ms = -1;
    CHECK(reference_server_new(&source, NULL, 0, NULL, NULL, &server, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    reference_server_config_init(&source);
    source.worker_count = 254;
    CHECK(reference_server_new(&source, NULL, 0, NULL, NULL, &server, &error) ==
              ROTMOS_INVALID &&
          server == NULL);
    source.worker_count = 1;
    source.access_log.success_level = (pslog_level)999;
    CHECK(reference_server_new(&source, NULL, 0, NULL, NULL, &server, &error) ==
              ROTMOS_INVALID &&
          server == NULL);
    return 0;
}
struct lifecycle {
    int phase, worker_phase, notes[2], control[2], supervised, controls;
};
static rotmos_status parent_prepare(rotmos_server *server,
                                    const rotmos_process_context *process,
                                    void *data, rotmos_error *error)
{
    struct lifecycle *life = data;
    (void)server;
    (void)error;
    if (process->role != ROTMOS_PROCESS_PARENT || life->phase != 0)
        return ROTMOS_STATE;
    life->phase = 1;
    return ROTMOS_OK;
}
static rotmos_status parent_ready(rotmos_server *server,
                                  const rotmos_process_context *process,
                                  void *data, rotmos_error *error)
{
    struct lifecycle *life = data;
    rotmos_bound_listener listener;
    if (process->role != ROTMOS_PROCESS_PARENT || life->phase != 1 ||
        rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK ||
        listener.port == 0)
        return ROTMOS_STATE;
    life->phase = 2;
    if (life->supervised) {
        if (rotmos_server_get_state(server) != ROTMOS_SERVER_READY_PAUSED)
            return ROTMOS_STATE;
        return write(life->control[1], "R", 1) == 1 ? ROTMOS_OK : ROTMOS_IO;
    }
    return rotmos_server_request_stop(server, error);
}
static rotmos_status control_ready(rotmos_server *server, unsigned int events,
                                   void *data, rotmos_error *error)
{
    struct lifecycle *life = data;
    char message;
    if (!(events & ROTMOS_CONTROL_READ) || life->phase != 2 || life->controls ||
        read(life->control[0], &message, 1) != 1 || message != 'R')
        return ROTMOS_STATE;
    ++life->controls;
    if (rotmos_server_set_serving(server, 1, error) != ROTMOS_OK)
        return ROTMOS_STATE;
    return rotmos_server_request_stop(server, error);
}
static rotmos_status parent_stopping(rotmos_server *server,
                                     const rotmos_process_context *process,
                                     void *data, rotmos_error *error)
{
    struct lifecycle *life = data;
    (void)server;
    (void)process;
    (void)error;
    if (life->phase != 2)
        return ROTMOS_STATE;
    life->phase = 3;
    return ROTMOS_OK;
}
static rotmos_status parent_stopped(rotmos_server *server,
                                    const rotmos_process_context *process,
                                    void *data, rotmos_error *error)
{
    struct lifecycle *life = data;
    (void)server;
    (void)process;
    (void)error;
    if (life->phase != 3)
        return ROTMOS_STATE;
    life->phase = 4;
    return ROTMOS_OK;
}
static rotmos_status worker_start(rotmos_server *server,
                                  const rotmos_process_context *process,
                                  void *data, rotmos_error *error)
{
    struct lifecycle *life = data;
    (void)server;
    (void)error;
    if (process->role != ROTMOS_PROCESS_HTTP ||
        process->process_id != (long)getpid() || life->worker_phase != 0)
        return ROTMOS_STATE;
    life->worker_phase = 1;
    return write(life->notes[1], "W", 1) == 1 ? ROTMOS_OK : ROTMOS_IO;
}
static rotmos_status worker_stopping(rotmos_server *server,
                                     const rotmos_process_context *process,
                                     void *data, rotmos_error *error)
{
    struct lifecycle *life = data;
    (void)server;
    (void)process;
    (void)error;
    if (life->worker_phase != 1)
        return ROTMOS_STATE;
    life->worker_phase = 2;
    return write(life->notes[1], "S", 1) == 1 ? ROTMOS_OK : ROTMOS_IO;
}
static int worker_exit(rotmos_server *server,
                       const rotmos_process_context *process, void *data)
{
    struct lifecycle *life = data;
    (void)server;
    (void)process;
    return life->worker_phase == 2 && write(life->notes[1], "X", 1) == 1 ? 0
                                                                         : 1;
}
static int run(int supervised)
{
    struct reference_server_config source;
    struct lifecycle life;
    rotmos_server_callbacks callbacks;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_server *server;
    rotmos_run_result result;
    rotmos_error error;
    struct pollfd poller;
    char notes[3];
    memset(&life, 0, sizeof(life));
    life.supervised = supervised;
    memset(&callbacks, 0, sizeof(callbacks));
    CHECK(pipe(life.notes) == 0);
    reference_server_config_init(&source);
    source.worker_count = 1;
    source.worker_set_affinity_disabled = source.kore_quiet = 1;
    source.access_log.logger_disabled = 1;
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    callbacks.on_parent_prepare = parent_prepare;
    callbacks.on_parent_ready = parent_ready;
    callbacks.on_parent_stopping = parent_stopping;
    callbacks.on_parent_stopped = parent_stopped;
    callbacks.on_worker_start = worker_start;
    callbacks.on_worker_stopping = worker_stopping;
    callbacks.on_worker_exit = worker_exit;
    callbacks.userdata = &life;
    if (supervised) {
        CHECK(pipe(life.control) == 0);
        CHECK(reference_server_translate(&source, &config, &error) ==
              ROTMOS_OK);
        config.listeners = &listener;
        config.listener_count = 1;
        config.callbacks = callbacks;
        config.start_paused = 1;
        config.signals.mode = ROTMOS_SIGNALS_HOST;
        config.supervision.enabled = 1;
        config.supervision.control_fd = life.control[0];
        config.supervision.on_event = control_ready;
        config.supervision.userdata = &life;
        CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    } else
        CHECK(reference_server_new(&source, &listener, 1, &callbacks, NULL,
                                   &server, &error) == ROTMOS_OK);
    CHECK(rotmos_server_run(server, &result, &error) == ROTMOS_OK &&
          life.phase == 4 && result.reason == ROTMOS_RUN_REQUESTED_STOP &&
          !result.escalated && !result.has_failure);
    poller.fd = life.notes[0];
    poller.events = POLLIN;
    poller.revents = 0;
    CHECK(poll(&poller, 1, 100) == 1 &&
          read(life.notes[0], notes, sizeof(notes)) == 3 &&
          !memcmp(notes, "WSX", 3));
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    if (supervised) {
        CHECK(life.controls == 1 && fcntl(life.control[0], F_GETFD) == -1 &&
              errno == EBADF);
        close(life.control[1]);
    }
    close(life.notes[0]);
    close(life.notes[1]);
    return 0;
}
int main(void) { return settings() || run(0) || run(1); }
