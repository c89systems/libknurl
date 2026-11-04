#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "body_internal.h"

#include <sys/mman.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t runtime_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rotmos_runtime *active_runtime;
static const int handled_signals[] = {SIGINT, SIGTERM, SIGQUIT, SIGCHLD};

rotmos_status rotmos_runtime_hook(struct rotmos_runtime *runtime,
                                  rotmos_process_fn function)
{
    rotmos_status status;
    rotmos_error error;
    if (function == NULL)
        return ROTMOS_OK;
    rotmos_error_clear(&error);
    status = function(runtime->server, &runtime->server->process,
                      runtime->server->config.callbacks.userdata, &error);
    if (status != ROTMOS_OK && error.status == ROTMOS_OK)
        (void)rotmos_fail(&error, status, "server_hook",
                          "server lifecycle callback failed");
    if (status != ROTMOS_OK && runtime->status == ROTMOS_OK)
        runtime->error = error;
    return status;
}

static void fatal_error(void *userdata, const char *message)
{
    struct rotmos_runtime *runtime;
    runtime = userdata;
    if (getpid() != runtime->parent_pid)
        _exit(70);
    runtime->status =
        rotmos_fail(&runtime->error, ROTMOS_BACKEND, "kore_runtime", message);
    runtime->error.source = ROTMOS_ERROR_BACKEND;
    longjmp(runtime->unwind, 1);
}

static void signal_received(int number)
{
    if (active_runtime != NULL)
        rotmos_server_notify_signal(active_runtime->server, number);
}

void rotmos_server_notify_signal(rotmos_server *server, int number)
{
    int saved;
    unsigned char wake;
    ssize_t count;
    saved = errno;
    wake = 1;
    if (server != NULL && server->notify_fd >= 0) {
        if ((number == SIGINT || number == SIGTERM || number == SIGQUIT) &&
            server->terminating_signal == 0)
            server->terminating_signal = number;
        do {
            count = write((int)server->notify_fd, &wake, 1);
        } while (count == -1 && errno == EINTR);
    }
    errno = saved;
}

rotmos_status rotmos_server_request_stop(rotmos_server *server,
                                         rotmos_error *error)
{
    unsigned char wake;
    rotmos_status status;
    ssize_t count;
    wake = 1;
    status = ROTMOS_OK;
    (void)pthread_mutex_lock(&runtime_lock);
    if (server == NULL || server->runtime == NULL || server->notify_fd < 0 ||
        server->process.process_id != (long)getpid()) {
        status = rotmos_fail(error, ROTMOS_STATE, "server_request_stop",
                             "server has no active run");
    } else if (!server->stop_requested) {
        server->stop_requested = 1;
        do {
            count = write((int)server->notify_fd, &wake, 1);
        } while (count == -1 && errno == EINTR);
        if (count == -1 && errno != EAGAIN)
            status = rotmos_fail(error, ROTMOS_IO, "server_request_stop",
                                 "cannot wake runtime loop");
    }
    (void)pthread_mutex_unlock(&runtime_lock);
    if (status == ROTMOS_OK)
        rotmos_error_clear(error);
    return status;
}

static void wake_event(void *userdata, int failed)
{
    struct rotmos_runtime *runtime;
    unsigned char bytes[128];
    ssize_t count;
    (void)userdata;
    runtime = active_runtime;
    do {
        count = read(runtime->wake[0], bytes, sizeof(bytes));
    } while (count > 0 || (count == -1 && errno == EINTR));
    if (failed || runtime->server->stop_requested ||
        runtime->server->terminating_signal) {
        if (worker != NULL) {
            if (!runtime->stop_sent) {
                runtime->stop_sent = 1;
                kore_shutdown();
            }
        } else if (kore_quit == KORE_QUIT_NONE)
            kore_quit = KORE_QUIT_NORMAL;
    }
    runtime->wake_event.flags = 0;
}

