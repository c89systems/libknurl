#include "binding.h"

static int client_ip(lua_State *L, knurl_client_ip_config *config,
                     knurl_error *error)
{
    static const char *const fields[] = {"trusted_proxies", "forwarded_header",
                                         "real_ip_header", "forwarded_format",
                                         "max_hops"};
    const char *format = "comma";
    int options;
    rl_field(L, 1, "client_ip");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    options = lua_absindex(L, -1);
    if (!rl_fields(L, options, fields, RL_COUNT(fields), error) ||
        !rl_string_array(L, options, "trusted_proxies",
                         &config->trusted_proxies, &config->trusted_proxy_count,
                         error))
        return 0;
    if (!rl_field_string(L, options, "forwarded_header",
                         &config->forwarded_header) ||
        !rl_field_string(L, options, "real_ip_header",
                         &config->real_ip_header) ||
        !rl_field_size(L, options, "max_hops", &config->max_hops) ||
        !rl_field_string(L, options, "forwarded_format", &format) ||
        (strcmp(format, "comma") && strcmp(format, "rfc7239"))) {
        rl_fail(error, KNURL_INVALID,
                "invalid client_ip forwarding configuration");
        return 0;
    }
    config->forwarded_format = strcmp(format, "comma") == 0
                                   ? KNURL_FORWARDED_COMMA
                                   : KNURL_FORWARDED_RFC7239;
    return 1;
}

static int server_new(lua_State *L)
{
    static const char *const fields[] = {"bind",
                                         "port",
                                         "workers",
                                         "max_connections",
                                         "backend_quiet",
                                         "logger",
                                         "access_log",
                                         "client_ip",
                                         "autoblock_disabled",
                                         "callbacks",
                                         "application_max_tasks",
                                         "application_timeout_ms"};
    knurl_server_config config;
    knurl_listener_config listener;
    struct rl_server *server;
    knurl_error error;
    knurl_status status;
    size_t port = 8080, max_tasks = 0, timeout = 0;
    int autoblock_disabled = 0;

    if (!rl_fields(L, 1, fields, RL_COUNT(fields), &error))
        return rl_error(L, &error);
    knurl_server_config_init_production_webserver(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = 1;
    config.workers.count = 1;
    config.workers.affinity_disabled = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    if (!rl_field_string(L, 1, "bind", &listener.bind) ||
        !rl_field_size(L, 1, "port", &port) || port > 65535UL ||
        !rl_field_size(L, 1, "workers", &config.workers.count) ||
        !rl_field_size(L, 1, "max_connections",
                       &config.limits.max_connections) ||
        !rl_field_bool(L, 1, "backend_quiet", &config.http.backend_quiet) ||
        !rl_field_bool(L, 1, "autoblock_disabled", &autoblock_disabled) ||
        !rl_field_size(L, 1, "application_max_tasks", &max_tasks) ||
        max_tasks > 256 ||
        !rl_field_size(L, 1, "application_timeout_ms", &timeout) ||
        timeout > LONG_MAX)
        return rl_result(
            L,
            rl_fail(&error, KNURL_INVALID, "invalid HTTP server configuration"),
            &error);
    listener.port = (unsigned short)port;
    if (autoblock_disabled)
        config.autoblock.enabled = 0;
    if (!client_ip(L, &config.client_ip, &error))
        return rl_error(L, &error);

    server = lua_newuserdatauv(L, sizeof(*server), 1);
    memset(server, 0, sizeof(*server));
    rl_logging_init(server);
    luaL_setmetatable(L, RL_SERVER);
    lua_newtable(L);
    lua_setiuservalue(L, -2, 1);
    knurl_blocking_pool_config_init(&server->pool);
    server->pool.threads = 1;
    server->pool.max_tasks = max_tasks;
    server->pool.timeout_ms = (long)timeout;
    server->hard_body_bytes = config.limits.hard_body_bytes;
    status = knurl_server_new(&config, &server->native, &error);
    if (status != KNURL_OK || !rl_logging_config(L, 1, server, &error))
        return rl_error(L, &error);
    return 1;
}

static int server_group(lua_State *L)
{
    static const char *const fields[] = {"prefix", "acl", "hooks"};
    struct rl_server *server = luaL_testudata(L, 1, RL_SERVER);
    struct rl_group *group;
    knurl_route_group_config config;
    knurl_error error;
    knurl_status status;

    if (server == NULL || server->native == NULL)
        return rl_result(L, rl_fail(&error, KNURL_STATE, "server is closed"),
                         &error);
    if (server->running)
        return rl_result(L, rl_fail(&error, KNURL_STATE, "server is running"),
                         &error);
    if (!rl_fields(L, 2, fields, RL_COUNT(fields), &error))
        return rl_error(L, &error);
    knurl_route_group_config_init(&config);
    if (!rl_field_string(L, 2, "prefix", &config.prefix) ||
        config.prefix == NULL)
        return rl_result(
            L, rl_fail(&error, KNURL_INVALID, "group prefix is required"),
            &error);
    if (!rl_acl_option(L, 2, &config.acl, &error))
        return rl_error(L, &error);
    group = lua_newuserdatauv(L, sizeof(*group), 1);
    group->server = server;
    group->native = NULL;
    luaL_setmetatable(L, RL_GROUP);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);
    status = knurl_router_group(knurl_server_router(server->native), &config,
                                &group->native, &error);
    return status == KNURL_OK ? 1 : rl_error(L, &error);
}

