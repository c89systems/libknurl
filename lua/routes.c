#include "binding.h"

enum route_kind {
    ROUTE_TEXT,
    ROUTE_JSON,
    ROUTE_REDIRECT,
    ROUTE_STATIC_FILE,
    ROUTE_STATIC_DIRECTORY,
    ROUTE_UPLOAD_FILE,
    ROUTE_PROXY,
    ROUTE_APPLICATION
};

struct rl_fixed {
    struct rl_fixed *next;
    int status;
    knurl_bytes body;
    const char *type;
    const char *location;
    const char *cache;
};

void rl_fixed_free(struct rl_fixed *reply)
{
    while (reply != NULL) {
        struct rl_fixed *next = reply->next;
        free(reply);
        reply = next;
    }
}

static knurl_status fixed_reply(knurl_request *request, knurl_reply *reply,
                                void *data, knurl_error *error)
{
    const struct rl_fixed *fixed = data;
    knurl_status status;
    (void)request;
    status = knurl_reply_bytes(reply, fixed->status, fixed->type, fixed->body,
                               error);
    if (status == KNURL_OK && fixed->location != NULL)
        status =
            knurl_reply_header_add(reply, "Location", fixed->location, error);
    if (status == KNURL_OK && fixed->cache != NULL)
        status =
            knurl_reply_header_add(reply, "Cache-Control", fixed->cache, error);
    return status;
}

static const char *copy_text(char **cursor, const char *text)
{
    char *start = *cursor;
    if (text == NULL)
        return NULL;
    *cursor += strlen(text) + 1;
    memcpy(start, text, (size_t)(*cursor - start));
    return start;
}

static knurl_status register_fixed(lua_State *L, struct rl_server *server,
                                   knurl_route_group *group,
                                   enum route_kind kind, knurl_error *error)
{
    static const char *const fields[] = {
        "path",         "path_kind", "method",       "methods",
        "acl",          "hooks",     "status",       "body",
        "content_type", "location",  "cache_control"};
    knurl_route_config route;
    knurl_reply *validation;
    struct rl_fixed input, *fixed;
    char *cursor;
    size_t code, bytes, i;
    const char *strings[3];
    knurl_status status;
    int redirect = kind == ROUTE_REDIRECT;

    if (!rl_fields(L, 2, fields, RL_COUNT(fields), error))
        return error->status;
    knurl_route_config_init(&route);
    memset(&input, 0, sizeof(input));
    code = redirect ? 302U : 200U;
    input.type =
        kind == ROUTE_JSON ? "application/json" : "text/plain; charset=utf-8";
    input.body.data = kind == ROUTE_JSON ? "{\"ok\":true}\n"
                      : redirect         ? ""
                                         : "ok\n";
    input.body.length = strlen(input.body.data);
    if (!rl_route_path(L, 2, &route) ||
        !rl_methods(L, 2, KNURL_HTTP_METHODS_GET, &route.methods) ||
        !rl_field_size(L, 2, "status", &code) ||
        !rl_field_string(L, 2, "content_type", &input.type) ||
        !rl_field_string(L, 2, "location", &input.location) ||
        !rl_field_string(L, 2, "cache_control", &input.cache) ||
        (redirect && (code < 300 || code > 399 || input.location == NULL ||
                      input.location[0] == '\0')) ||
        (!redirect && (code < 200 || code > 599 || input.location != NULL)))
        return rl_fail(error, KNURL_INVALID,
                       "invalid fixed response or redirect fields");
    input.status = (int)code;
    rl_field(L, 2, "body");
    if (lua_type(L, -1) == LUA_TSTRING)
        input.body.data = lua_tolstring(L, -1, &input.body.length);
    else if (!lua_isnil(L, -1))
        return rl_fail(error, KNURL_INVALID,
                       "fixed response body must be a string");
    lua_pop(L, 1);
    if (!rl_acl_option(L, 2, &route.acl, error))
        return error->status;

    status = knurl_reply_new(&validation, error);
    if (status != KNURL_OK)
        return status;
    status = fixed_reply(NULL, validation, &input, error);
    knurl_reply_destroy(validation, NULL);
    if (status != KNURL_OK)
        return status;

    if (input.body.length > (size_t)-1 - sizeof(*fixed))
        return rl_fail(error, KNURL_LIMIT, "fixed response is too large");
    bytes = sizeof(*fixed) + input.body.length;
    strings[0] = input.type;
    strings[1] = input.location;
    strings[2] = input.cache;
    for (i = 0; i < RL_COUNT(strings); ++i) {
        size_t length = strings[i] == NULL ? 0 : strlen(strings[i]) + 1;
        if (length > (size_t)-1 - bytes)
            return rl_fail(error, KNURL_LIMIT, "fixed response is too large");
        bytes += length;
    }
    fixed = malloc(bytes);
    if (fixed == NULL)
        return rl_fail(error, KNURL_NOMEM, "copying fixed response");
    *fixed = input;
    cursor = (char *)(fixed + 1);
    fixed->type = copy_text(&cursor, input.type);
    fixed->location = copy_text(&cursor, input.location);
    fixed->cache = copy_text(&cursor, input.cache);
    memcpy(cursor, input.body.data, input.body.length);
    fixed->body.data = cursor;

