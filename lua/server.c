#include "binding.h"

static int array_size(lua_State *L, int index, size_t *count)
{
    size_t key;
    if (!lua_istable(L, index))
        return 0;
    *count = lua_rawlen(L, index);
    if (*count == 0 || *count > 255)
        return 0;
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (!lua_isinteger(L, -2) || !rl_size(L, -2, &key) || key == 0 ||
            key > *count || !lua_istable(L, -1)) {
            lua_pop(L, 2);
            return 0;
        }
        lua_pop(L, 1);
    }
    return 1;
}

static int listeners(lua_State *L, knurl_server_config *config,
                     knurl_error *error)
{
    static const char *const fields[] = {"name",
                                         "bind",
                                         "port",
                                         "kind",
                                         "identity",
                                         "presented_port",
                                         "redirect_https_port"};
    knurl_listener_config *items;
    size_t count, i, port, identity, presented, redirect;
    const char *kind;
    int list, item;
    rl_field(L, 1, "listeners");
    list = lua_absindex(L, -1);
    if (lua_isnil(L, list)) {
        lua_pop(L, 1);
        return 1;
    }
    if (!array_size(L, list, &count) || !lua_checkstack(L, 5)) {
        rl_fail(error, KNURL_INVALID,
                "listeners must be a nonempty array of at most 255 tables");
        return 0;
    }
    items = lua_newuserdatauv(L, count * sizeof(*items), 0);
    for (i = 0; i < count; ++i) {
        knurl_listener_config_init(&items[i]);
        items[i].bind = "127.0.0.1";
        lua_rawgeti(L, list, (lua_Integer)i + 1);
        item = lua_absindex(L, -1);
        kind = "http";
        port = presented = redirect = 0;
        identity = 1;
        if (!rl_fields(L, item, fields, RL_COUNT(fields), error))
            return 0;
        if (!rl_field_string(L, item, "name", &items[i].name) ||
            !rl_field_string(L, item, "bind", &items[i].bind) ||
            !rl_field_string(L, item, "kind", &kind) ||
            !rl_field_size(L, item, "port", &port) || port > 65535 ||
            !rl_field_size(L, item, "identity", &identity) || identity == 0 ||
            !rl_field_size(L, item, "presented_port", &presented) ||
            presented > 65535 ||
            !rl_field_size(L, item, "redirect_https_port", &redirect) ||
            redirect > 65535 ||
            (strcmp(kind, "http") && strcmp(kind, "https") &&
             strcmp(kind, "redirect"))) {
            rl_fail(error, KNURL_INVALID, "invalid listener configuration");
            return 0;
        }
        items[i].kind = strcmp(kind, "http") == 0    ? KNURL_LISTENER_HTTP
                        : strcmp(kind, "https") == 0 ? KNURL_LISTENER_HTTPS
                                                     : KNURL_LISTENER_REDIRECT;
        items[i].port = (unsigned short)port;
        items[i].identity_index = identity - 1;
        items[i].presented_port = (unsigned short)presented;
        items[i].redirect_https_port = (unsigned short)redirect;
        lua_pop(L, 1);
    }
    config->listeners = items;
    config->listener_count = count;
    return 1;
}

static int tls_files(lua_State *L, int index, knurl_tls_identity *identity,
                     knurl_error *error)
{
    knurl_tls_material *files[5];
    static const char *const keys[] = {"bundle", "certificate", "private_key",
                                       "extra_chain", "client_ca"};
    const char *mode = "manual";
    size_t i;
    files[0] = &identity->bundle;
    files[1] = &identity->certificate;
    files[2] = &identity->private_key;
    files[3] = &identity->extra_chain;
    files[4] = &identity->client_ca;
    for (i = 0; i < RL_COUNT(keys); ++i) {
        if (!rl_field_string(L, index, keys[i], &files[i]->path))
            goto invalid;
        if (files[i]->path != NULL)
            files[i]->kind = KNURL_MATERIAL_FILE;
    }
    if (!rl_field_string(L, index, "mode", &mode) ||
        (strcmp(mode, "manual") && strcmp(mode, "acme")) ||
        !rl_field_bool(L, index, "require_client_certificate",
                       &identity->require_client_certificate))
        goto invalid;
    identity->mode =
        strcmp(mode, "acme") == 0 ? KNURL_TLS_ACME : KNURL_TLS_MANUAL;
    return 1;
invalid:
    rl_fail(error, KNURL_INVALID, "invalid TLS identity declaration");
    return 0;
}

