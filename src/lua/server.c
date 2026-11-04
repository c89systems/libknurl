#include "binding.h"
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
static int l_new(lua_State *L)
{
    struct rl_server *s;
    rotmos_server_config config;
    rotmos_error error;
    rotmos_status status;
    int index;
    if (!lua_istable(L, 1))
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "server configuration table required"),
                            &error));
    s = lua_newuserdatauv(L, sizeof(*s), 2);
    index = lua_absindex(L, -1);
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
    if (!rl_config_parse(L, 1, &config, &error)) {
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
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushvalue(L, index);
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
static int l_effective(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    rotmos_server_effective effective;
    rotmos_error error;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_STATE, "server is closed"), &error));
    if (rotmos_server_effective_config(s->native, &effective, &error) !=
        ROTMOS_OK)
        return rl_error(L, &error);
    rl_config_push(L, &effective);
    return 1;
}
static int l_serving(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    rotmos_error error;
    if (s == NULL || s->native == NULL || !lua_isboolean(L, 2))
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "server and serving boolean required"),
                            &error));
    if (rotmos_server_set_serving(s->native, lua_toboolean(L, 2), &error) !=
        ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int l_state(lua_State *L)
{
    static const char *states[] = {"declared", "starting", "ready_paused",
                                   "serving",  "stopping", "stopped",
                                   "failed"};
    struct rl_server *s = rl_server(L, 1);
    if (s == NULL || s->native == NULL)
        lua_pushstring(L, "closed");
    else
        lua_pushstring(L, states[rotmos_server_get_state(s->native)]);
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
static int route_methods(lua_State *L, int index, rotmos_route_config *config,
                         int upload)
{
    size_t i, n;
    rotmos_http_method method;
    config->method = upload ? ROTMOS_HTTP_POST : ROTMOS_HTTP_GET;
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
static int body_policy(lua_State *L, int index, rotmos_body_policy *policy,
                       int upload, rotmos_error *error)
{
    size_t mode = (size_t)policy->mode;
    static const char *const fields[] = {"mode", "max_bytes",
                                         "memory_buffer_limit_bytes",
                                         "disk_spool_disabled"};
    index = lua_absindex(L, index);
    if (!lua_istable(L, index))
        goto invalid;
    if (!rl_fields(L, index, fields, sizeof(fields) / sizeof(fields[0]), error))
        return 0;
    if (!rl_field_size(L, index, "mode", &mode) ||
        !rl_field_size(L, index, "memory_buffer_limit_bytes",
                       &policy->memory_buffer_limit_bytes))
        goto invalid;
    if ((!upload && mode != ROTMOS_BODY_NONE && mode != ROTMOS_BODY_BUFFERED &&
         mode != ROTMOS_BODY_MATERIALIZED) ||
        (upload && mode != ROTMOS_BODY_STREAMING_UPLOAD &&
         mode != ROTMOS_BODY_STREAM))
        goto invalid;
    policy->mode = (rotmos_body_mode)mode;
    rl_field(L, index, "max_bytes");
    if (!lua_isnil(L, -1) && !rl_u64(L, -1, &policy->max_bytes))
        goto invalid;
    lua_pop(L, 1);
    rl_field(L, index, "disk_spool_disabled");
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1))
            goto invalid;
        policy->disk_spool_disabled = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return 1;
invalid:
    rl_fail(error, ROTMOS_INVALID, "invalid route body policy");
    return 0;
}
static int rule(lua_State *L, int kind_id)
{
    int group = kind_id == 1, upload = kind_id == 2;
    struct rl_server *s = rl_server(L, 1);
    struct rl_rule *parent = NULL, *r;
    rotmos_error e;
    rotmos_status status;
    rotmos_route_config config;
    rotmos_route_group_config gc;
    rotmos_upload_stream_config uc;
    const char *kind = NULL;
    int root, index;
    static const char *const route_fields[] = {
        "path",    "path_kind", "method", "methods", "body",
        "handler", "before",    "after",  "auth"};
    static const char *const group_fields[] = {"prefix", "before", "after",
                                               "auth"};
    static const char *const upload_fields[] = {
        "path",        "path_kind", "method",       "methods",       "body",
        "before",      "after",     "auth",         "open",          "on_chunk",
        "on_complete", "close",     "buffer_bytes", "max_body_bytes"};
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
    if (!rl_fields(L, 2,
                   group    ? group_fields
                   : upload ? upload_fields
                            : route_fields,
                   group    ? sizeof(group_fields) / sizeof(group_fields[0])
                   : upload ? sizeof(upload_fields) / sizeof(upload_fields[0])
                            : sizeof(route_fields) / sizeof(route_fields[0]),
                   &e))
        return rl_error(L, &e);
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
    r->is_upload = upload;
    r->buffer_bytes = 262144UL;
    r->group = parent != NULL ? parent->group : NULL;
    index = lua_absindex(L, -1);
    luaL_setmetatable(L, group ? RL_GROUP : RL_RULE);
    rl_table_copy(L, 2);
    if (!rl_auth_prepare(L, -1, &e))
        return rl_error(L, &e);
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
        gc.hooks.auth = rl_auth;
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
            !route_methods(L, 2, &config, upload))
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
        config.hooks.auth = rl_auth;
        config.hooks.before = rl_before;
        config.hooks.after = rl_after;
        config.hooks.userdata = r;
        if (!upload) {
            rl_field(L, 2, "handler");
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                goto invalid;
            }
            lua_pop(L, 1);
        } else {
            rotmos_body_policy_init_upload(&config.body);
            if (!rl_upload_prepare(L, 2, &e))
                goto failed;
            if (!rl_field_size(L, 2, "buffer_bytes", &r->buffer_bytes))
                goto invalid;
            if (r->buffer_bytes == 0)
                r->buffer_bytes = 262144UL;
            rl_field(L, 2, "max_body_bytes");
            if (!lua_isnil(L, -1) && !rl_u64(L, -1, &config.body.max_bytes))
                goto invalid;
            lua_pop(L, 1);
        }
        rl_field(L, 2, "body");
        if (!lua_isnil(L, -1) && !body_policy(L, -1, &config.body, upload, &e))
            goto failed;
        lua_pop(L, 1);
        lua_pushvalue(L, index);
        r->root = luaL_ref(L, root);
        if (upload) {
            rl_upload_config(&uc, &config, r);
            status = parent != NULL
                         ? rotmos_route_group_upload_stream(parent->group, &uc,
                                                            NULL, &e)
                         : rotmos_router_upload_stream(
                               rotmos_server_router(s->native), &uc, NULL, &e);
        } else
            status =
                parent != NULL
                    ? rotmos_route_group_route(parent->group, &config, NULL, &e)
                    : rotmos_router_route(rotmos_server_router(s->native),
                                          &config, NULL, &e);
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
static int l_upload(lua_State *L) { return rule(L, 2); }
void rl_servers_open(lua_State *L)
{
    static const luaL_Reg methods[] = {{"run", l_run},
                                       {"stop", l_stop},
                                       {"close", l_close},
                                       {"validate", l_validate},
                                       {"effective_config", l_effective},
                                       {"set_serving", l_serving},
                                       {"state", l_state},
                                       {"listener", l_listener},
                                       {"route", l_route},
                                       {"upload", l_upload},
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
    lua_pushcfunction(L, l_upload);
    lua_setfield(L, -2, "upload");
    lua_pop(L, 1);
    luaL_newmetatable(L, RL_RULE);
    lua_pop(L, 1);
    lua_pushcfunction(L, l_new);
    lua_setfield(L, -2, "server");
}