    route.handler = fixed_reply;
    route.userdata = fixed;
    status = group != NULL
                 ? knurl_route_group_route(group, &route, NULL, error)
                 : knurl_router_route(knurl_server_router(server->native),
                                      &route, NULL, error);
    if (status == KNURL_OK) {
        fixed->next = server->replies;
        server->replies = fixed;
    } else {
        free(fixed);
    }
    return status;
}

static knurl_status register_static(lua_State *L, knurl_router *router,
                                    knurl_route_group *group, int directory,
                                    knurl_error *error)
{
    static const char *const fields[] = {"path",
                                         "file_path",
                                         "root_dir",
                                         "method",
                                         "methods",
                                         "acl",
                                         "hooks",
                                         "content_type",
                                         "cache_control",
                                         "index_file",
                                         "index_enabled",
                                         "not_found_body",
                                         "not_found_content_type",
                                         "mime_types",
                                         "compression_disabled",
                                         "precompressed_disabled"};
    knurl_static_config config;
    if (!rl_fields(L, 2, fields, RL_COUNT(fields), error))
        return error->status;
    knurl_static_config_init(&config);
    if (!rl_field_string(L, 2, "path", &config.path) ||
        !rl_field_string(L, 2, "file_path", &config.file_path) ||
        !rl_field_string(L, 2, "root_dir", &config.root_dir) ||
        !rl_field_string(L, 2, "content_type", &config.content_type) ||
        !rl_field_string(L, 2, "cache_control", &config.cache_control) ||
        !rl_field_string(L, 2, "index_file", &config.index_file) ||
        !rl_field_string(L, 2, "not_found_body", &config.not_found_body) ||
        !rl_field_string(L, 2, "not_found_content_type",
                         &config.not_found_content_type) ||
        !rl_field_bool(L, 2, "index_enabled", &config.index_enabled) ||
        !rl_field_bool(L, 2, "compression_disabled",
                       &config.compression_disabled) ||
        !rl_field_bool(L, 2, "precompressed_disabled",
                       &config.precompressed_disabled) ||
        !rl_methods(L, 2, config.methods, &config.methods) ||
        (directory && (config.root_dir == NULL || config.file_path != NULL)) ||
        (!directory && (config.file_path == NULL || config.root_dir != NULL)))
        return rl_fail(error, KNURL_INVALID, "invalid static route fields");
    rl_field(L, 2, "mime_types");
    if (!rl_mime_types(L, -1, &config.mime_types, &config.mime_type_count,
                       error) ||
        !rl_acl_option(L, 2, &config.acl, error))
        return error->status;
    if (directory)
        return group != NULL ? knurl_route_group_static_directory(
                                   group, &config, NULL, error)
                             : knurl_router_static_directory(router, &config,
                                                             NULL, error);
    return group != NULL
               ? knurl_route_group_static_file(group, &config, NULL, error)
               : knurl_router_static_file(router, &config, NULL, error);
}

static knurl_status register_upload(lua_State *L, struct rl_server *server,
                                    knurl_route_group *group,
                                    knurl_error *error)
{
    static const char *const fields[] = {
        "path",    "file_path",      "path_kind", "method",
        "methods", "max_body_bytes", "acl",       "hooks"};
    knurl_upload_file_config config;
    knurl_route_config path;
    if (!rl_fields(L, 2, fields, RL_COUNT(fields), error))
        return error->status;
    knurl_upload_file_config_init(&config);
    knurl_body_policy_init_stream(&config.body);
    knurl_route_config_init(&path);
    if (!rl_route_path(L, 2, &path) ||
        !rl_field_string(L, 2, "file_path", &config.file_path) ||
        !rl_methods(L, 2, config.methods, &config.methods))
        return rl_fail(error, KNURL_INVALID, "invalid file upload fields");
    if (!rl_body_limit(L, 2, server, &config.body.max_bytes, error) ||
        !rl_acl_option(L, 2, &config.acl, error))
        return error->status;
    config.path = path.path;
    config.path_kind = path.path_kind;
    return group != NULL
               ? knurl_route_group_upload_file(group, &config, NULL, error)
               : knurl_router_upload_file(knurl_server_router(server->native),
                                          &config, NULL, error);
}

