#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "executor_internal.h"
#include "storage_internal.h"
#include "body_internal.h"
#include "autoblock_internal.h"
#include "logging_internal.h"
#include "access_log_file.h"
#include "file_internal.h"

#include <sys/mman.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t runtime_lock = PTHREAD_MUTEX_INITIALIZER;
static struct knurl_runtime *active_runtime;
static const int handled_signals[] = {SIGINT, SIGTERM, SIGQUIT, SIGCHLD};

knurl_status knurl_runtime_hook(struct knurl_runtime *runtime,
                                knurl_process_fn function)
{
    knurl_status status;
    knurl_error error;
    if (function == NULL)
        return KNURL_OK;
    knurl_error_clear(&error);
    status = function(runtime->server, &runtime->server->process,
                      runtime->server->config.callbacks.userdata, &error);
    if (status < KNURL_OK || status == KNURL_AGAIN || status == KNURL_EOF ||
        status > KNURL_BACKEND)
        status = KNURL_INVALID;
    if (status != KNURL_OK && error.status != status)
        (void)knurl_fail(&error, status, "server_hook",
                         "server lifecycle callback failed");
    if (status != KNURL_OK && runtime->status == KNURL_OK)
        runtime->error = error;
    if (status != KNURL_OK && getpid() != runtime->parent_pid) {
        runtime->status = status;
        knurl_kore_child_failure(runtime, &error);
    }
    return status;
}

static void fatal_error(void *userdata, const char *message)
{
    struct knurl_runtime *runtime;
    runtime = userdata;
    if (getpid() != runtime->parent_pid) {
        if (runtime->error.status == KNURL_OK) {
            (void)knurl_fail(&runtime->error, KNURL_BACKEND, "kore_worker",
                             message);
            runtime->error.source = KNURL_ERROR_BACKEND;
        }
        knurl_kore_child_failure(runtime, &runtime->error);
        _exit(70);
    }
    runtime->status =
        knurl_fail(&runtime->error, KNURL_BACKEND, "kore_runtime", message);
    runtime->error.source = KNURL_ERROR_BACKEND;
    longjmp(runtime->unwind, 1);
}

static void signal_received(int number)
{
    if (active_runtime != NULL)
        knurl_server_notify_signal(active_runtime->server, number);
}

static void worker_signal_received(int number)
{
    /* Keep Kore's signal priority/meaning, and wake the selected event backend
     * even when delivery precedes its blocking wait. A zero notification is
     * only a wakeup; it does not turn a worker signal into a host stop request.
     */
    kore_signal(number);
    if (active_runtime != NULL)
        knurl_server_notify_signal(active_runtime->server, 0);
}

static knurl_status worker_signals(struct knurl_runtime *runtime)
{
    static const int signals[] = {SIGHUP,  SIGQUIT, SIGTERM,
                                  SIGUSR1, SIGCHLD, SIGINT};
    struct sigaction action;
    size_t i;
    int native_code;
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        if (sigaction(signals[i], NULL, &action) == -1)
            break;
        if (action.sa_handler != kore_signal)
            continue;
        action.sa_handler = worker_signal_received;
        if (sigaction(signals[i], &action, NULL) == -1)
            break;
    }
    if (i == sizeof(signals) / sizeof(signals[0]))
        return KNURL_OK;
    native_code = errno;
    (void)knurl_fail(&runtime->error, KNURL_IO, "worker_signals",
                     "cannot install worker signal wakeup");
    runtime->error.source = KNURL_ERROR_SYSTEM;
    runtime->error.native_code = native_code;
    return KNURL_IO;
}

void knurl_server_notify_signal(knurl_server *server, int number)
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