static int acme(lua_State *L, int index, knurl_acme_config *config,
                knurl_error *error)
{
    static const char *const fields[] = {
        "email",   "directory_url", "runtime_directory",
        "ca_file", "ca_directory",  "persist_timeout_ms"};
    size_t timeout = 0;
    int item;
    rl_field(L, index, "acme");
    item = lua_absindex(L, -1);
    if (lua_isnil(L, item)) {
        lua_pop(L, 1);
        return 1;
    }
    if (!rl_fields(L, item, fields, RL_COUNT(fields), error))
        return 0;
    if (!rl_field_string(L, item, "email", &config->email) ||
        !rl_field_string(L, item, "directory_url", &config->directory_url) ||
        !rl_field_string(L, item, "runtime_directory",
                         &config->runtime_directory) ||
        !rl_field_string(L, item, "ca_file", &config->ca_file) ||
        !rl_field_string(L, item, "ca_directory", &config->ca_directory) ||
        !rl_field_size(L, item, "persist_timeout_ms", &timeout) ||
        timeout > LONG_MAX) {
        rl_fail(error, KNURL_INVALID, "invalid ACME configuration");
        return 0;
    }
    config->persist_timeout_ms = (long)timeout;
    config->state = knurl_acme_file_state();
    lua_pop(L, 1);
    return 1;
}

static int tls(lua_State *L, knurl_tls_config *config, knurl_error *error)
{
    static const char *const root_fields[] = {"host",
                                              "mode",
                                              "bundle",
                                              "certificate",
                                              "private_key",
                                              "extra_chain",
                                              "client_ca",
                                              "require_client_certificate",
                                              "identities",
                                              "acme",
                                              "cipher_list",
                                              "version",
                                              "max_material_bytes"};
    static const char *const multi_fields[] = {
        "identities", "acme", "cipher_list", "version", "max_material_bytes"};
    static const char *const identity_fields[] = {
        "domains",     "mode",
        "bundle",      "certificate",
        "private_key", "extra_chain",
        "client_ca",   "require_client_certificate"};
    knurl_tls_identity *items;
    const char **host;
    const char *version = "default";
    size_t n = 1, i;
    int root, list, item, multi;
    rl_field(L, 1, "tls");
    root = lua_absindex(L, -1);
    if (lua_isnil(L, root)) {
        lua_pop(L, 1);
        return 1;
    }
    if (!lua_istable(L, root)) {
        rl_fail(error, KNURL_INVALID, "tls must be a table");
        return 0;
    }
    rl_field(L, root, "identities");
    list = lua_absindex(L, -1);
    multi = !lua_isnil(L, list);
    if (!(multi
              ? rl_fields(L, root, multi_fields, RL_COUNT(multi_fields), error)
              : rl_fields(L, root, root_fields, RL_COUNT(root_fields), error)))
        return 0;
    if ((multi && !array_size(L, list, &n)) ||
        !lua_checkstack(L, (int)(3 * n + 12))) {
        rl_fail(
            error, KNURL_INVALID,
            "tls.identities must be a nonempty array of at most 255 tables");
        return 0;
    }
    if (!rl_field_string(L, root, "version", &version) ||
        !rl_field_string(L, root, "cipher_list", &config->cipher_list) ||
        !rl_field_size(L, root, "max_material_bytes",
                       &config->max_material_bytes) ||
        (strcmp(version, "default") && strcmp(version, "both") &&
         strcmp(version, "1.2") && strcmp(version, "1.3"))) {
        rl_fail(error, KNURL_INVALID, "invalid TLS policy");
        return 0;
    }
    if (strcmp(version, "both") == 0)
        config->version = KNURL_TLS_VERSION_BOTH;
    else if (strcmp(version, "1.2") == 0)
        config->version = KNURL_TLS_VERSION_1_2;
    else if (strcmp(version, "1.3") == 0)
        config->version = KNURL_TLS_VERSION_1_3;
    if (!acme(L, root, &config->acme, error))
        return 0;
    items = lua_newuserdatauv(L, n * sizeof(*items), 0);
    for (i = 0; i < n; ++i) {
        knurl_tls_identity_init(&items[i]);
        if (multi) {
            lua_rawgeti(L, list, (lua_Integer)i + 1);
            item = lua_absindex(L, -1);
            if (!rl_fields(L, item, identity_fields, RL_COUNT(identity_fields),
                           error) ||
                !rl_string_array(L, item, "domains", &items[i].domains,
                                 &items[i].domain_count, error))
                return 0;
        } else {
            item = root;
            host = lua_newuserdatauv(L, sizeof(*host), 0);
            *host = NULL;
            if (!rl_field_string(L, item, "host", host) || *host == NULL) {
                rl_fail(error, KNURL_INVALID, "tls.host is required");
                return 0;
            }
            items[i].domains = host;
            items[i].domain_count = 1;
        }
        if (!tls_files(L, item, &items[i], error))
            return 0;
    }
    config->identities = items;
    config->identity_count = n;
    return 1;
}

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
                                         "listeners",
                                         "tls",
                                         "websocket_max_frame_bytes",
                                         "application_max_tasks",
                                         "application_timeout_ms"};
    knurl_server_config config;
    knurl_server_effective effective;
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
        !rl_field_size(L, 1, "websocket_max_frame_bytes",
                       &config.limits.websocket_max_frame_bytes) ||
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
    if (!client_ip(L, &config.client_ip, &error) ||
        !tls(L, &config.tls, &error) || !listeners(L, &config, &error))
        return rl_error(L, &error);
    if (config.listeners == &listener)
        listener.kind = config.tls.identity_count != 0 ? KNURL_LISTENER_HTTPS
                                                       : KNURL_LISTENER_HTTP;
    else {
        int explicit_bind, explicit_port;
        rl_field(L, 1, "bind");
        rl_field(L, 1, "port");
        explicit_bind = !lua_isnil(L, -2);
        explicit_port = !lua_isnil(L, -1);
        lua_pop(L, 2);
        if (explicit_bind || explicit_port)
            return rl_result(
                L,
                rl_fail(&error, KNURL_CONFLICT,
                        "listeners and top-level bind/port cannot be combined"),
                &error);
    }

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
    status = knurl_server_new(&config, &server->native, &error);
    if (status == KNURL_OK)
        status =
            knurl_server_effective_config(server->native, &effective, &error);
    if (status != KNURL_OK || !rl_logging_config(L, 1, server, &error))
        return rl_error(L, &error);
    server->hard_body_bytes = effective.config.limits.hard_body_bytes;
    server->websocket_max_frame_bytes =
        effective.config.limits.websocket_max_frame_bytes;
    return 1;
}

