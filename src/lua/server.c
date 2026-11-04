#include "binding.h"
struct setting {
    const char *name;
    size_t offset;
    int kind;
};
#define SZ(t, n) {#n, offsetof(t, n), 0}
#define UI(t, n) {#n, offsetof(t, n), 1}
#define LG(t, n) {#n, offsetof(t, n), 2}
#define BL(t, n) {#n, offsetof(t, n), 3}
#define WIDE(t, n) {#n, offsetof(t, n), 4}
#define TEXT(t, n) {#n, offsetof(t, n), 5}
#define UL(t, n) {#n, offsetof(t, n), 6}
static int settings(lua_State *L, int index, void *config,
                    const struct setting *fields, size_t count)
{
    size_t i, n;
    unsigned char *base = config;
    int ok;
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        const char *name =
            lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        for (i = 0; name != NULL && i < count; ++i)
            if (!strcmp(name, fields[i].name))
                break;
        lua_pop(L, 1);
        if (name == NULL || i == count) {
            lua_pop(L, 1);
            return 0;
        }
    }
    for (i = 0; i < count; ++i) {
        rl_field(L, index, fields[i].name);
        if (!lua_isnil(L, -1)) {
            if (fields[i].kind == 4) {
                ok = rl_u64(L, -1, (rotmos_u64 *)(base + fields[i].offset));
            } else if (fields[i].kind == 5) {
                size_t length;
                const char *text;
                ok = lua_type(L, -1) == LUA_TSTRING;
                if (ok) {
                    text = lua_tolstring(L, -1, &length);
                    ok = memchr(text, 0, length) == NULL;
                    if (ok)
                        *(const char **)(base + fields[i].offset) = text;
                }
            } else if (fields[i].kind == 3) {
                ok = lua_isboolean(L, -1);
                if (ok)
                    *(int *)(base + fields[i].offset) = lua_toboolean(L, -1);
            } else {
                ok = rl_size(L, -1, &n);
                if (ok) {
                    if (fields[i].kind == 0)
                        *(size_t *)(base + fields[i].offset) = n;
                    else if (fields[i].kind == 1) {
                        ok = n <= UINT_MAX;
                        if (ok)
                            *(unsigned int *)(base + fields[i].offset) =
                                (unsigned int)n;
                    } else if (fields[i].kind == 6) {
                        *(unsigned long *)(base + fields[i].offset) =
                            (unsigned long)n;
                    } else {
                        ok = n <= (size_t)LONG_MAX;
                        if (ok)
                            *(long *)(base + fields[i].offset) = (long)n;
                    }
                }
            }
            if (!ok) {
                lua_pop(L, 1);
                return 0;
            }
        }
        lua_pop(L, 1);
    }
    return 1;
}
struct rl_server *rl_server(lua_State *L, int index)
{
    return luaL_testudata(L, index, RL_SERVER);
}
static int l_close(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    rotmos_error error;
    rotmos_status status;
    if (s == NULL)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_INVALID, "server required"), &error));
    status = rotmos_server_destroy(s->native, &error);
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    s->native = NULL;
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 1);
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 2);
    lua_pushboolean(L, 1);
    return 1;
}
static int l_gc(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    if (s != NULL) {
        free(s->pending_listeners);
        s->pending_listeners = NULL;
        if (s->native != NULL &&
            rotmos_server_destroy(s->native, NULL) == ROTMOS_OK)
            s->native = NULL;
    }
    return 0;
}
static void process_snapshot(lua_State *L, const rotmos_process_context *p)
{
    static const char *names[] = {"parent", "http", "key_manager", "acme"};
    lua_createtable(L, 0, 4);
    lua_pushstring(L, names[p->role]);
    lua_setfield(L, -2, "role");
    lua_pushinteger(L, p->process_id);
    lua_setfield(L, -2, "process_id");
    lua_pushinteger(L, p->worker_index);
    lua_setfield(L, -2, "worker_index");
    lua_pushboolean(L, p->restarted);
    lua_setfield(L, -2, "restarted");
}
static int process_trampoline(lua_State *L)
{
    struct rl_call *c = lua_touserdata(L, 1);
    int root;
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->server->root);
    root = lua_gettop(L);
    lua_getiuservalue(L, root, 1);
    rl_field(L, -1, "callbacks");
    if (!lua_istable(L, -1))
        return 0;
    rl_field(L, -1, c->name);
    if (lua_isnil(L, -1))
        return 0;
    if (!lua_isfunction(L, -1)) {
        c->status = rl_fail(c->error, ROTMOS_INVALID,
                            "lifecycle callback must be a function");
        return 0;
    }
    lua_pushvalue(L, root);
    process_snapshot(L, c->process);
    lua_call(L, 2, 2);
    if (lua_isboolean(L, -2) && !lua_toboolean(L, -2))
        c->status = rl_fail(c->error, ROTMOS_STATE,
                            "lifecycle callback rejected operation");
    else if (!lua_isnil(L, -1))
        c->status = rl_callback_error(L, -1, c->error,
                                      "lifecycle callback returned an error");
    return 0;
}
static rotmos_status process_call(rotmos_server *native,
                                  const rotmos_process_context *p, void *data,
                                  rotmos_error *error, const char *name)
{
    struct rl_server *s = data;
    struct rl_call c;
    rotmos_status status;
    (void)native;
    memset(&c, 0, sizeof(c));
    c.server = s;
    c.process = p;
    c.name = name;
    c.error = error;
    status = rl_protect(s->L, process_trampoline, &c, error);
    return status == ROTMOS_OK ? c.status : status;
}
static rotmos_status on_process_start(rotmos_server *server,
                                      const rotmos_process_context *process,
                                      void *data, rotmos_error *error)
{
    return process_call(server, process, data, error, "on_process_start");
}
static rotmos_status on_parent_prepare(rotmos_server *server,
                                       const rotmos_process_context *process,
                                       void *data, rotmos_error *error)
{
    return process_call(server, process, data, error, "on_parent_prepare");
}
static rotmos_status on_parent_ready(rotmos_server *server,
                                     const rotmos_process_context *process,
                                     void *data, rotmos_error *error)
{
    return process_call(server, process, data, error, "on_parent_ready");
}
static rotmos_status on_parent_stopping(rotmos_server *server,
                                        const rotmos_process_context *process,
                                        void *data, rotmos_error *error)
{
    return process_call(server, process, data, error, "on_parent_stopping");
}
static rotmos_status on_parent_stopped(rotmos_server *server,
                                       const rotmos_process_context *process,
                                       void *data, rotmos_error *error)
{
    return process_call(server, process, data, error, "on_parent_stopped");
}
static rotmos_status on_worker_start(rotmos_server *server,
                                     const rotmos_process_context *process,
                                     void *data, rotmos_error *error)
{
    return process_call(server, process, data, error, "on_worker_start");
}
static rotmos_status on_worker_stopping(rotmos_server *server,
                                        const rotmos_process_context *process,
                                        void *data, rotmos_error *error)
{
    return process_call(server, process, data, error, "on_worker_stopping");
}
static int on_worker_exit(rotmos_server *native,
                          const rotmos_process_context *p, void *data)
{
    struct rl_server *s = data;
    rotmos_error error;
    rotmos_status status =
        process_call(native, p, data, &error, "on_worker_exit");
    if (status != ROTMOS_OK && s->callback_error.status == ROTMOS_OK)
        s->callback_error = error;
    return status != ROTMOS_OK || s->callback_error.status != ROTMOS_OK;
}
static int config_parse(lua_State *L, int options, rotmos_server_config *c,
                        rotmos_listener_config **allocated, rotmos_error *error)
{
    static const struct setting worker[] = {
        SZ(rotmos_worker_config, count),
        SZ(rotmos_worker_config, accept_threshold),
        SZ(rotmos_worker_config, rlimit_nofiles),
        LG(rotmos_worker_config, shutdown_timeout_ms),
        BL(rotmos_worker_config, affinity_disabled)};
    static const struct setting io[] = {
        SZ(rotmos_io_config, chunk_bytes),
        SZ(rotmos_io_config, input_queue_bytes),
        SZ(rotmos_io_config, output_queue_bytes)};
    static const struct setting limits[] = {
        SZ(rotmos_limit_config, max_connections),
        SZ(rotmos_limit_config, request_limit),
        SZ(rotmos_limit_config, max_request_header_bytes),
        SZ(rotmos_limit_config, max_response_header_bytes),
        SZ(rotmos_limit_config, websocket_max_frame_bytes),
        WIDE(rotmos_limit_config, default_body_bytes),
        WIDE(rotmos_limit_config, hard_body_bytes),
        WIDE(rotmos_limit_config, body_min_rate_bytes_per_second),
        TEXT(rotmos_limit_config, body_spool_directory),
        BL(rotmos_limit_config, body_min_rate_disabled)};
    static const struct setting http[] = {
        BL(rotmos_http_config, keepalive_disabled),
        UI(rotmos_http_config, keepalive_max_requests),
        UI(rotmos_http_config, socket_backlog),
        UI(rotmos_http_config, request_process_budget_ms),
        BL(rotmos_http_config, backend_quiet),
        TEXT(rotmos_http_config, server_header),
        TEXT(rotmos_http_config, access_log_path),
        UL(rotmos_http_config, hsts_max_age_seconds),
        UL(rotmos_http_config, backend_curl_timeout_seconds),
        WIDE(rotmos_http_config, backend_curl_recv_max_bytes),
        BL(rotmos_http_config, pretty_error_pages)};
    size_t n, i, port;
    const char *kind = NULL;
    int table;
    static const char *const allowed[] = {"name",    "listeners", "signals",
                                          "workers", "io",        "limits",
                                          "http",    "callbacks", "tls"};
    static const char *const callbacks[] = {
        "on_process_start",   "on_parent_prepare", "on_parent_ready",
        "on_parent_stopping", "on_parent_stopped", "on_worker_start",
        "on_worker_stopping", "on_worker_exit"};
    if (!rl_fields(L, options, allowed, sizeof(allowed) / sizeof(allowed[0]),
                   error))
        return 0;
    rl_field(L, options, "callbacks");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            goto invalid;
        if (!rl_fields(L, -1, callbacks,
                       sizeof(callbacks) / sizeof(callbacks[0]), error))
            return 0;
        for (i = 0; i < sizeof(callbacks) / sizeof(callbacks[0]); ++i) {
            rl_field(L, -1, callbacks[i]);
            if (!lua_isnil(L, -1) && !lua_isfunction(L, -1))
                goto invalid;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (!rl_field_string(L, options, "name", &c->name))
        goto invalid;
    rl_field(L, options, "tls");
    if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        rl_fail(error, ROTMOS_NOT_SUPPORTED,
                "Lua TLS configuration is not connected yet");
        return 0;
    }
    lua_pop(L, 1);
#define GROUP(key, member, fields)                                             \
    do {                                                                       \
        rl_field(L, options, key);                                             \
        if (!lua_isnil(L, -1) &&                                               \
            (!lua_istable(L, -1) ||                                            \
             !settings(L, -1, &c->member, fields,                              \
                       sizeof(fields) / sizeof(fields[0]))))                   \
            goto invalid;                                                      \
        lua_pop(L, 1);                                                         \
    } while (0)
    GROUP("workers", workers, worker);
    GROUP("io", io, io);
    GROUP("limits", limits, limits);
    GROUP("http", http, http);
#undef GROUP
    rl_field(L, options, "signals");
    if (!lua_isnil(L, -1)) {
        static const char *const fields[] = {"mode"};
        if (lua_istable(L, -1) && !rl_fields(L, -1, fields, 1, error))
            return 0;
        if (!lua_istable(L, -1) || !rl_field_string(L, -1, "mode", &kind))
            goto invalid;
        if (kind != NULL) {
            if (!strcmp(kind, "library"))
                c->signals.mode = ROTMOS_SIGNALS_LIBRARY;
            else if (strcmp(kind, "host"))
                goto invalid;
        }
    }
    lua_pop(L, 1);
    rl_field(L, options, "listeners");
    if (!lua_istable(L, -1))
        goto invalid;
    table = lua_absindex(L, -1);
    n = lua_rawlen(L, table);
    if (n == 0 || n > 255)
        goto invalid;
    *allocated = calloc(n, sizeof(**allocated));
    if (*allocated == NULL) {
        rl_fail(error, ROTMOS_NOMEM, "allocating listener declarations");
        return 0;
    }
    for (i = 0; i < n; ++i) {
        static const char *const fields[] = {"bind", "name", "port", "kind"};
        lua_rawgeti(L, table, (lua_Integer)i + 1);
        if (!lua_istable(L, -1))
            goto invalid;
        if (!rl_fields(L, -1, fields, sizeof(fields) / sizeof(fields[0]),
                       error))
            return 0;
        rotmos_listener_config_init(&(*allocated)[i]);
        port = 0;
        kind = NULL;
        if (!rl_field_string(L, -1, "bind", &(*allocated)[i].bind) ||
            !rl_field_string(L, -1, "name", &(*allocated)[i].name) ||
            !rl_field_size(L, -1, "port", &port) || port > 65535 ||
            !rl_field_string(L, -1, "kind", &kind))
            goto invalid;
        if (kind != NULL && strcmp(kind, "http")) {
            rl_fail(error, ROTMOS_NOT_SUPPORTED,
                    "Lua HTTPS/redirect listeners are not connected yet");
            return 0;
        }
        (*allocated)[i].port = (unsigned short)port;
        lua_pop(L, 1);
    }
    c->listeners = *allocated;
    c->listener_count = n;
    lua_pop(L, 1);
    return 1;
invalid:
    rl_fail(error, ROTMOS_INVALID,
            "invalid server configuration field or integer range");
    return 0;
}
static int l_new(lua_State *L)
{
    struct rl_server *s;
    rotmos_server_config config;
    rotmos_error error;
    rotmos_status status;
    if (!lua_istable(L, 1))
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "server configuration table required"),
                            &error));
    s = lua_newuserdatauv(L, sizeof(*s), 2);
    memset(s, 0, sizeof(*s));
    s->L = L;
    s->root = LUA_NOREF;
    luaL_setmetatable(L, RL_SERVER);
    rl_table_copy(L, 1);
    rl_field(L, -1, "callbacks");
    if (lua_istable(L, -1)) {
        rl_table_copy(L, -1);
        lua_setfield(L, -3, "callbacks");
    }
    lua_pop(L, 1);
    lua_setiuservalue(L, -2, 1);
    lua_newtable(L);
    lua_setiuservalue(L, -2, 2);
    rotmos_server_config_init(&config);
    if (!config_parse(L, 1, &config, &s->pending_listeners, &error)) {
        free(s->pending_listeners);
        s->pending_listeners = NULL;
        return rl_error(L, &error);
    } /* No native object exists during Lua conversion. */
    config.callbacks.on_process_start = on_process_start;
    config.callbacks.on_parent_prepare = on_parent_prepare;
    config.callbacks.on_parent_ready = on_parent_ready;
    config.callbacks.on_parent_stopping = on_parent_stopping;
    config.callbacks.on_parent_stopped = on_parent_stopped;
    config.callbacks.on_worker_start = on_worker_start;
    config.callbacks.on_worker_stopping = on_worker_stopping;
    config.callbacks.on_worker_exit = on_worker_exit;
    config.callbacks.on_request_start = rl_request_start;
    config.callbacks.on_request_close = rl_request_close;
    config.callbacks.userdata = s;
    status = rotmos_server_new(&config, &s->native, &error);
    free(s->pending_listeners);
    s->pending_listeners = NULL;
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    return 1;
}
static int l_run(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    rotmos_error error;
    rotmos_run_result result;
    rotmos_status status;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_STATE, "server is closed"), &error));
    if (s->root != LUA_NOREF)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_STATE, "server is already running"),
                &error));
    lua_pushvalue(L, 1);
    s->root = luaL_ref(L, LUA_REGISTRYINDEX);
    s->L = L;
    rotmos_error_clear(&s->callback_error);
    status = rotmos_server_run(s->native, &result, &error);
    luaL_unref(L, LUA_REGISTRYINDEX, s->root);
    s->root = LUA_NOREF;
    if (status == ROTMOS_OK && s->callback_error.status != ROTMOS_OK) {
        error = s->callback_error;
        status = error.status;
    }
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, result.reason);
    lua_setfield(L, -2, "reason");
    lua_pushboolean(L, result.escalated);
    lua_setfield(L, -2, "escalated");
    lua_pushinteger(L, result.signal_number);
    lua_setfield(L, -2, "signal_number");
    return 1;
}
static int l_stop(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    rotmos_error error;
    rotmos_status status;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_STATE, "server is closed"), &error));
    status = rotmos_server_request_stop(s->native, &error);
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int l_validate(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    rotmos_error e;
    if (s == NULL || s->native == NULL)
        return rl_error(L, (rl_fail(&e, ROTMOS_STATE, "server is closed"), &e));
    if (rotmos_server_validate(s->native, &e) != ROTMOS_OK)
        return rl_error(L, &e);
    lua_pushboolean(L, 1);
    return 1;
}
static int l_listener(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    rotmos_error e;
    rotmos_bound_listener b;
    size_t i = 1;
    if (s == NULL || s->native == NULL ||
        (!lua_isnoneornil(L, 2) && (!rl_size(L, 2, &i) || i == 0)))
        return rl_error(L,
                        (rl_fail(&e, ROTMOS_INVALID,
                                 "server and positive listener index required"),
                         &e));
    if (rotmos_server_listener_at(s->native, i - 1, &b, &e) != ROTMOS_OK)
        return rl_error(L, &e);
    lua_createtable(L, 0, 4);
    lua_pushlstring(L, b.address.data, b.address.length);
    lua_setfield(L, -2, "address");
    lua_pushinteger(L, b.port);
    lua_setfield(L, -2, "port");
    lua_pushboolean(L, b.tls);
    lua_setfield(L, -2, "tls");
    return 1;
}
static int l_route_count(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    rotmos_error e;
    if (s == NULL || s->native == NULL)
        return rl_error(L, (rl_fail(&e, ROTMOS_STATE, "server is closed"), &e));
    rl_push_size(L, rotmos_router_route_count(rotmos_server_router(s->native)));
    return 1;
}
static int method_value(lua_State *L, int index, rotmos_http_method *out)
{
    static const char *names[] = {"ANY",      "GET",    "POST", "PUT",
                                  "PATCH",    "DELETE", "HEAD", "OPTIONS",
                                  "PROPFIND", "MKCOL",  "COPY", "MOVE"};
    const char *text;
    size_t i, j, length, value;
    if (lua_isinteger(L, index)) {
        if (!rl_size(L, index, &value) || value > ROTMOS_HTTP_MOVE)
            return 0;
        *out = (rotmos_http_method)value;
        return 1;
    }
    if (lua_type(L, index) != LUA_TSTRING)
        return 0;
    text = lua_tolstring(L, index, &length);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strlen(names[i]) != length)
            continue;
        for (j = 0; j < length; ++j) {
            int c = (unsigned char)text[j];
            if (c >= 'a' && c <= 'z')
                c -= 'a' - 'A';
            if (c != names[i][j])
                break;
        }
        if (j == length) {
            *out = (rotmos_http_method)i;
            return 1;
        }
    }
    return 0;
}
static int route_methods(lua_State *L, int index, rotmos_route_config *config)
{
    size_t i, n;
    rotmos_http_method method;
    config->method = ROTMOS_HTTP_GET;
    rl_field(L, index, "methods");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        rl_field(L, index, "method");
        if (!lua_isnil(L, -1) && !method_value(L, -1, &config->method))
            return 0;
    } else if (!lua_istable(L, -1)) {
        if (!method_value(L, -1, &config->method))
            return 0;
    } else {
        n = lua_rawlen(L, -1);
        if (n == 0)
            return 0;
        config->method = ROTMOS_HTTP_ANY;
        for (i = 0; i < n; ++i) {
            lua_rawgeti(L, -1, (lua_Integer)i + 1);
            if (!method_value(L, -1, &method))
                return 0;
            config->methods |= method == ROTMOS_HTTP_ANY
                                   ? ROTMOS_HTTP_METHODS_ALL
                                   : ROTMOS_HTTP_METHOD_MASK(method);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return 1;
}
static int rule(lua_State *L, int group)
{
    struct rl_server *s = rl_server(L, 1);
    struct rl_rule *parent = NULL, *r;
    rotmos_error e;
    rotmos_status status;
    rotmos_route_config config;
    rotmos_route_group_config gc;
    size_t body = 0, max = 0;
    const char *kind = NULL;
    int root, index;
    static const char *const route_fields[] = {
        "path",    "path_kind", "method", "methods", "body",
        "handler", "before",    "after",  "auth"};
    static const char *const group_fields[] = {"prefix", "before", "after",
                                               "auth"};
    static const char *const hooks[] = {"before", "after"};
    size_t i;
    if (s == NULL) {
        parent = luaL_testudata(L, 1, RL_GROUP);
        if (parent != NULL)
            s = parent->server;
    }
    if (s == NULL || s->native == NULL || !lua_istable(L, 2))
        return rl_error(
            L, (rl_fail(&e, ROTMOS_STATE,
                        "active server/group and declaration required"),
                &e));
    if (group && parent != NULL)
        return rl_error(L, (rl_fail(&e, ROTMOS_NOT_SUPPORTED,
                                    "nested groups are not connected yet"),
                            &e));
    if (!rl_fields(L, 2, group ? group_fields : route_fields,
                   group ? sizeof(group_fields) / sizeof(group_fields[0])
                         : sizeof(route_fields) / sizeof(route_fields[0]),
                   &e))
        return rl_error(L, &e);
    rl_field(L, 2, "auth");
    if (!lua_isnil(L, -1))
        return rl_error(L, (rl_fail(&e, ROTMOS_NOT_SUPPORTED,
                                    "Lua auth providers are not connected yet"),
                            &e));
    lua_pop(L, 1);
    for (i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        rl_field(L, 2, hooks[i]);
        if (!lua_isnil(L, -1) && !lua_isfunction(L, -1))
            return rl_error(L, (rl_fail(&e, ROTMOS_INVALID,
                                        "route hooks must be functions"),
                                &e));
        lua_pop(L, 1);
    }
    r = lua_newuserdatauv(L, sizeof(*r), 2);
    memset(r, 0, sizeof(*r));
    r->root = LUA_NOREF;
    r->server = s;
    r->is_group = group;
    r->group = parent != NULL ? parent->group : NULL;
    index = lua_absindex(L, -1);
    luaL_setmetatable(L, group ? RL_GROUP : RL_RULE);
    rl_table_copy(L, 2);
    lua_setiuservalue(L, index, 1);
    if (parent != NULL)
        lua_getiuservalue(L, 1, 2);
    else
        lua_pushvalue(L, 1);
    lua_pushvalue(L, -1);
    lua_setiuservalue(L, index, 2);
    lua_getiuservalue(L, -1, 2);
    root = lua_absindex(L, -1);
    if (group) {
        rotmos_route_group_config_init(&gc);
        if (!rl_field_string(L, 2, "prefix", &gc.prefix))
            goto invalid;
        gc.hooks.before = rl_before;
        gc.hooks.after = rl_after;
        gc.hooks.userdata = r;
        lua_pushvalue(L, index);
        r->root = luaL_ref(L, root);
        status = rotmos_router_group(rotmos_server_router(s->native), &gc,
                                     &r->group, &e);
    } else {
        rotmos_route_config_init(&config);
        if (!rl_field_string(L, 2, "path", &config.path) ||
            config.path == NULL || !rl_field_string(L, 2, "path_kind", &kind) ||
            !route_methods(L, 2, &config))
            goto invalid;
        if (config.path[0] == '^')
            config.path_kind = ROTMOS_PATH_REGEX;
        else if (strchr(config.path, ':') != NULL)
            config.path_kind = ROTMOS_PATH_PARAMS;
        if (kind != NULL) {
            if (!strcmp(kind, "params"))
                config.path_kind = ROTMOS_PATH_PARAMS;
            else if (!strcmp(kind, "regex"))
                config.path_kind = ROTMOS_PATH_REGEX;
            else if (!strcmp(kind, "literal"))
                config.path_kind = ROTMOS_PATH_LITERAL;
            else
                goto invalid;
        }
        config.handler = rl_handler;
        config.userdata = r;
        config.hooks.before = rl_before;
        config.hooks.after = rl_after;
        config.hooks.userdata = r;
        rl_field(L, 2, "handler");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            goto invalid;
        }
        lua_pop(L, 1);
        rl_field(L, 2, "body");
        if (!lua_isnil(L, -1)) {
            static const char *const fields[] = {"mode", "max_bytes"};
            if (lua_istable(L, -1) && !rl_fields(L, -1, fields, 2, &e))
                goto failed;
            if (!lua_istable(L, -1) || !rl_field_size(L, -1, "mode", &body) ||
                !rl_field_size(L, -1, "max_bytes", &max))
                goto invalid;
            if (body != ROTMOS_BODY_NONE && body != ROTMOS_BODY_BUFFERED) {
                rl_fail(&e, ROTMOS_NOT_SUPPORTED,
                        "Lua route currently requires NONE or BUFFERED body "
                        "policy");
                goto failed;
            }
            config.body.mode = (rotmos_body_mode)body;
            if (max != 0) {
                rotmos_u64_from_size(max, &config.body.max_bytes, NULL);
                config.body.memory_buffer_limit_bytes = max;
            }
        }
        lua_pop(L, 1);
        lua_pushvalue(L, index);
        r->root = luaL_ref(L, root);
        status =
            parent != NULL
                ? rotmos_route_group_route(parent->group, &config, NULL, &e)
                : rotmos_router_route(rotmos_server_router(s->native), &config,
                                      NULL, &e);
    }
    if (status != ROTMOS_OK)
        goto failed;
    lua_settop(L, index);
    return 1;
invalid:
    rl_fail(&e, ROTMOS_INVALID, "invalid route/group declaration");
failed:
    if (r->root != LUA_NOREF)
        luaL_unref(L, root, r->root);
    return rl_error(L, &e);
}
static int l_route(lua_State *L) { return rule(L, 0); }
static int l_group(lua_State *L) { return rule(L, 1); }
void rl_servers_open(lua_State *L)
{
    static const luaL_Reg methods[] = {{"run", l_run},
                                       {"stop", l_stop},
                                       {"close", l_close},
                                       {"validate", l_validate},
                                       {"listener", l_listener},
                                       {"route", l_route},
                                       {"group", l_group},
                                       {"route_count", l_route_count},
                                       {NULL, NULL}};
    luaL_newmetatable(L, RL_SERVER);
    lua_pushcfunction(L, l_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pop(L, 1);
    luaL_newmetatable(L, RL_GROUP);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_route);
    lua_setfield(L, -2, "route");
    lua_pop(L, 1);
    luaL_newmetatable(L, RL_RULE);
    lua_pop(L, 1);
    lua_pushcfunction(L, l_new);
    lua_setfield(L, -2, "server");
}