knurl_status knurl_server_request_stop(knurl_server *server, knurl_error *error)
{
    unsigned char wake;
    knurl_status status;
    ssize_t count;
    wake = 1;
    status = KNURL_OK;
    (void)pthread_mutex_lock(&runtime_lock);
    if (server == NULL || server->runtime == NULL || server->notify_fd < 0 ||
        server->process.process_id != (long)getpid()) {
        status = knurl_fail(error, KNURL_STATE, "server_request_stop",
                            "server has no active run");
    } else if (!server->stop_requested) {
        server->stop_requested = 1;
        do {
            count = write((int)server->notify_fd, &wake, 1);
        } while (count == -1 && errno == EINTR);
        if (count == -1 && errno != EAGAIN)
            status = knurl_fail(error, KNURL_IO, "server_request_stop",
                                "cannot wake runtime loop");
    }
    (void)pthread_mutex_unlock(&runtime_lock);
    if (status == KNURL_OK)
        knurl_error_clear(error);
    return status;
}

static void wake_event(void *userdata, int failed)
{
    struct knurl_runtime *runtime;
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

static int wake_open(struct knurl_runtime *runtime)
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

static void wake_close(struct knurl_runtime *runtime)
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
    struct knurl_runtime *runtime;
    knurl_process_context *process;
    runtime = userdata;
    process = &runtime->server->process;
    knurl_kore_logging_child(runtime);
    wake_close(runtime);
    kore_platform_event_cleanup();
    kore_timer_cleanup();
    knurl_loop_discard_after_fork(runtime->server->loop);
    runtime->server->loop = NULL;
    runtime->control_watch = NULL;
    if (runtime->server->config.supervision.enabled &&
        runtime->server->config.supervision.control_fd >= 0) {
        (void)close(runtime->server->config.supervision.control_fd);
        runtime->server->config.supervision.control_fd = -1;
    }
    process->process_id = (long)getpid();
    process->role = child->id == KORE_WORKER_KEYMGR
                        ? KNURL_PROCESS_KEY_MANAGER
                        : (child->id == KORE_WORKER_ACME ? KNURL_PROCESS_ACME
                                                         : KNURL_PROCESS_HTTP);
    process->worker_index =
        process->role == KNURL_PROCESS_HTTP ? child->id - 1U : 0;
    process->restarted = child->restarted;
    runtime->worker_started = runtime->worker_stopped = 0;
    runtime->stop_sent = 0;
    runtime->server->stop_requested = runtime->server->terminating_signal = 0;
    if (!wake_open(runtime))
        return 0;
    if (knurl_kore_loop_create(runtime, &runtime->error) != KNURL_OK)
        return 0;
    runtime->worker_started = 1;
    return knurl_runtime_hook(
               runtime, runtime->server->config.callbacks.on_process_start) ==
           KNURL_OK;
}

static int worker_start(void *userdata)
{
    struct knurl_runtime *runtime;
    runtime = userdata;
    kore_platform_event_level_read(runtime->wake[0], &runtime->wake_event);
    runtime->status = worker_signals(runtime);
    if (runtime->status == KNURL_OK)
        runtime->status =
            knurl_loop_activate(runtime->server->loop, &runtime->error);
    if (runtime->status != KNURL_OK) {
        knurl_kore_child_failure(runtime, &runtime->error);
        return 0;
    }
    runtime->worker_started = 1;
    if (runtime->server->process.role != KNURL_PROCESS_HTTP)
        return 1;
    runtime->server->loop->http_worker = 1;
    knurl_kore_tls_worker(runtime, runtime->server->process.worker_index, 1);
    return knurl_runtime_hook(
               runtime, runtime->server->config.callbacks.on_worker_start) ==
           KNURL_OK;
}

static void worker_stopping(void *userdata)
{
    struct knurl_runtime *runtime;
    runtime = userdata;
    if (runtime->worker_started && !runtime->worker_stopped) {
        runtime->worker_stopped = 1;
        runtime->server->state = KNURL_SERVER_STOPPING;
        (void)knurl_runtime_hook(
            runtime, runtime->server->config.callbacks.on_worker_stopping);
        knurl_kore_channels_stopping(runtime);
        knurl_kore_websockets_stopping(runtime);
        knurl_storage_stopping(runtime->server->loop);
        knurl_executors_stopping(runtime->server->loop);
    }
}