static int server_operation(lua_State *L)
{
    struct rl_server *server = luaL_testudata(L, 1, RL_SERVER);
    int operation = (int)lua_tointeger(L, lua_upvalueindex(1));
    knurl_error error;
    knurl_status status;
    if (server == NULL || server->native == NULL)
        return rl_result(L, rl_fail(&error, KNURL_STATE, "server is closed"),
                         &error);
    if (server->running)
        return rl_result(L, rl_fail(&error, KNURL_STATE, "server is running"),
                         &error);
    knurl_error_clear(&error);
    switch (operation) {
    case 0:
        status = knurl_server_validate(server->native, &error);
        break;
    case 1:
        status = knurl_server_preflight(server->native, &error);
        break;
    default:
        status = rl_server_run(L, server, &error);
        break;
    }
    return rl_result(L, status, &error);
}

static int server_close(lua_State *L)
{
    struct rl_server *server = luaL_testudata(L, 1, RL_SERVER);
    knurl_error error;
    knurl_status status;
    if (server != NULL && server->native != NULL) {
        if (server->running)
            return rl_result(
                L, rl_fail(&error, KNURL_STATE, "server is running"), &error);
        status = knurl_server_destroy(server->native, &error);
        if (status != KNURL_OK)
            return rl_error(L, &error);
        server->native = NULL;
        rl_logging_cleanup(server);
        rl_fixed_free(server->replies);
        server->replies = NULL;
        lua_pushnil(L);
        lua_setiuservalue(L, 1, 1);
    }
    return rl_result(L, KNURL_OK, NULL);
}

void rl_server_open(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"group", server_group},   {"set_pslog", rl_set_pslog},
        {"close", server_close},   {"__gc", server_close},
        {"__close", server_close}, {NULL, NULL}};
    static const char *const operations[] = {"validate", "preflight", "run"};
    size_t i;

    rl_logging_open(L);
    luaL_newmetatable(L, RL_SERVER);
    luaL_setfuncs(L, methods, 0);
    for (i = 0; i < RL_COUNT(operations); ++i) {
        lua_pushinteger(L, (lua_Integer)i);
        lua_pushcclosure(L, server_operation, 1);
        lua_setfield(L, -2, operations[i]);
    }
    rl_routes_open(L, 0);
    rl_application_open(L, 0);
    lua_pop(L, 1);

    luaL_newmetatable(L, RL_GROUP);
    rl_routes_open(L, 1);
    rl_application_open(L, 1);
    lua_pop(L, 1);

    lua_pushcfunction(L, server_new);
    lua_setfield(L, -2, "server");
}
