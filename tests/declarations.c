#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>

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

static rotmos_status should_not_run(rotmos_server *server,
                                    const rotmos_process_context *process,
                                    void *userdata, rotmos_error *error)
{
    int *calls;
    (void)server;
    (void)process;
    (void)error;
    calls = userdata;
    ++*calls;
    return ROTMOS_STATE;
}

static rotmos_status handler(rotmos_request *request, rotmos_reply *reply,
                             void *userdata, rotmos_error *error)
{
    (void)request;
    (void)userdata;
    return rotmos_reply_status(reply, 204, error);
}

static rotmos_status control(rotmos_server *server, unsigned int events,
                             void *userdata, rotmos_error *error)
{
    (void)server;
    (void)events;
    (void)userdata;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

int main(void)
{
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_tls_identity identity;
    rotmos_server_effective effective;
    rotmos_server *server;
    rotmos_server *other;
    rotmos_router *router;
    rotmos_router *standalone;
    rotmos_route_config route;
    rotmos_route_group_config group_config;
    rotmos_route_group *group;
    rotmos_route *first;
    const rotmos_route *entry;
    rotmos_error error;
    char bind[] = "127.0.0.1";
    char header[] = "custom";
    char domain[] = "example.test";
    char pem[] = "copied material";
    const char *domains[1];
    char path[] = "/items/:id";
    int fds[2];
    int calls;

    calls = 0;
    domains[0] = domain;
    rotmos_server_config_init_production_webserver(&config);
    rotmos_listener_config_init(&listener);
    rotmos_tls_identity_init(&identity);
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
    identity.mode = ROTMOS_TLS_MANUAL;
    identity.domains = domains;
    identity.domain_count = 1;
    identity.bundle.kind = ROTMOS_MATERIAL_MEMORY;
    identity.bundle.bytes.data = pem;
    identity.bundle.bytes.length = strlen(pem);
    config.tls.identities = &identity;
    config.tls.identity_count = 1;
    REQUIRE(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    REQUIRE(calls == 0);
    bind[0] = header[0] = domain[0] = pem[0] = 'X';
    REQUIRE(rotmos_server_effective_config(server, &effective, &error) ==
            ROTMOS_OK);
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
    REQUIRE(effective.max_header_fields == 23 &&
            effective.config.autoblock.status_rule_count == 4);
    REQUIRE(rotmos_server_get_state(server) == ROTMOS_SERVER_DECLARED);

    router = rotmos_server_router(server);
    REQUIRE(rotmos_router_destroy(router, &error) == ROTMOS_STATE);
    rotmos_route_group_config_init(&group_config);
    group_config.prefix = "/v1/";
    REQUIRE(rotmos_router_group(router, &group_config, &group, &error) ==
            ROTMOS_OK);
    rotmos_route_config_init(&route);
    route.method = ROTMOS_HTTP_GET;
    route.path_kind = ROTMOS_PATH_PARAMS;
    route.path = path;
    route.handler = handler;
    REQUIRE(rotmos_route_group_route(group, &route, &first, &error) ==
            ROTMOS_OK);
    REQUIRE(strcmp(rotmos_route_declaration(first)->path, "/v1/items/:id") ==
            0);
    REQUIRE(rotmos_route_group_route(group, &route, NULL, &error) ==
            ROTMOS_CONFLICT);
    route.method = ROTMOS_HTTP_POST;
    REQUIRE(rotmos_route_group_route(group, &route, NULL, &error) == ROTMOS_OK);
    path[1] = 'X';
    route.path_kind = ROTMOS_PATH_LITERAL;
    route.path = "/v1/items/new";
    REQUIRE(rotmos_router_route(router, &route, NULL, &error) == ROTMOS_OK);
    route.path_kind = ROTMOS_PATH_REGEX;
    route.path = "^/v1/.*$";
    REQUIRE(rotmos_router_route(router, &route, NULL, &error) == ROTMOS_OK);
    route.path = "^/[";
    REQUIRE(rotmos_router_route(router, &route, NULL, &error) ==
            ROTMOS_INVALID);
    REQUIRE(rotmos_router_route_count(router) == 4);
    REQUIRE(rotmos_router_route_at(router, 0, &entry, &error) == ROTMOS_OK &&
            entry == first);
    REQUIRE(strcmp(rotmos_route_declaration(first)->path, "/v1/items/:id") ==
            0);
    group_config.prefix = "/api.v1";
    REQUIRE(rotmos_router_group(router, &group_config, &group, &error) ==
            ROTMOS_OK);
    route.path = "^/x$";
    REQUIRE(rotmos_route_group_route(group, &route, &first, &error) ==
            ROTMOS_OK);
    REQUIRE(strcmp(rotmos_route_declaration(first)->path, "^/api\\.v1/x$") ==
            0);
    group_config.prefix = "/org/:org/";
    REQUIRE(rotmos_router_group(router, &group_config, &group, &error) ==
            ROTMOS_OK);
    route.path_kind = ROTMOS_PATH_LITERAL;
    route.path = "/health";
    REQUIRE(rotmos_route_group_route(group, &route, &first, &error) ==
            ROTMOS_OK);
    REQUIRE(rotmos_route_declaration(first)->path_kind == ROTMOS_PATH_PARAMS);
    REQUIRE(strcmp(rotmos_route_declaration(first)->path, "/org/:org/health") ==
            0);
    REQUIRE(rotmos_router_new(&standalone, &error) == ROTMOS_OK);
    REQUIRE(rotmos_server_router_take(server, standalone, &error) ==
            ROTMOS_STATE);
    REQUIRE(rotmos_server_new(NULL, &other, &error) == ROTMOS_OK);
    REQUIRE(rotmos_server_router_take(other, standalone, &error) == ROTMOS_OK);
    REQUIRE(rotmos_router_destroy(standalone, &error) == ROTMOS_STATE);
    REQUIRE(rotmos_server_destroy(other, &error) == ROTMOS_OK);
    REQUIRE(rotmos_server_destroy(server, &error) == ROTMOS_OK && calls == 0);

    REQUIRE(pipe(fds) == 0);
    rotmos_server_config_init(&config);
    config.supervision.enabled = 1;
    config.supervision.control_fd = fds[0];
    config.supervision.on_event = control;
    config.workers.count = 254;
    server = NULL;
    REQUIRE(rotmos_server_new(&config, &server, &error) == ROTMOS_INVALID &&
            server == NULL);
    REQUIRE(fcntl(fds[0], F_GETFD) != -1);
    config.workers.count = 1;
    REQUIRE(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    REQUIRE(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    REQUIRE(fcntl(fds[0], F_GETFD) == -1);
    REQUIRE(fcntl(fds[1], F_GETFD) != -1);
    REQUIRE(close(fds[1]) == 0);
    return 0;
}