static int startup_cancel(void *userdata)
{
    struct knurl_runtime *runtime;
    size_t i;
    runtime = userdata;
    if (!runtime->ready && runtime->status == KNURL_OK) {
        for (i = 0; i < KORE_WORKER_MAX; ++i)
            if (__atomic_load_n(&runtime->shared->failures[i].valid,
                                __ATOMIC_ACQUIRE)) {
                runtime->error = runtime->shared->failures[i].error;
                runtime->status = runtime->error.status;
                break;
            }
    }
    return runtime->status != KNURL_OK || runtime->server->stop_requested ||
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
    struct knurl_runtime *runtime;
    struct kore_worker *child;
    knurl_server *server;
    knurl_process_context process;
    knurl_worker_exit_fn callback;
    void *data;
    int status, failed;
    runtime = userdata;
    knurl_kore_loop_finish(runtime);
    wake_close(runtime);
    knurl_kore_acme_clear(runtime);
    knurl_tls_prepared_clear(&runtime->tls);
    knurl_kore_logging_finish(runtime);
    failed = runtime->status != KNURL_OK;
    if (runtime->status != KNURL_OK)
        knurl_kore_child_failure(runtime, &runtime->error);
    child = worker;
    server = runtime->server;
    process = server->process;
    callback = server->config.callbacks.on_worker_exit;
    data = server->config.callbacks.userdata;
    /* Vectis's terminal fork callback may reclaim its app and this fork's
     * server. Publish native cleanup first and retain no host state afterward.
     */
    (void)pthread_mutex_lock(&runtime_lock);
    server->state = failed ? KNURL_SERVER_FAILED : KNURL_SERVER_STOPPED;
    server->runtime = NULL;
    server->router->frozen = 0;
    active_runtime = NULL;
    runtime->server = NULL;
    (void)pthread_mutex_unlock(&runtime_lock);
    status = callback != NULL ? callback(server, &process, data) : 0;
    if (child != NULL)
        __atomic_store_n(&child->library_clean, 1, __ATOMIC_RELEASE);
    return status != 0 ? status : failed ? 70 : 0;
}

static int worker_pending(void *userdata)
{
    struct knurl_runtime *runtime;
    runtime = userdata;
    return runtime->websockets != NULL ||
           knurl_loop_pending(runtime->server->loop);
}

