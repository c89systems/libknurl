#ifndef ROTMOS_SERVER_INTERNAL_H
#define ROTMOS_SERVER_INTERNAL_H

#include "internal.h"
#include <rotmos/server.h>
#include <rotmos/router.h>
#include <signal.h>

/* Reserve chunk framing storage and leave at least one payload byte. */
#define ROTMOS_RESPONSE_CHUNK_OVERHEAD 32U

struct rotmos_runtime;
struct rotmos_proxy_worker;
struct rotmos_autoblock;
struct rotmos_logging;

struct rotmos_server {
    rotmos_server_config config;
    rotmos_timeout_config requested_timeouts;
    size_t requested_worker_count;
    rotmos_router *router;
    rotmos_server_state state;
    rotmos_process_context process;
    struct rotmos_runtime *runtime;
    rotmos_loop *loop;
    struct rotmos_proxy_worker *proxy;
    struct rotmos_autoblock *autoblock;
    struct rotmos_logging *logging;
    volatile sig_atomic_t notify_fd;
    volatile sig_atomic_t stop_requested;
    volatile sig_atomic_t terminating_signal;
};

rotmos_status rotmos_config_copy(const rotmos_server_config *input,
                                 rotmos_server_config *out,
                                 rotmos_error *error);
void rotmos_config_cleanup(rotmos_server_config *config);
rotmos_status rotmos_config_normalize(rotmos_server_config *config,
                                      rotmos_error *error);
int rotmos_server_running(const rotmos_server *server);

#endif