static knurl_status register_proxy(lua_State *L, knurl_router *router,
                                   int application, knurl_error *error)
{
    static const char *const fields[] = {"path",
                                         "path_kind",
                                         "target",
                                         "method",
                                         "methods",
                                         "acl",
                                         "hooks",
                                         "connect_timeout_ms",
                                         "idle_timeout_ms",
                                         "total_timeout_ms",
                                         "buffer_limit_bytes",
                                         "upstream_http_version",
                                         "tls_ca_pem",
                                         "tls_client_cert_pem",
                                         "tls_client_key_pem"};
    knurl_proxy_config config;
    knurl_route_config path;
    const char *version = "auto";
    size_t connect = 0, idle = 0, total = application ? 30000U : 0;
    if (!rl_fields(L, 2, fields, RL_COUNT(fields), error))
        return error->status;
    knurl_proxy_config_init(&config);
    knurl_route_config_init(&path);
    if (!rl_route_path(L, 2, &path) ||
        !rl_field_string(L, 2, "target", &config.target) ||
        !rl_methods(L, 2, config.methods, &config.methods) ||
        !rl_field_size(L, 2, "connect_timeout_ms", &connect) ||
        connect > LONG_MAX || !rl_field_size(L, 2, "idle_timeout_ms", &idle) ||
        idle > LONG_MAX || !rl_field_size(L, 2, "total_timeout_ms", &total) ||
        total > LONG_MAX ||
        !rl_field_size(L, 2, "buffer_limit_bytes",
                       &config.buffer_limit_bytes) ||
        !rl_field_string(L, 2, "upstream_http_version", &version) ||
        !rl_field_string(L, 2, "tls_ca_pem", &config.tls_ca_pem) ||
        !rl_field_string(L, 2, "tls_client_cert_pem",
                         &config.tls_client_cert_pem) ||
        !rl_field_string(L, 2, "tls_client_key_pem",
                         &config.tls_client_key_pem))
        return rl_fail(error, KNURL_INVALID, "invalid proxy fields");
    if (!strcmp(version, "http1"))
        config.upstream_http_version = KNURL_PROXY_HTTP_1_1;
    else if (strcmp(version, "auto"))
        return rl_fail(error, KNURL_INVALID,
                       "proxy upstream_http_version must be auto or http1");
    if (!rl_acl_option(L, 2, &config.acl, error))
        return error->status;
    config.path = path.path;
    config.path_kind = path.path_kind;
    config.connect_timeout_ms = (long)connect;
    config.idle_timeout_ms = (long)idle;
    config.total_timeout_ms = (long)total;
    return knurl_router_proxy(router, &config, NULL, error);
}

static int route(lua_State *L)
{
    struct rl_server *server = luaL_testudata(L, 1, RL_SERVER);
    struct rl_group *group = luaL_testudata(L, 1, RL_GROUP);
    knurl_route_group *native_group = group != NULL ? group->native : NULL;
    enum route_kind kind =
        (enum route_kind)lua_tointeger(L, lua_upvalueindex(1));
    knurl_router *router;
    knurl_error error;
    knurl_status status;

    if (group != NULL)
        server = group->server;
    if (server == NULL || server->native == NULL)
        return rl_result(L, rl_fail(&error, KNURL_STATE, "server is closed"),
                         &error);
    if (server->running)
        return rl_result(L, rl_fail(&error, KNURL_STATE, "server is running"),
                         &error);
    router = knurl_server_router(server->native);
    switch (kind) {
    case ROUTE_TEXT:
    case ROUTE_JSON:
    case ROUTE_REDIRECT:
        status = register_fixed(L, server, native_group, kind, &error);
        break;
    case ROUTE_STATIC_FILE:
    case ROUTE_STATIC_DIRECTORY:
        status = register_static(L, router, native_group,
                                 kind == ROUTE_STATIC_DIRECTORY, &error);
        break;
    case ROUTE_UPLOAD_FILE:
        status = register_upload(L, server, native_group, &error);
        break;
    default:
        if (group != NULL)
            status = rl_fail(&error, KNURL_INVALID,
                             "proxy and application routes require a server");
        else
            status =
                register_proxy(L, router, kind == ROUTE_APPLICATION, &error);
        break;
    }
    return rl_result(L, status, &error);
}

void rl_routes_open(lua_State *L, int group)
{
    static const char *const methods[] = {
        "text",        "json",  "redirect",   "static_file", "static_directory",
        "upload_file", "proxy", "application"};
    size_t i;
    for (i = 0; i < (group ? (size_t)ROUTE_PROXY : RL_COUNT(methods)); ++i) {
        lua_pushinteger(L, (lua_Integer)i);
        lua_pushcclosure(L, route, 1);
        lua_setfield(L, -2, methods[i]);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "knurl");
    lua_setfield(L, -2, "__metatable");
}