static knurl_status validate_config(knurl_server *server, int *spool,
                                    knurl_error *error)
{
    size_t i;
    knurl_status status;
    knurl_u64 limit;
    knurl_body_policy policy;
    knurl_body *body;
    const knurl_route_config *route;
    if (server == NULL)
        return knurl_fail(error, KNURL_INVALID, "server_validate",
                          "server is required");
    if (knurl_server_running(server))
        return knurl_fail(error, KNURL_STATE, "server_validate",
                          "server is running");
    if (server->config.listener_count == 0)
        return knurl_fail(error, KNURL_INVALID, "server_validate",
                          "declare at least one listener");
    if (server->config.supervision.enabled &&
        server->config.supervision.control_fd < 0)
        return knurl_fail(
            error, KNURL_STATE, "server_validate",
            "supervised rerun requires a fresh control descriptor");
    for (i = 0; i < server->router->count; ++i) {
        route = &server->router->routes[i]->config;
        if (route->on_headers != NULL)
            continue;
        if (route->body.mode != KNURL_BODY_NONE &&
            route->body.mode != KNURL_BODY_JSON &&
            route->body.mode != KNURL_BODY_STREAMING_UPLOAD &&
            route->body.mode != KNURL_BODY_BUFFERED &&
            route->body.mode != KNURL_BODY_MATERIALIZED)
            return knurl_fail(error, KNURL_NOT_SUPPORTED, "server_validate",
                              "uploads require upload registration; STREAM "
                              "requires an on_headers route");
        if (route->body.mode == KNURL_BODY_NONE)
            continue;
        limit = knurl_body_policy_limit(
            &route->body, server->config.limits.default_body_bytes);
        policy = route->body;
        policy.max_bytes = limit;
        status = knurl_body_create(
            &policy, server->config.limits.body_spool_directory, &body, error);
        if (status != KNURL_OK)
            return status;
        if (spool != NULL && !policy.disk_spool_disabled) {
            size_t size;
            if (knurl_u64_to_size(limit, &size, NULL) != KNURL_OK ||
                size > body->memory_limit)
                *spool = 1;
        }
        knurl_body_release(body);
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status validate_material(knurl_server *server, knurl_error *error)
{
    struct knurl_tls_prepared tls;
    struct knurl_runtime preparation;
    knurl_status status;
    status = knurl_tls_prepare(&server->config.tls, KORE_DEFAULT_CIPHER_LIST,
                               &tls, error);
    knurl_tls_prepared_clear(&tls);
    if (status == KNURL_OK) {
        memset(&preparation, 0, sizeof(preparation));
        preparation.server = server;
        status = knurl_kore_acme_prepare(&preparation, error);
        knurl_kore_acme_clear(&preparation);
    }
    return status;
}

knurl_status knurl_server_validate(knurl_server *server, knurl_error *error)
{
    knurl_status status = validate_config(server, NULL, error);
    return status == KNURL_OK ? validate_material(server, error) : status;
}

static knurl_status resource_error(knurl_error *error, knurl_status status,
                                   const char *operation, const char *message,
                                   knurl_error_source source, int code)
{
    knurl_fail(error, status, operation, message);
    if (error != NULL) {
        error->source = source;
        error->native_code = code;
    }
    return status;
}

static knurl_status listener_error(const knurl_listener_config *config,
                                   const char *operation,
                                   knurl_error_source source, int code,
                                   knurl_error *error)
{
    resource_error(error, KNURL_BACKEND, operation,
                   source == KNURL_ERROR_SYSTEM ? strerror(code)
                                                : gai_strerror(code),
                   source, code);
    if (error != NULL)
        (void)snprintf(error->detail, sizeof(error->detail), "port %u at %s",
                       (unsigned int)config->port, config->bind);
    return KNURL_BACKEND;
}

/* Open the same TCP listener for preflight and for adoption by Kore at run. */
static knurl_status open_listener(const knurl_listener_config *config,
                                  unsigned int backlog, int *out, int *family,
                                  knurl_error *error)
{
    struct addrinfo hints, *address;
    const char *operation;
    char port[8];
    int fd, code, enabled = 1;

    *out = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    (void)snprintf(port, sizeof(port), "%u", (unsigned int)config->port);
    code = getaddrinfo(config->bind, port, &hints, &address);
    if (code != 0)
        return listener_error(config, "listener_resolve",
                              code == EAI_SYSTEM ? KNURL_ERROR_SYSTEM
                                                 : KNURL_ERROR_BACKEND,
                              code == EAI_SYSTEM ? errno : code, error);

    operation = "listener_socket";
    fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd == -1)
        goto failed;
    operation = "listener_configure";
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1 ||
        !kore_connection_nonblock(fd, 0) ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) ==
            -1)
        goto failed;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    operation = "listener_bind";
    if (bind(fd, address->ai_addr, address->ai_addrlen) == -1)
        goto failed;
    operation = "listener_listen";
    if (listen(fd, (int)backlog) == -1)
        goto failed;
    if (family != NULL)
        *family = address->ai_family;
    freeaddrinfo(address);
    *out = fd;
    return KNURL_OK;

failed:
    code = errno;
    if (fd != -1)
        (void)close(fd);
    freeaddrinfo(address);
    return listener_error(config, operation, KNURL_ERROR_SYSTEM, code, error);
}

static knurl_status probe_spool(const char *directory, knurl_error *error)
{
    knurl_status status;
    int fd, code;
    char *path;

    status = knurl_private_file(directory, "knurl-body", &fd, &path, error);
    if (status == KNURL_OK) {
        status = knurl_file_write(fd, "\0", 1, error);
        code = unlink(path) == -1 ? errno : 0;
        if (close(fd) == -1 && code == 0)
            code = errno;
        free(path);
        if (status == KNURL_OK && code != 0)
            status = resource_error(error, KNURL_IO, "body_spool",
                                    strerror(code), KNURL_ERROR_SYSTEM, code);
    }
    if (status != KNURL_OK && error != NULL) {
        (void)snprintf(error->operation, sizeof(error->operation),
                       "body_spool");
        (void)snprintf(error->detail, sizeof(error->detail), "%s", directory);
    }
    return status;
}