static int wake_open(struct rotmos_runtime *runtime)
{
    int i;
    int flags;
    runtime->wake[0] = runtime->wake[1] = -1;
    if (pipe(runtime->wake) == -1)
        return 0;
    for (i = 0; i < 2; ++i) {
        flags = fcntl(runtime->wake[i], F_GETFL);
        if (flags == -1 ||
            fcntl(runtime->wake[i], F_SETFL, flags | O_NONBLOCK) == -1 ||
            fcntl(runtime->wake[i], F_SETFD, FD_CLOEXEC) == -1)
            return 0;
    }
    runtime->server->notify_fd = runtime->wake[1];
    runtime->wake_event.type = 0;
    runtime->wake_event.flags = 0;
    runtime->wake_event.handle = wake_event;
    return 1;
}

static void wake_close(struct rotmos_runtime *runtime)
{
    runtime->server->notify_fd = -1;
    if (runtime->wake[0] >= 0)
        (void)close(runtime->wake[0]);
    if (runtime->wake[1] >= 0)
        (void)close(runtime->wake[1]);
    runtime->wake[0] = runtime->wake[1] = -1;
}

static int child_start(void *userdata, struct kore_worker *child)
{
    struct rotmos_runtime *runtime;
    rotmos_process_context *process;
    runtime = userdata;
    process = &runtime->server->process;
    wake_close(runtime);
    kore_platform_event_cleanup();
    kore_timer_cleanup();
    rotmos_loop_discard_after_fork(runtime->server->loop);
    runtime->server->loop = NULL;
    runtime->control_watch = NULL;
    if (runtime->server->config.supervision.enabled &&
        runtime->server->config.supervision.control_fd >= 0) {
        (void)close(runtime->server->config.supervision.control_fd);
        runtime->server->config.supervision.control_fd = -1;
    }
    process->process_id = (long)getpid();
    process->role = child->id == KORE_WORKER_KEYMGR
                        ? ROTMOS_PROCESS_KEY_MANAGER
                        : (child->id == KORE_WORKER_ACME ? ROTMOS_PROCESS_ACME
                                                         : ROTMOS_PROCESS_HTTP);
    process->worker_index =
        process->role == ROTMOS_PROCESS_HTTP ? child->id - 1U : 0;
    process->restarted = child->restarted;
    runtime->worker_started = runtime->worker_stopped = 0;
    runtime->stop_sent = 0;
    runtime->server->stop_requested = runtime->server->terminating_signal = 0;
    if (!wake_open(runtime))
        return 0;
    if (rotmos_kore_loop_create(runtime, &runtime->error) != ROTMOS_OK)
        return 0;
    return rotmos_runtime_hook(
               runtime, runtime->server->config.callbacks.on_process_start) ==
           ROTMOS_OK;
}

static int worker_start(void *userdata)
{
    struct rotmos_runtime *runtime;
    runtime = userdata;
    kore_platform_event_level_read(runtime->wake[0], &runtime->wake_event);
    if (rotmos_loop_activate(runtime->server->loop, &runtime->error) !=
        ROTMOS_OK)
        return 0;
    runtime->worker_started = 1;
    return rotmos_runtime_hook(
               runtime, runtime->server->config.callbacks.on_worker_start) ==
           ROTMOS_OK;
}

static void worker_stopping(void *userdata)
{
    struct rotmos_runtime *runtime;
    runtime = userdata;
    if (runtime->worker_started && !runtime->worker_stopped) {
        runtime->worker_stopped = 1;
        runtime->server->state = ROTMOS_SERVER_STOPPING;
        (void)rotmos_runtime_hook(
            runtime, runtime->server->config.callbacks.on_worker_stopping);
        rotmos_kore_channels_stopping(runtime);
    }
}

static int startup_cancel(void *userdata)
{
    struct rotmos_runtime *runtime;
    runtime = userdata;
    return runtime->server->stop_requested ||
           runtime->server->terminating_signal;
}

static void fork_prepare(void *userdata)
{
    (void)userdata;
    (void)pthread_mutex_lock(&runtime_lock);
}

static void fork_complete(void *userdata)
{
    (void)userdata;
    (void)pthread_mutex_unlock(&runtime_lock);
}