static int server_group(lua_State *L)
{
    static const char *const fields[] = {"prefix", "acl", "auth", "hooks"};
    struct rl_server *server = luaL_testudata(L, 1, RL_SERVER);
    struct rl_group *group;
    knurl_route_group_config config;
    knurl_async_route_hooks hooks;
    knurl_auth *auth;
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
    if (!rl_acl_option(L, 2, &config.acl, &error) ||
        !rl_auth_option(L, 2, &auth, &error))
        return rl_error(L, &error);
    group = lua_newuserdatauv(L, sizeof(*group), 1);
    group->server = server;
    group->native = NULL;
    luaL_setmetatable(L, RL_GROUP);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);
    if (!rl_hooks_prepare(L, server, &hooks, &error))
        return rl_error(L, &error);
    status = knurl_router_group(knurl_server_router(server->native), &config,
                                &group->native, &error);
    if (status == KNURL_OK && auth != NULL)
        status = knurl_route_group_set_auth(group->native, auth, &error);
    if (status == KNURL_OK && hooks.userdata != NULL)
        status =
            knurl_route_group_set_async_hooks(group->native, &hooks, &error);
    rl_hooks_finish(L, &hooks, status);
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
    rl_websocket_open(L, 0);
    rl_openapi_open(L);
    rl_auth_endpoints(L);
    lua_pop(L, 1);

    luaL_newmetatable(L, RL_GROUP);
    rl_routes_open(L, 1);
    rl_application_open(L, 1);
    rl_websocket_open(L, 1);
    lua_pop(L, 1);

    lua_pushcfunction(L, server_new);
    lua_setfield(L, -2, "server");
}