knurl_status knurl_server_preflight(knurl_server *server, knurl_error *error)
{
    const knurl_server_config *config;
    struct knurl_access_file *log;
    knurl_status status;
    int spool = 0, code, *listeners;
    size_t opened = 0;

    status = validate_config(server, &spool, error);
    if (status == KNURL_OK)
        status = validate_material(server, error);
    if (status != KNURL_OK)
        return status;
    config = &server->config;
    listeners = calloc(config->listener_count, sizeof(*listeners));
    if (listeners == NULL)
        return knurl_fail(error, KNURL_NOMEM, "server_preflight",
                          "cannot allocate listener probes");
    /* Hold all sockets until the last bind to catch conflicting declarations.
     */
    while (opened < config->listener_count && status == KNURL_OK) {
        status = open_listener(&config->listeners[opened],
                               config->http.socket_backlog, &listeners[opened],
                               NULL, error);
        if (status == KNURL_OK)
            ++opened;
    }
    while (opened != 0)
        (void)close(listeners[--opened]);
    free(listeners);
    if (status == KNURL_OK && spool &&
        config->limits.body_spool_directory != NULL)
        status = probe_spool(config->limits.body_spool_directory, error);
    if (status == KNURL_OK && config->http.access_log_path != NULL) {
        status = knurl_access_file_open(&config->http, &log, error);
        if (status == KNURL_OK) {
            code = knurl_access_file_close(log);
            if (code != 0)
                status =
                    resource_error(error, KNURL_IO, "access_log_close",
                                   strerror(code), KNURL_ERROR_SYSTEM, code);
        }
        if (status != KNURL_OK && error != NULL)
            (void)snprintf(error->detail, sizeof(error->detail), "%s",
                           config->http.access_log_path);
    }
    return status;
}

