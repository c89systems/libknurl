#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define REQUIRE(expression)                                                    \
    do {                                                                       \
        if (!(expression)) {                                                   \
            (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__,           \
                          #expression);                                        \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static knurl_status should_not_run(knurl_server *server,
                                   const knurl_process_context *process,
                                   void *userdata, knurl_error *error)
{
    int *calls;
    (void)server;
    (void)process;
    (void)error;
    calls = userdata;
    ++*calls;
    return KNURL_STATE;
}

static knurl_status handler(knurl_request *request, knurl_reply *reply,
                            void *userdata, knurl_error *error)
{
    (void)request;
    (void)userdata;
    return knurl_reply_status(reply, 204, error);
}

static knurl_status headers(knurl_request *request, knurl_request_plan *plan,
                            void *userdata, knurl_error *error)
{
    int *calls;
    (void)request;
    (void)plan;
    (void)error;
    calls = userdata;
    ++*calls;
    return KNURL_STATE;
}

static knurl_status control(knurl_server *server, unsigned int events,
                            void *userdata, knurl_error *error)
{
    (void)server;
    (void)events;
    (void)userdata;
    knurl_error_clear(error);
    return KNURL_OK;
}

int main(void)
{
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_tls_identity identity;
    knurl_server_effective effective;
    knurl_server *server;
    knurl_server *other;
    knurl_router *router;
    knurl_router *standalone;
    knurl_route_config route;
    knurl_route_group_config group_config;
    knurl_route_group *group;
    knurl_route *first;
    const knurl_route *entry;
    knurl_error error;
    char bind[] = "127.0.0.1";
    char header[] = "custom";
    char domain[] = "example.test";
    char pem[] = "copied material";
    const char *domains[1];
    char path[] = "/items/:id";
    int fds[2];
    int calls;
    size_t queue_bytes;

    calls = 0;
    domains[0] = domain;
    knurl_server_config_init_production_webserver(&config);
    knurl_listener_config_init(&listener);
    knurl_tls_identity_init(&identity);
    listener.bind = bind;
    config.listeners = &listener;
    config.listener_count = 1;
    config.workers.count = 1;
    config.limits.max_connections = 0;
    config.http.server_header = header;
    config.timeouts.request_header.milliseconds = 1001;
    config.timeouts.request_body_idle.disabled = 1;
    config.callbacks.on_process_start = should_not_run;
    config.callbacks.userdata = &calls;
    identity.mode = KNURL_TLS_MANUAL;
    identity.domains = domains;
    identity.domain_count = 1;
    identity.bundle.kind = KNURL_MATERIAL_MEMORY;
    identity.bundle.bytes.data = pem;
    identity.bundle.bytes.length = strlen(pem);
    config.tls.identities = &identity;
    config.tls.identity_count = 1;
    REQUIRE(knurl_server_new(&config, &server, &error) == KNURL_OK);
    REQUIRE(calls == 0);
    bind[0] = header[0] = domain[0] = pem[0] = 'X';
    REQUIRE(knurl_server_effective_config(server, &effective, &error) ==
            KNURL_OK);
    REQUIRE(strcmp(effective.config.listeners[0].bind, "127.0.0.1") == 0);
    REQUIRE(strcmp(effective.config.http.server_header, "custom") == 0);
    REQUIRE(strcmp(effective.config.tls.identities[0].domains[0],
                   "example.test") == 0);
    REQUIRE(memcmp(effective.config.tls.identities[0].bundle.bytes.data,
                   "copied material", 15) == 0);
    REQUIRE(effective.config.limits.max_connections == 1024);
    REQUIRE(effective.config.limits.request_limit == 1024);
    REQUIRE(effective.config.timeouts.request_header.milliseconds == 2000);
    REQUIRE(effective.requested_timeouts.request_header.milliseconds == 1001);
    REQUIRE(effective.config.timeouts.request_body_idle.disabled);
    REQUIRE(effective.config.timeouts.request_body_idle.milliseconds == 0);
    REQUIRE(effective.config.limits.max_response_header_bytes == 0);
    REQUIRE(effective.max_header_fields == 23 &&
            effective.config.autoblock.status_rule_count == 4);
    REQUIRE(knurl_server_get_state(server) == KNURL_SERVER_DECLARED);

    router = knurl_server_router(server);
    REQUIRE(knurl_router_destroy(router, &error) == KNURL_STATE);
    knurl_route_group_config_init(&group_config);
    group_config.prefix = "/v1/";
    REQUIRE(knurl_router_group(router, &group_config, &group, &error) ==
            KNURL_OK);
    knurl_route_config_init(&route);
    route.method = KNURL_HTTP_GET;
    route.path_kind = KNURL_PATH_PARAMS;
    route.path = path;
    route.handler = handler;
    REQUIRE(knurl_route_group_route(group, &route, &first, &error) == KNURL_OK);
    REQUIRE(strcmp(knurl_route_declaration(first)->path, "/v1/items/:id") == 0);
    REQUIRE(knurl_route_group_route(group, &route, NULL, &error) ==
            KNURL_CONFLICT);
    route.method = KNURL_HTTP_POST;
    REQUIRE(knurl_route_group_route(group, &route, NULL, &error) == KNURL_OK);
    path[1] = 'X';
    route.path_kind = KNURL_PATH_LITERAL;
    route.path = "/v1/items/new";
    REQUIRE(knurl_router_route(router, &route, NULL, &error) == KNURL_OK);
    route.path_kind = KNURL_PATH_REGEX;
    route.path = "^/v1/.*$";
    REQUIRE(knurl_router_route(router, &route, NULL, &error) == KNURL_OK);
    route.path = "^/[";
    REQUIRE(knurl_router_route(router, &route, NULL, &error) == KNURL_INVALID);
    REQUIRE(knurl_router_route_count(router) == 4);
    REQUIRE(knurl_router_route_at(router, 0, &entry, &error) == KNURL_OK &&
            entry == first);
    REQUIRE(strcmp(knurl_route_declaration(first)->path, "/v1/items/:id") == 0);
    group_config.prefix = "/api.v1";
    REQUIRE(knurl_router_group(router, &group_config, &group, &error) ==
            KNURL_OK);
    route.path = "^/x$";
    REQUIRE(knurl_route_group_route(group, &route, &first, &error) == KNURL_OK);
    REQUIRE(strcmp(knurl_route_declaration(first)->path, "^/api\\.v1/x$") == 0);
    group_config.prefix = "/org/:org/";
    REQUIRE(knurl_router_group(router, &group_config, &group, &error) ==
            KNURL_OK);
    route.path_kind = KNURL_PATH_LITERAL;
    route.path = "/health";
    REQUIRE(knurl_route_group_route(group, &route, &first, &error) == KNURL_OK);
    REQUIRE(knurl_route_declaration(first)->path_kind == KNURL_PATH_PARAMS);
    REQUIRE(strcmp(knurl_route_declaration(first)->path, "/org/:org/health") ==
            0);
    REQUIRE(knurl_router_new(&standalone, &error) == KNURL_OK);
    REQUIRE(knurl_server_router_take(server, standalone, &error) ==
            KNURL_STATE);
    REQUIRE(knurl_server_new(NULL, &other, &error) == KNURL_OK);
    REQUIRE(knurl_server_router_take(other, standalone, &error) == KNURL_OK);
    REQUIRE(knurl_router_destroy(standalone, &error) == KNURL_STATE);
    REQUIRE(knurl_server_destroy(other, &error) == KNURL_OK);
    REQUIRE(knurl_server_destroy(server, &error) == KNURL_OK && calls == 0);

    knurl_server_config_init(&config);
    config.limits.max_response_header_bytes = 1023;
    REQUIRE(knurl_server_new(&config, &server, &error) == KNURL_INVALID);

    knurl_server_config_init(&config);
    config.io.chunk_bytes = config.io.input_queue_bytes = 1;
    for (queue_bytes = 1; queue_bytes <= 32; ++queue_bytes) {
        config.io.output_queue_bytes = queue_bytes;
        REQUIRE(knurl_server_new(&config, &server, &error) == KNURL_INVALID);
        REQUIRE(server == NULL && error.status == KNURL_INVALID &&
                strcmp(error.operation, "server_config") == 0 &&
                strstr(error.message, "33 bytes") != NULL);
    }
    config.io.output_queue_bytes = 33;
    REQUIRE(knurl_server_new(&config, &server, &error) == KNURL_OK);
    REQUIRE(knurl_server_effective_config(server, &effective, &error) ==
                KNURL_OK &&
            effective.config.io.output_queue_bytes == 33);
    REQUIRE(knurl_server_destroy(server, &error) == KNURL_OK);
    config.io.output_queue_bytes = 0;
    REQUIRE(knurl_server_new(&config, &server, &error) == KNURL_OK);
    REQUIRE(knurl_server_effective_config(server, &effective, &error) ==
                KNURL_OK &&
            effective.config.io.output_queue_bytes == 262144UL);
    REQUIRE(knurl_server_destroy(server, &error) == KNURL_OK);

    knurl_server_config_init(&config);
    config.dispatch.mode = KNURL_DISPATCH_CALLBACK;
    REQUIRE(knurl_server_new(&config, &server, &error) == KNURL_INVALID);
    config.dispatch.on_headers = headers;
    config.dispatch.userdata = &calls;
    REQUIRE(knurl_server_new(&config, &server, &error) == KNURL_OK);
    knurl_route_config_init(&route);
    route.path = "/raw";
    route.handler = handler;
    route.on_headers = headers;
    REQUIRE(knurl_router_route(knurl_server_router(server), &route, NULL,
                               &error) == KNURL_INVALID);
    route.handler = NULL;
    REQUIRE(knurl_router_route(knurl_server_router(server), &route, NULL,
                               &error) == KNURL_OK);
    REQUIRE(knurl_server_destroy(server, &error) == KNURL_OK && calls == 0);
    config.dispatch.mode = KNURL_DISPATCH_ROUTER;
    REQUIRE(knurl_server_new(&config, &server, &error) == KNURL_INVALID);

    REQUIRE(pipe(fds) == 0);
    knurl_server_config_init(&config);
    config.supervision.enabled = 1;
    config.supervision.control_fd = fds[0];
    config.supervision.on_event = control;
    config.workers.count = 254;
    server = NULL;
    REQUIRE(knurl_server_new(&config, &server, &error) == KNURL_INVALID &&
            server == NULL);
    REQUIRE(fcntl(fds[0], F_GETFD) != -1);
    config.workers.count = 1;
    REQUIRE(knurl_server_new(&config, &server, &error) == KNURL_OK);
    REQUIRE(knurl_server_destroy(server, &error) == KNURL_OK);
    REQUIRE(fcntl(fds[0], F_GETFD) == -1);
    REQUIRE(fcntl(fds[1], F_GETFD) != -1);
    REQUIRE(close(fds[1]) == 0);
    return 0;
}