static int worker_exit(void *userdata)
{
    struct rotmos_runtime *runtime;
    int status;
    runtime = userdata;
    rotmos_kore_loop_finish(runtime);
    wake_close(runtime);
    status = 0;
    if (runtime->server->config.callbacks.on_worker_exit != NULL)
        status = runtime->server->config.callbacks.on_worker_exit(
            runtime->server, &runtime->server->process,
            runtime->server->config.callbacks.userdata);
    return status;
}

static int worker_pending(void *userdata)
{
    struct rotmos_runtime *runtime;
    runtime = userdata;
    return rotmos_loop_pending(runtime->server->loop);
}

static rotmos_status validate_config(rotmos_server *server, rotmos_error *error)
{
    size_t i;
    rotmos_status status;
    rotmos_u64 limit;
    rotmos_body_policy policy;
    rotmos_body *body;
    const rotmos_route_config *route;
    if (server == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "server_validate",
                           "server is required");
    if (rotmos_server_running(server))
        return rotmos_fail(error, ROTMOS_STATE, "server_validate",
                           "server is running");
    if (server->config.listener_count == 0)
        return rotmos_fail(error, ROTMOS_INVALID, "server_validate",
                           "declare at least one listener");
    if (server->config.supervision.enabled &&
        server->config.supervision.control_fd < 0)
        return rotmos_fail(
            error, ROTMOS_STATE, "server_validate",
            "supervised rerun requires a fresh control descriptor");
    if (server->config.callbacks.on_worker_event != NULL ||
        server->config.autoblock.enabled ||
        server->config.client_ip.trusted_proxy_count != 0 ||
        server->config.http.access_log_path != NULL)
        return rotmos_fail(error, ROTMOS_NOT_SUPPORTED, "server_validate",
                           "Worker observations, access logs and admission "
                           "policy are unavailable in the current runtime");
    for (i = 0; i < server->config.tls.identity_count; ++i)
        if (server->config.tls.identities[i].mode != ROTMOS_TLS_MANUAL)
            return rotmos_fail(error, ROTMOS_NOT_SUPPORTED, "server_validate",
                               "ACME identity activation is not connected yet");
    for (i = 0; i < server->router->count; ++i) {
        route = &server->router->routes[i]->config;
        if (route->on_headers != NULL)
            continue;
        if (route->body.mode != ROTMOS_BODY_NONE &&
            route->body.mode != ROTMOS_BODY_BUFFERED &&
            route->body.mode != ROTMOS_BODY_MATERIALIZED)
            return rotmos_fail(error, ROTMOS_NOT_SUPPORTED, "server_validate",
                               "JSON/upload facades are not connected; STREAM "
                               "requires an on_headers route");
        if (route->body.mode == ROTMOS_BODY_NONE)
            continue;
        limit = route->body.max_bytes;
        if (limit.high == 0 && limit.low == 0)
            limit = server->config.limits.default_body_bytes;
        policy = route->body;
        policy.max_bytes = limit;
        status = rotmos_body_create(
            &policy, server->config.limits.body_spool_directory, &body, error);
        if (status != ROTMOS_OK)
            return status;
        rotmos_body_release(body);
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_server_validate(rotmos_server *server, rotmos_error *error)
{
    struct rotmos_tls_prepared tls;
    rotmos_status status;
    status = validate_config(server, error);
    if (status != ROTMOS_OK)
        return status;
    status = rotmos_tls_prepare(&server->config.tls, KORE_DEFAULT_CIPHER_LIST,
                                &tls, error);
    rotmos_tls_prepared_clear(&tls);
    return status;
}

static void configure(struct rotmos_runtime *runtime)
{
    const rotmos_server_config *config;
    size_t ceiling;
    config = &runtime->server->config;
    worker_count = (u_int8_t)config->workers.count;
    worker_set_affinity = !config->workers.affinity_disabled;
    worker_accept_threshold = (u_int32_t)config->workers.accept_threshold;
    worker_max_connections = (u_int32_t)config->limits.max_connections;
    worker_rlimit_nofiles = (u_int32_t)config->workers.rlimit_nofiles;
    worker_shutdown_timeout_ms = (u_int32_t)config->workers.shutdown_timeout_ms;
    kore_library_startup_timeout_ms =
        (u_int32_t)config->timeouts.startup.milliseconds;
    worker_policy = config->workers.death_policy == ROTMOS_WORKER_RESTART
                        ? KORE_WORKER_POLICY_RESTART
                        : KORE_WORKER_POLICY_TERMINATE;
    worker_idle_timeout =
        (u_int64_t)config->timeouts.connection_idle.milliseconds;
    http_header_max = config->limits.max_request_header_bytes;
    http_header_timeout =
        (u_int16_t)(config->timeouts.request_header.milliseconds / 1000);
    http_body_timeout =
        (u_int16_t)(config->timeouts.request_body_idle.milliseconds / 1000);
    http_response_write_timeout =
        (u_int32_t)config->timeouts.response_write_idle.milliseconds;
    http_body_min_rate =
        ((u_int64_t)config->limits.body_min_rate_bytes_per_second.high << 32) |
        config->limits.body_min_rate_bytes_per_second.low;
    http_body_min_rate_grace =
        (u_int64_t)config->timeouts.body_min_rate_grace.milliseconds;
    http_request_limit = (u_int32_t)config->limits.request_limit;
    http_request_ms = config->http.request_process_budget_ms;
    http_keepalive_time =
        config->http.keepalive_disabled
            ? 0
            : (u_int16_t)(config->timeouts.keepalive.milliseconds / 1000);
    http_keepalive_max_requests = config->http.keepalive_disabled
                                      ? 0
                                      : config->http.keepalive_max_requests;
    ceiling = (size_t)-1;
    if (config->limits.hard_body_bytes.high != 0 ||
        config->limits.hard_body_bytes.low != 0)
        (void)rotmos_u64_to_size(config->limits.hard_body_bytes, &ceiling,
                                 NULL);
    http_body_max = ceiling;
    http_body_disk_offload = 0;
    http_hsts_enable = config->http.hsts_max_age_seconds;
    http_pretty_error = config->http.pretty_error_pages;
    kore_quiet = config->http.backend_quiet;
    kore_socket_backlog = config->http.socket_backlog;
    kore_websocket_maxframe = config->limits.websocket_max_frame_bytes;
    kore_websocket_timeout =
        (u_int64_t)config->timeouts.websocket_idle.milliseconds;
    http_server_version(config->http.server_header);
    runtime->curl_timeout = kore_curl_timeout;
    runtime->curl_recv_max = kore_curl_recv_max;
    if (config->http.backend_curl_timeout_seconds != 0)
        kore_curl_timeout =
            (u_int16_t)config->http.backend_curl_timeout_seconds;
    if (config->http.backend_curl_recv_max_bytes.high != 0 ||
        config->http.backend_curl_recv_max_bytes.low != 0)
        kore_curl_recv_max =
            ((u_int64_t)config->http.backend_curl_recv_max_bytes.high << 32) |
            config->http.backend_curl_recv_max_bytes.low;
}

static void bind_listeners(struct rotmos_runtime *runtime)
{
    const rotmos_listener_config *config;
    struct kore_server *server;
    struct listener *listener;
    struct sockaddr_storage address;
    struct sockaddr_in *ipv4;
    struct sockaddr_in6 *ipv6;
    socklen_t size;
    char name[32];
    char port[8];
    size_t i;
    size_t count;
    count = runtime->server->config.listener_count;
    runtime->bound = calloc(count, sizeof(*runtime->bound));
    runtime->addresses = calloc(count, sizeof(*runtime->addresses));
    runtime->native_servers = calloc(count, sizeof(*runtime->native_servers));
    if (runtime->bound == NULL || runtime->addresses == NULL ||
        runtime->native_servers == NULL)
        fatal("cannot allocate bound listener registry");
    for (i = 0; i < count; ++i) {
        config = &runtime->server->config.listeners[i];
        (void)snprintf(name, sizeof(name), "rotmos-%lu", (unsigned long)i);
        (void)snprintf(port, sizeof(port), "%u", (unsigned int)config->port);
        server = kore_server_create(name);
        runtime->native_servers[i] = server;
        server->tls = config->kind == ROTMOS_LISTENER_HTTPS;
        if (!kore_server_bind(server, config->bind, port, NULL))
            fatal("cannot bind listener %s", name);
        listener = LIST_FIRST(&server->listeners);
        size = sizeof(address);
        if (getsockname(listener->fd, (struct sockaddr *)&address, &size) != 0)
            fatal("cannot inspect bound listener %s", name);
        if (address.ss_family == AF_INET) {
            ipv4 = (struct sockaddr_in *)&address;
            (void)inet_ntop(AF_INET, &ipv4->sin_addr, runtime->addresses[i],
                            46);
            runtime->bound[i].port = ntohs(ipv4->sin_port);
        } else {
            ipv6 = (struct sockaddr_in6 *)&address;
            (void)inet_ntop(AF_INET6, &ipv6->sin6_addr, runtime->addresses[i],
                            46);
            runtime->bound[i].port = ntohs(ipv6->sin6_port);
        }
        runtime->bound[i].name = config->name;
        runtime->bound[i].address = rotmos_view(runtime->addresses[i]);
        ++runtime->bound_count;
        rotmos_kore_listener_domains(runtime, server, config);
    }
}

rotmos_status rotmos_server_listener_at(const rotmos_server *server,
                                        size_t index,
                                        rotmos_bound_listener *out,
                                        rotmos_error *error)
{
    if (server == NULL || out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "server_listener_at",
                           "server and output are required");
    if (server->runtime == NULL)
        return rotmos_fail(error, ROTMOS_STATE, "server_listener_at",
                           "listener addresses belong to the active run");
    if (index >= server->runtime->bound_count)
        return rotmos_fail(error, ROTMOS_INVALID, "server_listener_at",
                           "listener index is outside the bound registry");
    *out = server->runtime->bound[index];
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_server_set_serving(rotmos_server *server, int serving,
                                        rotmos_error *error)
{
    struct rotmos_runtime *runtime;
    if (server == NULL || server->runtime == NULL)
        return rotmos_fail(error, ROTMOS_STATE, "server_set_serving",
                           "server has no active run");
    runtime = server->runtime;
    if (serving && (server->stop_requested || server->terminating_signal))
        return rotmos_fail(error, ROTMOS_STATE, "server_set_serving",
                           "a stop has already been requested");
    if (getpid() != runtime->parent_pid ||
        !pthread_equal(pthread_self(), runtime->owner_thread) ||
        (server->state != ROTMOS_SERVER_SERVING &&
         server->state != ROTMOS_SERVER_READY_PAUSED))
        return rotmos_fail(error, ROTMOS_STATE, "server_set_serving",
                           "serving gate is a ready parent-loop operation");
    __atomic_store_n(&runtime->shared->serving, serving != 0, __ATOMIC_RELEASE);
    runtime->gate_selected = 1;
    server->state =
        serving ? ROTMOS_SERVER_SERVING : ROTMOS_SERVER_READY_PAUSED;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_server_run(rotmos_server *server, rotmos_run_result *out,
                                rotmos_error *error)
{
    struct rotmos_runtime *runtime;
    struct kore_library_hooks hooks = {0};
    struct sigaction action;
    rotmos_status status;
    rotmos_status stopped_status;
    if (out != NULL) {
        out->reason = ROTMOS_RUN_STARTUP_FAILURE;
        out->signal_number = out->escalated = 0;
    }
    status = validate_config(server, error);
    if (status != ROTMOS_OK)
        return status;
    runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "server_run",
                           "cannot allocate runtime");
    runtime->server = server;
    runtime->parent_pid = getpid();
    runtime->owner_thread = pthread_self();
    runtime->wake[0] = runtime->wake[1] = -1;
    runtime->shared =
        mmap(NULL, sizeof(*runtime->shared), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (runtime->shared == MAP_FAILED) {
        free(runtime);
        return rotmos_fail(error, ROTMOS_IO, "server_run",
                           "cannot allocate shared serving gate");
    }
    (void)pthread_mutex_lock(&runtime_lock);
    if (active_runtime != NULL) {
        (void)pthread_mutex_unlock(&runtime_lock);
        (void)munmap(runtime->shared, sizeof(*runtime->shared));
        free(runtime);
        return rotmos_fail(error, ROTMOS_CONFLICT, "server_run",
                           "Kore supports one active server per process");
    }
    active_runtime = runtime;
    server->runtime = runtime;
    server->stop_requested = server->terminating_signal = 0;
    server->state = ROTMOS_SERVER_STARTING;
    server->router->frozen = 1;
    server->process.role = ROTMOS_PROCESS_PARENT;
    server->process.process_id = (long)runtime->parent_pid;
    (void)pthread_mutex_unlock(&runtime_lock);
    if (setjmp(runtime->unwind) == 0) {
        if (!wake_open(runtime))
            fatal_error(runtime, "cannot allocate runtime notification pipe");
        if (server->config.signals.mode == ROTMOS_SIGNALS_LIBRARY) {
            memset(&action, 0, sizeof(action));
            action.sa_handler = signal_received;
            (void)sigemptyset(&action.sa_mask);
            while (runtime->installed_signals <
                   sizeof(handled_signals) / sizeof(handled_signals[0])) {
                if (sigaction(
                        handled_signals[runtime->installed_signals], &action,
                        &runtime->saved_signals[runtime->installed_signals]) !=
                    0)
                    fatal_error(runtime, "cannot install run signal handlers");
                ++runtime->installed_signals;
            }
        }
        runtime->status = rotmos_kore_loop_create(runtime, &runtime->error);
        if (runtime->status == ROTMOS_OK)
            runtime->status =
                rotmos_kore_control_start(runtime, &runtime->error);
        if (runtime->status == ROTMOS_OK)
            runtime->status = rotmos_runtime_hook(
                runtime, server->config.callbacks.on_process_start);
        if (runtime->status == ROTMOS_OK)
            runtime->status = rotmos_runtime_hook(
                runtime, server->config.callbacks.on_parent_prepare);
        runtime->cancelled = startup_cancel(runtime);
        if (runtime->status == ROTMOS_OK && !runtime->cancelled)
            runtime->status = rotmos_tls_prepare(
                &server->config.tls, KORE_DEFAULT_CIPHER_LIST, &runtime->tls,
                &runtime->error);
        if (runtime->status == ROTMOS_OK && !runtime->cancelled) {
            hooks.arg = runtime;
            hooks.fatal = fatal_error;
            hooks.startup_cancel = startup_cancel;
            hooks.fork_prepare = fork_prepare;
            hooks.fork_complete = fork_complete;
            hooks.process_start = child_start;
            hooks.worker_start = worker_start;
            hooks.worker_stopping = worker_stopping;
            hooks.worker_exit = worker_exit;
            hooks.worker_pending = worker_pending;
            hooks.loop_wait = rotmos_kore_loop_wait;
            hooks.loop_turn = rotmos_kore_loop_turn;
            hooks.parent_drain = rotmos_kore_parent_drain;
            hooks.parent_cleanup = rotmos_kore_loop_shutdown;
            hooks.worker_cleanup = rotmos_kore_loop_shutdown;
            hooks.request_release = rotmos_kore_response_release;
            hooks.net_progress = rotmos_kore_response_progress;
            runtime->initialized = 1;
            runtime->curl_timeout = kore_curl_timeout;
            runtime->curl_recv_max = kore_curl_recv_max;
            kore_library_init(&hooks);
            configure(runtime);
            runtime->callbacks.http_request = rotmos_kore_dispatch;
            runtime->callbacks.http_request_free = rotmos_kore_request_free;
            bind_listeners(runtime);
            rotmos_tls_prepared_clear(&runtime->tls);
            if (!kore_library_parent_start()) {
                runtime->cancelled = startup_cancel(runtime);
                if (!runtime->cancelled)
                    runtime->status = rotmos_fail(
                        &runtime->error, ROTMOS_BACKEND, "server_start",
                        "required worker did not initialize");
            }
        }
        if (runtime->status == ROTMOS_OK && !runtime->cancelled) {
            kore_platform_event_level_read(runtime->wake[0],
                                           &runtime->wake_event);
            runtime->status =
                rotmos_loop_activate(server->loop, &runtime->error);
        }
        if (runtime->status == ROTMOS_OK && !runtime->cancelled) {
            server->state = ROTMOS_SERVER_READY_PAUSED;
            runtime->status = rotmos_runtime_hook(
                runtime, server->config.callbacks.on_parent_ready);
            if (runtime->status == ROTMOS_OK) {
                runtime->ready = 1;
                if (!runtime->gate_selected)
                    (void)rotmos_server_set_serving(
                        server, !server->config.start_paused, NULL);
            }
        }
        while (runtime->status == ROTMOS_OK && runtime->ready &&
               kore_quit == KORE_QUIT_NONE) {
            if (server->stop_requested || server->terminating_signal) {
                kore_quit = KORE_QUIT_NORMAL;
                break;
            }
            kore_library_parent_step(1000);
        }
    }
    server->state = ROTMOS_SERVER_STOPPING;
    __atomic_store_n(&runtime->shared->serving, 0, __ATOMIC_RELEASE);
    stopped_status = rotmos_runtime_hook(
        runtime, server->config.callbacks.on_parent_stopping);
    if (runtime->status == ROTMOS_OK && stopped_status != ROTMOS_OK)
        runtime->status = stopped_status;
    if (runtime->initialized) {
        runtime->cleaning = 1;
        kore_library_cleanup();
        kore_curl_timeout = runtime->curl_timeout;
        kore_curl_recv_max = runtime->curl_recv_max;
    } else
        rotmos_kore_loop_shutdown(runtime);
    rotmos_kore_loop_finish(runtime);
    rotmos_tls_prepared_clear(&runtime->tls);
    while (runtime->installed_signals != 0) {
        --runtime->installed_signals;
        (void)sigaction(handled_signals[runtime->installed_signals],
                        &runtime->saved_signals[runtime->installed_signals],
                        NULL);
    }
    if (runtime->ready && kore_quit == KORE_QUIT_FATAL &&
        runtime->status == ROTMOS_OK)
        runtime->status = rotmos_fail(&runtime->error, ROTMOS_BACKEND,
                                      "server_run", "backend or worker failed");
    stopped_status = rotmos_runtime_hook(
        runtime, server->config.callbacks.on_parent_stopped);
    if (runtime->status == ROTMOS_OK && stopped_status != ROTMOS_OK)
        runtime->status = stopped_status;
    rotmos_kore_control_close(runtime);
    if (out != NULL) {
        out->reason = runtime->status != ROTMOS_OK
                          ? (runtime->ready ? ROTMOS_RUN_BACKEND_FAILURE
                                            : ROTMOS_RUN_STARTUP_FAILURE)
                          : (server->terminating_signal != 0
                                 ? ROTMOS_RUN_TERMINATING_SIGNAL
                                 : ROTMOS_RUN_REQUESTED_STOP);
        out->signal_number = (int)server->terminating_signal;
        out->escalated = runtime->initialized && kore_library_escalated;
    }
    status = runtime->status;
    if (error != NULL)
        *error = runtime->error;
    (void)pthread_mutex_lock(&runtime_lock);
    wake_close(runtime);
    server->runtime = NULL;
    active_runtime = NULL;
    server->state =
        status == ROTMOS_OK ? ROTMOS_SERVER_STOPPED : ROTMOS_SERVER_FAILED;
    server->router->frozen = 0;
    (void)pthread_mutex_unlock(&runtime_lock);
    (void)munmap(runtime->shared, sizeof(*runtime->shared));
    free(runtime->native_servers);
    free(runtime->bound);
    free(runtime->addresses);
    free(runtime);
    if (status == ROTMOS_OK)
        rotmos_error_clear(error);
    return status;
}