static void configure(struct knurl_runtime *runtime)
{
    const knurl_server_config *config;
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
    worker_policy = config->workers.death_policy == KNURL_WORKER_RESTART
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
        (void)knurl_u64_to_size(config->limits.hard_body_bytes, &ceiling, NULL);
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

static knurl_status bind_listeners(struct knurl_runtime *runtime)
{
    const knurl_listener_config *config;
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
    knurl_status status;
    count = runtime->server->config.listener_count;
    runtime->bound = calloc(count, sizeof(*runtime->bound));
    runtime->addresses = calloc(count, sizeof(*runtime->addresses));
    runtime->native_servers = calloc(count, sizeof(*runtime->native_servers));
    if (runtime->bound == NULL || runtime->addresses == NULL ||
        runtime->native_servers == NULL)
        return knurl_fail(&runtime->error, KNURL_NOMEM, "listener_bind",
                          "cannot allocate bound listener registry");
    for (i = 0; i < count; ++i) {
        config = &runtime->server->config.listeners[i];
        (void)snprintf(name, sizeof(name), "knurl-%lu", (unsigned long)i);
        (void)snprintf(port, sizeof(port), "%u", (unsigned int)config->port);
        server = kore_server_create(name);
        runtime->native_servers[i] = server;
        server->tls = config->kind == KNURL_LISTENER_HTTPS;
        listener = kore_listener_create(server);
        listener->host = kore_strdup(config->bind);
        listener->port = kore_strdup(port);
        status =
            open_listener(config, runtime->server->config.http.socket_backlog,
                          &listener->fd, &listener->family, &runtime->error);
        if (status != KNURL_OK)
            return status;
        size = sizeof(address);
        if (getsockname(listener->fd, (struct sockaddr *)&address, &size) != 0)
            return listener_error(config, "listener_inspect",
                                  KNURL_ERROR_SYSTEM, errno, &runtime->error);
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
        runtime->bound[i].tls = server->tls;
        runtime->bound[i].address = knurl_view(runtime->addresses[i]);
        ++runtime->bound_count;
        knurl_kore_listener_domains(runtime, server, config);
    }
    return KNURL_OK;
}

knurl_status knurl_server_listener_at(const knurl_server *server, size_t index,
                                      knurl_bound_listener *out,
                                      knurl_error *error)
{
    if (server == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "server_listener_at",
                          "server and output are required");
    if (server->runtime == NULL)
        return knurl_fail(error, KNURL_STATE, "server_listener_at",
                          "listener addresses belong to the active run");
    if (index >= server->runtime->bound_count)
        return knurl_fail(error, KNURL_INVALID, "server_listener_at",
                          "listener index is outside the bound registry");
    *out = server->runtime->bound[index];
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_server_set_serving(knurl_server *server, int serving,
                                      knurl_error *error)
{
    struct knurl_runtime *runtime;
    if (server == NULL || server->runtime == NULL)
        return knurl_fail(error, KNURL_STATE, "server_set_serving",
                          "server has no active run");
    runtime = server->runtime;
    if (serving && (server->stop_requested || server->terminating_signal))
        return knurl_fail(error, KNURL_STATE, "server_set_serving",
                          "a stop has already been requested");
    if (getpid() != runtime->parent_pid ||
        !pthread_equal(pthread_self(), runtime->owner_thread) ||
        (server->state != KNURL_SERVER_SERVING &&
         server->state != KNURL_SERVER_READY_PAUSED))
        return knurl_fail(error, KNURL_STATE, "server_set_serving",
                          "serving gate is a ready parent-loop operation");
    __atomic_store_n(&runtime->shared->serving, serving != 0, __ATOMIC_RELEASE);
    runtime->gate_selected = 1;
    server->state = serving ? KNURL_SERVER_SERVING : KNURL_SERVER_READY_PAUSED;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_server_run(knurl_server *server, knurl_run_result *out,
                              knurl_error *error)
{
    struct knurl_runtime *runtime;
    struct kore_library_hooks hooks = {0};
    struct sigaction action;
    knurl_status status;
    knurl_status stopped_status;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->reason = KNURL_RUN_STARTUP_FAILURE;
        out->signal_number = out->escalated = 0;
    }
    status = validate_config(server, NULL, error);
    if (status != KNURL_OK)
        return status;
    runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL)
        return knurl_fail(error, KNURL_NOMEM, "server_run",
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
        return knurl_fail(error, KNURL_IO, "server_run",
                          "cannot allocate shared serving gate");
    }
    (void)pthread_mutex_lock(&runtime_lock);
    if (active_runtime != NULL) {
        (void)pthread_mutex_unlock(&runtime_lock);
        (void)munmap(runtime->shared, sizeof(*runtime->shared));
        free(runtime);
        return knurl_fail(error, KNURL_CONFLICT, "server_run",
                          "Kore supports one active server per process");
    }
    active_runtime = runtime;
    server->runtime = runtime;
    server->stop_requested = server->terminating_signal = 0;
    server->state = KNURL_SERVER_STARTING;
    server->router->frozen = 1;
    server->process.role = KNURL_PROCESS_PARENT;
    server->process.process_id = (long)runtime->parent_pid;
    (void)pthread_mutex_unlock(&runtime_lock);
    if (setjmp(runtime->unwind) == 0) {
        if (!wake_open(runtime))
            fatal_error(runtime, "cannot allocate runtime notification pipe");
        if (server->config.signals.mode == KNURL_SIGNALS_LIBRARY) {
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
        runtime->status = knurl_kore_loop_create(runtime, &runtime->error);
        if (runtime->status == KNURL_OK)
            runtime->status =
                knurl_kore_control_start(runtime, &runtime->error);
        if (runtime->status == KNURL_OK)
            runtime->status = knurl_runtime_hook(
                runtime, server->config.callbacks.on_process_start);
        if (runtime->status == KNURL_OK)
            runtime->status = knurl_runtime_hook(
                runtime, server->config.callbacks.on_parent_prepare);
        if (runtime->status == KNURL_OK)
            runtime->status = knurl_autoblock_open(server, &runtime->error);
        if (runtime->status == KNURL_OK)
            runtime->status =
                knurl_kore_logging_start(runtime, &runtime->error);
        runtime->cancelled = startup_cancel(runtime);
        if (runtime->status == KNURL_OK && !runtime->cancelled)
            runtime->status =
                knurl_tls_prepare(&server->config.tls, KORE_DEFAULT_CIPHER_LIST,
                                  &runtime->tls, &runtime->error);
        runtime->cancelled = startup_cancel(runtime);
        if (runtime->status == KNURL_OK && !runtime->cancelled)
            runtime->status = knurl_kore_acme_prepare(runtime, &runtime->error);
        if (runtime->status == KNURL_OK && !runtime->cancelled)
            runtime->status = knurl_kore_tls_registry(runtime, &runtime->error);
        if (runtime->status == KNURL_OK && !runtime->cancelled &&
            runtime->acme_root != NULL)
            runtime->status = knurl_acme_state_call(
                &server->config.tls, server, &server->process,
                KNURL_ACME_RESTORE, NULL, runtime->acme_root, &runtime->error);
        runtime->cancelled = startup_cancel(runtime);
        if (runtime->status == KNURL_OK && !runtime->cancelled) {
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
            hooks.loop_wait = knurl_kore_loop_wait;
            hooks.loop_turn = knurl_kore_loop_turn;
            hooks.parent_drain = knurl_kore_parent_drain;
            hooks.parent_cleanup = knurl_kore_loop_shutdown;
            hooks.worker_cleanup = knurl_kore_loop_shutdown;
            hooks.request_release = knurl_kore_response_release;
            hooks.net_progress = knurl_kore_response_progress;
            hooks.accept = knurl_kore_accept;
            hooks.tcp_stall = knurl_kore_tcp_stall;
            hooks.tls_failure = knurl_kore_tls_failure;
            hooks.http_status = knurl_kore_http_status;
            hooks.connection_status = knurl_kore_connection_status;
            hooks.worker_event = knurl_kore_worker_event;
            hooks.tls_updated = knurl_kore_tls_updated;
            if (runtime->acme_root != NULL) {
                hooks.keymgr_prepare = knurl_kore_acme_start;
                hooks.acme_persist = knurl_kore_acme_persist;
                hooks.acme_request = knurl_kore_acme_request;
            }
            runtime->initialized = 1;
            runtime->curl_timeout = kore_curl_timeout;
            runtime->curl_recv_max = kore_curl_recv_max;
            kore_library_init(&hooks);
            configure(runtime);
            knurl_kore_acme_configure(runtime);
            runtime->callbacks.http_request = knurl_kore_dispatch;
            runtime->callbacks.http_request_free = knurl_kore_request_free;
            runtime->status = bind_listeners(runtime);
            if (runtime->status == KNURL_OK && !kore_library_parent_start()) {
                runtime->cancelled = startup_cancel(runtime);
                if (!runtime->cancelled)
                    if (runtime->status == KNURL_OK)
                        runtime->status = knurl_fail(
                            &runtime->error,
                            kore_library_startup_timed_out ? KNURL_TIMEOUT
                                                           : KNURL_BACKEND,
                            "server_start",
                            "required worker did not initialize");
            }
            if (runtime->status == KNURL_OK &&
                server->config.http.access_log_path != NULL)
                kore_timer_add(kore_accesslog_run, 10, NULL, 0);
            runtime->cancelled = startup_cancel(runtime);
        }
        if (runtime->status == KNURL_OK && !runtime->cancelled) {
            kore_platform_event_level_read(runtime->wake[0],
                                           &runtime->wake_event);
            runtime->status =
                knurl_loop_activate(server->loop, &runtime->error);
        }
        if (runtime->status == KNURL_OK && !runtime->cancelled) {
            server->state = KNURL_SERVER_READY_PAUSED;
            runtime->status = knurl_runtime_hook(
                runtime, server->config.callbacks.on_parent_ready);
            if (runtime->status == KNURL_OK) {
                runtime->ready = 1;
                if (!runtime->gate_selected)
                    (void)knurl_server_set_serving(
                        server, !server->config.start_paused, NULL);
                knurl_kore_tls_observe(runtime, NULL);
            }
        }
        while (runtime->status == KNURL_OK && runtime->ready &&
               kore_quit == KORE_QUIT_NONE) {
            if (server->stop_requested || server->terminating_signal) {
                kore_quit = KORE_QUIT_NORMAL;
                break;
            }
            kore_library_parent_step(1000);
        }
    }
    server->state = KNURL_SERVER_STOPPING;
    __atomic_store_n(&runtime->shared->serving, 0, __ATOMIC_RELEASE);
    stopped_status = knurl_runtime_hook(
        runtime, server->config.callbacks.on_parent_stopping);
    if (runtime->status == KNURL_OK && stopped_status != KNURL_OK)
        runtime->status = stopped_status;
    if (runtime->initialized) {
        runtime->cleaning = 1;
        kore_library_cleanup();
        kore_curl_timeout = runtime->curl_timeout;
        kore_curl_recv_max = runtime->curl_recv_max;
    } else
        knurl_kore_loop_shutdown(runtime);
    knurl_autoblock_close(server);
    knurl_kore_logging_finish(runtime);
    knurl_kore_loop_finish(runtime);
    knurl_tls_prepared_clear(&runtime->tls);
    knurl_kore_acme_clear(runtime);
    while (runtime->installed_signals != 0) {
        --runtime->installed_signals;
        (void)sigaction(handled_signals[runtime->installed_signals],
                        &runtime->saved_signals[runtime->installed_signals],
                        NULL);
    }
    if (runtime->ready && kore_quit == KORE_QUIT_FATAL &&
        runtime->status == KNURL_OK) {
        if (runtime->has_failure) {
            runtime->error = runtime->last_failure.error;
            runtime->status = runtime->error.status;
        } else
            runtime->status =
                knurl_fail(&runtime->error, KNURL_BACKEND, "server_run",
                           "backend or worker failed");
    }
    stopped_status =
        knurl_runtime_hook(runtime, server->config.callbacks.on_parent_stopped);
    if (runtime->status == KNURL_OK && stopped_status != KNURL_OK)
        runtime->status = stopped_status;
    knurl_kore_control_close(runtime);
    if (out != NULL) {
        out->reason = runtime->status != KNURL_OK
                          ? (runtime->ready ? KNURL_RUN_BACKEND_FAILURE
                                            : KNURL_RUN_STARTUP_FAILURE)
                          : (server->terminating_signal != 0
                                 ? KNURL_RUN_TERMINATING_SIGNAL
                                 : KNURL_RUN_REQUESTED_STOP);
        out->signal_number = (int)server->terminating_signal;
        out->escalated = runtime->initialized && kore_library_escalated;
        out->has_failure = runtime->has_failure;
        out->last_failure = runtime->last_failure;
        out->recovered_failures = runtime->recovered_failures;
    }
    status = runtime->status;
    if (error != NULL)
        *error = runtime->error;
    (void)pthread_mutex_lock(&runtime_lock);
    wake_close(runtime);
    server->runtime = NULL;
    active_runtime = NULL;
    server->state =
        status == KNURL_OK ? KNURL_SERVER_STOPPED : KNURL_SERVER_FAILED;
    server->router->frozen = 0;
    (void)pthread_mutex_unlock(&runtime_lock);
    (void)munmap(runtime->shared, sizeof(*runtime->shared));
    knurl_kore_tls_clear(runtime);
    free(runtime->native_servers);
    free(runtime->bound);
    free(runtime->addresses);
    free(runtime);
    if (status == KNURL_OK)
        knurl_error_clear(error);
    return status;
}
