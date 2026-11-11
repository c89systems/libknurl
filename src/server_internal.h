#ifndef KNURL_SERVER_INTERNAL_H
#define KNURL_SERVER_INTERNAL_H

#include "internal.h"
#include <knurl/server.h>
#include <knurl/router.h>
#include <signal.h>

/* Reserve chunk framing storage and leave at least one payload byte. */
#define KNURL_RESPONSE_CHUNK_OVERHEAD 32U

struct knurl_runtime;
struct knurl_proxy_worker;
struct knurl_autoblock;
struct knurl_logging;

struct knurl_server_impl {
    knurl_server receiver;
    knurl_server_config config;
    knurl_timeout_config requested_timeouts;
    size_t requested_worker_count;
    knurl_router *router;
    knurl_server_state state;
    knurl_process_context process;
    struct knurl_runtime *runtime;
    knurl_loop *loop;
    struct knurl_proxy_worker *proxy;
    struct knurl_autoblock *autoblock;
    struct knurl_logging *logging;
    volatile sig_atomic_t notify_fd;
    volatile sig_atomic_t stop_requested;
    volatile sig_atomic_t terminating_signal;
};

knurl_status knurl_config_copy(const knurl_server_config *input,
                               knurl_server_config *out, knurl_error *error);
void knurl_config_cleanup(knurl_server_config *config);
knurl_status knurl_config_normalize(knurl_server_config *config,
                                    knurl_error *error);
int knurl_server_running(const knurl_server *server);

#endif
