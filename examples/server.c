/* Minimal foreground server. --check validates declarations without binding. */
#include <rotmos/rotmos.h>
#include <stdio.h>
#include <string.h>
static rotmos_status hello(rotmos_request *request, rotmos_reply *reply,
                           void *data, rotmos_error *error)
{
    (void)request;
    (void)data;
    return rotmos_reply_text(reply, 200, "text/plain", "hello\n", error);
}
int main(int argc, char **argv)
{
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_route_config route;
    rotmos_run_result result;
    rotmos_error error;
    rotmos_server *server = NULL;
    rotmos_status status;
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--check"))) {
        fprintf(stderr, "usage: %s [--check]\n", argv[0]);
        return 2;
    }
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    listener.port = 8080;
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    status = rotmos_server_new(&config, &server, &error);
    rotmos_route_config_init(&route);
    route.method = ROTMOS_HTTP_GET;
    route.path = "/";
    route.handler = hello;
    if (status == ROTMOS_OK)
        status = rotmos_router_route(rotmos_server_router(server), &route, NULL,
                                     &error);
    if (status == ROTMOS_OK)
        status = rotmos_server_validate(server, &error);
    if (status == ROTMOS_OK && argc == 1)
        status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    rotmos_server_destroy(server, NULL);
    return status == ROTMOS_OK ? 0 : 1;
}
