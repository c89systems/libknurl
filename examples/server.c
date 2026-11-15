/* Minimal foreground server. --check validates declarations without binding. */
#include <knurl/knurl.h>
#include <stdio.h>
#include <string.h>
static knurl_status hello(knurl_request *request, knurl_reply *reply,
                          void *data, knurl_error *error)
{
    (void)request;
    (void)data;
    return reply->text(reply, 200, "text/plain", "hello\n", error);
}
int main(int argc, char **argv)
{
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config route;
    knurl_run_result result;
    knurl_error error;
    knurl_server *server = NULL;
    knurl_router *router;
    knurl_status status;
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--check"))) {
        fprintf(stderr, "usage: %s [--check]\n", argv[0]);
        return 2;
    }
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    listener.port = 8080;
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    status = knurl_server_new(&config, &server, &error);
    knurl_route_config_init(&route);
    route.method = KNURL_HTTP_GET;
    route.path = "/";
    route.handler = hello;
    if (status == KNURL_OK) {
        router = server->router(server);
        status = router->route(router, &route, NULL, &error);
    }
    if (status == KNURL_OK)
        status = server->validate(server, &error);
    if (status == KNURL_OK && argc == 1)
        status = server->run(server, &result, &error);
    if (status != KNURL_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    if (server != NULL)
        server->destroy(server, NULL);
    return status == KNURL_OK ? 0 : 1;
}
