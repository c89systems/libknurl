#include "binding.h"
struct rl_server *rl_server(lua_State *L, int index)
{
    return luaL_testudata(L, index, RL_SERVER);
}
static int l_close(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    knurl_error error;
    knurl_status status;
    if (s == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_INVALID, "server required"), &error));
    if (s->root != LUA_NOREF)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "server is active"), &error));
    status = knurl_server_destroy(s->native, &error);
    if (status != KNURL_OK)
        return rl_error(L, &error);
    s->native = NULL;
    rl_logging_cleanup(s);
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 1);
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 2);
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 3);
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 4);
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 5);
    lua_pushboolean(L, 1);
    return 1;
}
static int l_gc(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    if (s != NULL) {
        if (s->native == NULL ||
            knurl_server_destroy(s->native, NULL) == KNURL_OK) {
            s->native = NULL;
            rl_logging_cleanup(s);
        }
    }
    return 0;
}
void rl_process_push(lua_State *L, const knurl_process_context *p)
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
    if (!strcmp(c->name, "on_process_start"))
        rl_loops_process_start(L, c->server);
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
        c->status = rl_fail(c->error, KNURL_INVALID,
                            "lifecycle callback must be a function");
        return 0;
    }
    lua_pushvalue(L, root);
    rl_process_push(L, c->process);
    lua_call(L, 2, 2);
    if (lua_isboolean(L, -2) && !lua_toboolean(L, -2))
        c->status = rl_fail(c->error, KNURL_STATE,
                            "lifecycle callback rejected operation");
    else if (!lua_isnil(L, -1))
        c->status = rl_callback_error(L, -1, c->error,
                                      "lifecycle callback returned an error");
    return 0;
}
static knurl_status process_call(knurl_server *native,
                                 const knurl_process_context *p, void *data,
                                 knurl_error *error, const char *name)
{
    struct rl_server *s = data;
    struct rl_call c;
    knurl_status status;
    (void)native;
    memset(&c, 0, sizeof(c));
    c.server = s;
    c.process = p;
    c.name = name;
    c.error = error;
    status = rl_protect(s->L, process_trampoline, &c, error);
    return status == KNURL_OK ? c.status : status;
}
static knurl_status on_process_start(knurl_server *server,
                                     const knurl_process_context *process,
                                     void *data, knurl_error *error)
{
    struct rl_server *owner = data;
    owner->process_id = process->process_id;
    return process_call(server, process, data, error, "on_process_start");
}
static knurl_status on_parent_prepare(knurl_server *server,
                                      const knurl_process_context *process,
                                      void *data, knurl_error *error)
{
    return process_call(server, process, data, error, "on_parent_prepare");
}
static knurl_status on_parent_ready(knurl_server *server,
                                    const knurl_process_context *process,
                                    void *data, knurl_error *error)
{
    return process_call(server, process, data, error, "on_parent_ready");
}
static knurl_status on_parent_stopping(knurl_server *server,
                                       const knurl_process_context *process,
                                       void *data, knurl_error *error)
{
    return process_call(server, process, data, error, "on_parent_stopping");
}
static knurl_status on_parent_stopped(knurl_server *server,
                                      const knurl_process_context *process,
                                      void *data, knurl_error *error)
{
    return process_call(server, process, data, error, "on_parent_stopped");
}
static knurl_status on_worker_start(knurl_server *server,
                                    const knurl_process_context *process,
                                    void *data, knurl_error *error)
{
    return process_call(server, process, data, error, "on_worker_start");
}
static knurl_status on_worker_stopping(knurl_server *server,
                                       const knurl_process_context *process,
                                       void *data, knurl_error *error)
{
    return process_call(server, process, data, error, "on_worker_stopping");
}
static int on_worker_exit(knurl_server *native, const knurl_process_context *p,
                          void *data)
{
    struct rl_server *s = data;
    knurl_error error;
    knurl_status status =
        process_call(native, p, data, &error, "on_worker_exit");
    if (status != KNURL_OK && s->callback_error.status == KNURL_OK)
        s->callback_error = error;
    return status != KNURL_OK || s->callback_error.status != KNURL_OK;
}
static int l_new(lua_State *L)
{
    struct rl_server *s;
    knurl_server_config config;
    knurl_error error;
    knurl_status status;
    int index;
    if (!lua_istable(L, 1))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "server configuration table required"),
                            &error));
    s = lua_newuserdatauv(L, sizeof(*s), 5);
    index = lua_absindex(L, -1);
    memset(s, 0, sizeof(*s));
    s->L = L;
    s->root = LUA_NOREF;
    rl_logging_init(s);
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
    lua_newtable(L);
    lua_setiuservalue(L, -2, 3);
    lua_newtable(L);
    lua_setiuservalue(L, -2, 4);
    lua_newtable(L);
    lua_setiuservalue(L, -2, 5);
    knurl_server_config_init(&config);
    if (!rl_config_parse(L, 1, index, &config, &error)) {
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
    if (!rl_policy_config(L, index, &config, s, &error))
        return rl_error(L, &error);
    status = knurl_server_new(&config, &s->native, &error);
    if (status != KNURL_OK)
        return rl_error(L, &error);
    if (!rl_logging_config(L, 1, s, &error))
        return rl_error(L, &error);
    lua_getiuservalue(L, index, 1);
    lua_pushnil(L);
    lua_setfield(L, -2, "logger");
    lua_pushnil(L);
    lua_setfield(L, -2, "access_log");
    lua_pop(L, 1);
    lua_pushvalue(L, index);
    return 1;
}
static int l_run(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    knurl_error error;
    knurl_run_result result;
    knurl_status status;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "server is closed"), &error));
    if (s->root != LUA_NOREF)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "server is active"), &error));
    lua_pushvalue(L, 1);
    s->root = luaL_ref(L, LUA_REGISTRYINDEX);
    s->L = L;
    ++s->run_epoch;
    knurl_error_clear(&s->callback_error);
    status = knurl_server_run(s->native, &result, &error);
    luaL_unref(L, LUA_REGISTRYINDEX, s->root);
    s->root = LUA_NOREF;
    if (status == KNURL_OK && s->callback_error.status != KNURL_OK) {
        error = s->callback_error;
        status = error.status;
    }
    if (status != KNURL_OK)
        return rl_error(L, &error);
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, result.reason);
    lua_setfield(L, -2, "reason");
    lua_pushboolean(L, result.escalated);
    lua_setfield(L, -2, "escalated");
    lua_pushinteger(L, result.signal_number);
    lua_setfield(L, -2, "signal_number");
    lua_pushboolean(L, result.has_failure);
    lua_setfield(L, -2, "has_failure");
    rl_push_u64(L, result.recovered_failures);
    lua_setfield(L, -2, "recovered_failures");
    if (result.has_failure) {
        rl_worker_event_push(L, &result.last_failure);
        lua_setfield(L, -2, "last_failure");
    }
    return 1;
}
static int l_stop(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    knurl_error error;
    knurl_status status;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "server is closed"), &error));
    status = knurl_server_request_stop(s->native, &error);
    if (status != KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int l_notify_signal(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    knurl_error error;
    size_t number;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "server is closed"), &error));
    if (!rl_size(L, 2, &number) || number > INT_MAX)
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "signal number must fit a nonnegative int"),
                            &error));
    s->native->notify_signal(s->native, (int)number);
    lua_pushboolean(L, 1);
    return 1;
}
static int l_kore_revision(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    knurl_error error;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "server is closed"), &error));
    lua_pushstring(L, s->native->kore_revision(s->native));
    return 1;
}
static int validate(lua_State *L, int resources)
{
    struct rl_server *s = rl_server(L, 1);
    knurl_error e;
    knurl_status status;
    if (s == NULL || s->native == NULL)
        return rl_error(L, (rl_fail(&e, KNURL_STATE, "server is closed"), &e));
    if (s->root != LUA_NOREF)
        return rl_error(L, (rl_fail(&e, KNURL_STATE, "server is active"), &e));
    lua_pushvalue(L, 1);
    s->root = luaL_ref(L, LUA_REGISTRYINDEX);
    s->L = L;
    knurl_error_clear(&s->callback_error);
    status = resources ? s->native->preflight(s->native, &e)
                       : s->native->validate(s->native, &e);
    luaL_unref(L, LUA_REGISTRYINDEX, s->root);
    s->root = LUA_NOREF;
    if (status == KNURL_OK && s->callback_error.status != KNURL_OK) {
        e = s->callback_error;
        status = e.status;
    }
    if (status != KNURL_OK)
        return rl_error(L, &e);
    lua_pushboolean(L, 1);
    return 1;
}
static int l_validate(lua_State *L) { return validate(L, 0); }
static int l_preflight(lua_State *L) { return validate(L, 1); }
static int l_content_type(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    const char *path = NULL;
    size_t length = 0;
    knurl_error error;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "server is closed"), &error));
    if (lua_type(L, 2) == LUA_TSTRING)
        path = lua_tolstring(L, 2, &length);
    if (path == NULL || memchr(path, 0, length) != NULL)
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "content_type needs a filename string"),
                            &error));
    lua_pushstring(L, knurl_server_content_type(s->native, path));
    return 1;
}
static int l_effective(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    knurl_server_effective effective;
    knurl_error error;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "server is closed"), &error));
    if (knurl_server_effective_config(s->native, &effective, &error) !=
        KNURL_OK)
        return rl_error(L, &error);
    rl_config_push(L, &effective);
    return 1;
}
static int l_serving(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    knurl_error error;
    if (s == NULL || s->native == NULL || !lua_isboolean(L, 2))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "server and serving boolean required"),
                            &error));
    if (knurl_server_set_serving(s->native, lua_toboolean(L, 2), &error) !=
        KNURL_OK)
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
        lua_pushstring(L, states[knurl_server_get_state(s->native)]);
    return 1;
}
static int l_listener(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    knurl_error e;
    knurl_bound_listener b;
    size_t i = 1;
    if (s == NULL || s->native == NULL ||
        (!lua_isnoneornil(L, 2) && (!rl_size(L, 2, &i) || i == 0)))
        return rl_error(L,
                        (rl_fail(&e, KNURL_INVALID,
                                 "server and positive listener index required"),
                         &e));
    if (knurl_server_listener_at(s->native, i - 1, &b, &e) != KNURL_OK)
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
    knurl_error e;
    if (s == NULL || s->native == NULL)
        return rl_error(L, (rl_fail(&e, KNURL_STATE, "server is closed"), &e));
    rl_push_size(L, knurl_router_route_count(knurl_server_router(s->native)));
    return 1;
}
int rl_method_value(lua_State *L, int index, knurl_http_method *out)
{
    static const char *names[] = {"ANY",      "GET",    "POST", "PUT",
                                  "PATCH",    "DELETE", "HEAD", "OPTIONS",
                                  "PROPFIND", "MKCOL",  "COPY", "MOVE"};
    const char *text;
    size_t i, j, length, value;
    if (lua_type(L, index) == LUA_TNUMBER) {
        if (!rl_size(L, index, &value) || value > KNURL_HTTP_MOVE)
            return 0;
        *out = (knurl_http_method)value;
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
            *out = (knurl_http_method)i;
            return 1;
        }
    }
    return 0;
}
int rl_methods(lua_State *L, int index, knurl_http_methods defaults,
               knurl_http_methods *out)
{
    size_t i, n;
    knurl_http_method method;
    index = lua_absindex(L, index);
    *out = defaults;
    rl_field(L, index, "methods");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        rl_field(L, index, "method");
    }
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    *out = 0;
    if (!lua_istable(L, -1)) {
        if (!rl_method_value(L, -1, &method))
            return 0;
        *out = method == KNURL_HTTP_ANY ? KNURL_HTTP_METHODS_ALL
                                        : KNURL_HTTP_METHOD_MASK(method);
    } else {
        n = lua_rawlen(L, -1);
        if (n == 0)
            return 0;
        for (i = 0; i < n; ++i) {
            lua_rawgeti(L, -1, (lua_Integer)i + 1);
            if (!rl_method_value(L, -1, &method))
                return 0;
            *out |= method == KNURL_HTTP_ANY ? KNURL_HTTP_METHODS_ALL
                                             : KNURL_HTTP_METHOD_MASK(method);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return 1;
}
int rl_body_policy(lua_State *L, int index, knurl_body_policy *policy,
                   int upload, knurl_error *error)
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
    if ((!upload && mode != KNURL_BODY_NONE && mode != KNURL_BODY_BUFFERED &&
         mode != KNURL_BODY_MATERIALIZED) ||
        (upload && mode != KNURL_BODY_STREAMING_UPLOAD &&
         mode != KNURL_BODY_STREAM))
        goto invalid;
    policy->mode = (knurl_body_mode)mode;
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
    rl_fail(error, KNURL_INVALID, "invalid route body policy");
    return 0;
}
int rl_route_path(lua_State *L, int index, knurl_route_config *config)
{
    const char *kind = NULL;
    if (!rl_field_string(L, index, "path", &config->path) ||
        config->path == NULL || !rl_field_string(L, index, "path_kind", &kind))
        return 0;
    if (config->path[0] == '^')
        config->path_kind = KNURL_PATH_REGEX;
    else if (strchr(config->path, ':') != NULL)
        config->path_kind = KNURL_PATH_PARAMS;
    if (kind == NULL)
        return 1;
    if (!strcmp(kind, "params"))
        config->path_kind = KNURL_PATH_PARAMS;
    else if (!strcmp(kind, "regex"))
        config->path_kind = KNURL_PATH_REGEX;
    else if (!strcmp(kind, "literal"))
        config->path_kind = KNURL_PATH_LITERAL;
    else
        return 0;
    return 1;
}
struct rl_rule *rl_rule_new(lua_State *L, knurl_error *error)
{
    struct rl_server *server = rl_server(L, 1);
    struct rl_rule *parent = NULL, *rule;
    static const char *const hooks[] = {"before", "after"};
    size_t i;
    int index;
    if (server == NULL) {
        parent = luaL_testudata(L, 1, RL_GROUP);
        if (parent != NULL)
            server = parent->server;
    }
    if (server == NULL || server->native == NULL || !lua_istable(L, 2)) {
        rl_fail(error, KNURL_STATE,
                "active server/group and declaration required");
        return NULL;
    }
    for (i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        rl_field(L, 2, hooks[i]);
        if (!lua_isnil(L, -1) && !lua_isfunction(L, -1)) {
            rl_fail(error, KNURL_INVALID, "route hooks must be functions");
            return NULL;
        }
        lua_pop(L, 1);
    }
    rule = lua_newuserdatauv(L, sizeof(*rule), 3);
    memset(rule, 0, sizeof(*rule));
    rule->root = LUA_NOREF;
    rule->server = server;
    rule->group = parent == NULL ? NULL : parent->group;
    index = lua_absindex(L, -1);
    luaL_setmetatable(L, RL_RULE);
    rl_table_copy(L, 2);
    if (!rl_auth_prepare(L, -1, error))
        return NULL;
    lua_setiuservalue(L, index, 1);
    if (parent == NULL)
        lua_pushvalue(L, 1);
    else
        lua_getiuservalue(L, 1, 2);
    lua_setiuservalue(L, index, 2);
    return rule;
}
int rl_rule_root(lua_State *L, int index, struct rl_rule *rule)
{
    int root;
    index = lua_absindex(L, index);
    lua_getiuservalue(L, index, 2);
    lua_getiuservalue(L, -1, 2);
    root = lua_absindex(L, -1);
    lua_pushvalue(L, index);
    rule->root = luaL_ref(L, root);
    return root;
}
int rl_rule_finish(lua_State *L, int index, int root, struct rl_rule *rule,
                   knurl_status status, const knurl_error *error)
{
    if (status != KNURL_OK) {
        if (rule->root != LUA_NOREF)
            luaL_unref(L, root, rule->root);
        return rl_error(L, error);
    }
    lua_settop(L, index);
    return 1;
}
void rl_rule_hooks(knurl_route_hooks *hooks, struct rl_rule *rule)
{
    memset(hooks, 0, sizeof(*hooks));
    hooks->auth = rl_auth;
    hooks->before = rl_before;
    hooks->after = rl_after;
    hooks->userdata = rule;
}
void rl_rule_options(lua_State *L, struct rl_rule *rule)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, rule->server->root);
    lua_getiuservalue(L, -1, 2);
    lua_rawgeti(L, -1, rule->root);
    lua_getiuservalue(L, -1, 1);
}
static int rule(lua_State *L, int kind_id)
{
    int group = kind_id == 1, upload = kind_id == 2, codec = kind_id >= 3;
    struct rl_server *s = rl_server(L, 1);
    struct rl_rule *parent = NULL, *r;
    knurl_error e;
    knurl_status status;
    knurl_route_config config;
    knurl_route_group_config gc;
    knurl_upload_stream_config uc;
    int root, index;
    static const char *const route_fields[] = {
        "path",    "path_kind", "method", "methods", "body",
        "handler", "before",    "after",  "auth"};
    static const char *const group_fields[] = {"prefix", "before", "after",
                                               "auth"};
    static const char *const codec_fields[] = {
        "path",         "path_kind",       "method",        "methods",
        "body",         "handler",         "before",        "after",
        "auth",         "input_schema",    "output_schema", "xml",
        "buffer_bytes", "max_output_bytes"};
    static const char *const upload_fields[] = {
        "path",        "path_kind", "method",       "methods",       "body",
        "before",      "after",     "auth",         "open",          "on_chunk",
        "on_complete", "close",     "buffer_bytes", "max_body_bytes"};
    if (s == NULL) {
        parent = luaL_testudata(L, 1, RL_GROUP);
        if (parent != NULL)
            s = parent->server;
    }
    if (s == NULL || s->native == NULL || !lua_istable(L, 2))
        return rl_error(
            L, (rl_fail(&e, KNURL_STATE,
                        "active server/group and declaration required"),
                &e));
    if (group && parent != NULL)
        return rl_error(L, (rl_fail(&e, KNURL_NOT_SUPPORTED,
                                    "nested groups are not connected yet"),
                            &e));
    if (!rl_fields(L, 2,
                   group    ? group_fields
                   : codec  ? codec_fields
                   : upload ? upload_fields
                            : route_fields,
                   group    ? sizeof(group_fields) / sizeof(group_fields[0])
                   : codec  ? sizeof(codec_fields) / sizeof(codec_fields[0])
                   : upload ? sizeof(upload_fields) / sizeof(upload_fields[0])
                            : sizeof(route_fields) / sizeof(route_fields[0]),
                   &e))
        return rl_error(L, &e);
    r = rl_rule_new(L, &e);
    if (r == NULL)
        return rl_error(L, &e);
    r->is_group = group;
    r->is_upload = upload;
    r->buffer_bytes = 262144UL;
    index = lua_absindex(L, -1);
    if (group)
        luaL_setmetatable(L, RL_GROUP);
    lua_getiuservalue(L, index, 2);
    lua_getiuservalue(L, -1, 2);
    root = lua_absindex(L, -1);
    if (group) {
        knurl_route_group_config_init(&gc);
        if (!rl_field_string(L, 2, "prefix", &gc.prefix))
            goto invalid;
        gc.hooks.auth = rl_auth;
        gc.hooks.before = rl_before;
        gc.hooks.after = rl_after;
        gc.hooks.userdata = r;
        lua_pushvalue(L, index);
        r->root = luaL_ref(L, root);
        status = knurl_router_group(knurl_server_router(s->native), &gc,
                                    &r->group, &e);
    } else {
        knurl_route_config_init(&config);
        if (kind_id == 5)
            knurl_body_policy_init_upload(&config.body);
        else if (codec)
            knurl_body_policy_init_materialized(&config.body);
        if (!rl_route_path(L, 2, &config) ||
            !rl_methods(L, 2,
                        KNURL_HTTP_METHOD_MASK((upload || codec)
                                                   ? KNURL_HTTP_POST
                                                   : KNURL_HTTP_GET),
                        &config.methods))
            goto invalid;
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
            knurl_body_policy_init_upload(&config.body);
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
        if (!lua_isnil(L, -1) &&
            !rl_body_policy(L, -1, &config.body, upload || kind_id == 5, &e))
            goto failed;
        lua_pop(L, 1);
        if (codec) {
            rl_field(L, 2, "input_schema");
            if (kind_id != 5 && !lua_isnil(L, -1) &&
                config.body.mode != KNURL_BODY_BUFFERED &&
                config.body.mode != KNURL_BODY_MATERIALIZED)
                goto invalid;
            lua_pop(L, 1);
        }
        lua_pushvalue(L, index);
        r->root = luaL_ref(L, root);
        if (upload) {
            rl_upload_config(&uc, &config, r);
            status = parent != NULL
                         ? knurl_route_group_upload_stream(parent->group, &uc,
                                                           NULL, &e)
                         : knurl_router_upload_stream(
                               knurl_server_router(s->native), &uc, NULL, &e);
        } else if (codec)
            status =
                rl_codec_route_register(L, r, parent, &config, kind_id, &e);
        else
            status =
                parent != NULL
                    ? knurl_route_group_route(parent->group, &config, NULL, &e)
                    : knurl_router_route(knurl_server_router(s->native),
                                         &config, NULL, &e);
    }
    if (status != KNURL_OK)
        goto failed;
    lua_settop(L, index);
    return 1;
invalid:
    rl_fail(&e, KNURL_INVALID, "invalid route/group declaration");
failed:
    if (r->root != LUA_NOREF)
        luaL_unref(L, root, r->root);
    return rl_error(L, &e);
}
static int l_route(lua_State *L) { return rule(L, 0); }
static int l_group(lua_State *L) { return rule(L, 1); }
static int l_upload(lua_State *L) { return rule(L, 2); }
static int l_json_route(lua_State *L) { return rule(L, 3); }
static int l_json_typed_route(lua_State *L) { return rule(L, 4); }
static int l_xml_route(lua_State *L) { return rule(L, 5); }
void rl_servers_open(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"run", l_run},
        {"stop", l_stop},
        {"notify_signal", l_notify_signal},
        {"kore_revision", l_kore_revision},
        {"close", l_close},
        {"validate", l_validate},
        {"preflight", l_preflight},
        {"effective_config", l_effective},
        {"content_type", l_content_type},
        {"set_serving", l_serving},
        {"state", l_state},
        {"listener", l_listener},
        {"route", l_route},
        {"rest_route", rl_rest_route},
        {"json_route", l_json_route},
        {"json_typed_route", l_json_typed_route},
        {"xml_route", l_xml_route},
        {"upload", l_upload},
        {"upload_file", rl_upload_file},
        {"static_file", rl_static_file},
        {"static_directory", rl_static_directory},
        {"static_assets", rl_static_assets},
        {"webdav", rl_webdav},
        {"proxy", rl_proxy},
        {"websocket", rl_websocket},
        {"defer", rl_defer},
        {"timer", rl_timer},
        {"watch", rl_watch},
        {"waker", rl_waker},
        {"now", rl_now},
        {"group", l_group},
        {"route_count", l_route_count},
        {"openapi_doc", rl_openapi_doc},
        {"openapi", rl_openapi},
        {"control_interest", rl_control_interest},
        {"supervision_take", rl_supervision_take},
        {"tls_identity_state", rl_tls_identity_state},
        {"set_pslog", rl_set_pslog},
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
    lua_pushcfunction(L, rl_rest_route);
    lua_setfield(L, -2, "rest_route");
    lua_pushcfunction(L, l_json_route);
    lua_setfield(L, -2, "json_route");
    lua_pushcfunction(L, l_json_typed_route);
    lua_setfield(L, -2, "json_typed_route");
    lua_pushcfunction(L, l_xml_route);
    lua_setfield(L, -2, "xml_route");
    lua_pushcfunction(L, l_upload);
    lua_setfield(L, -2, "upload");
    lua_pushcfunction(L, rl_upload_file);
    lua_setfield(L, -2, "upload_file");
    lua_pushcfunction(L, rl_static_file);
    lua_setfield(L, -2, "static_file");
    lua_pushcfunction(L, rl_static_directory);
    lua_setfield(L, -2, "static_directory");
    lua_pushcfunction(L, rl_static_assets);
    lua_setfield(L, -2, "static_assets");
    lua_pushcfunction(L, rl_webdav);
    lua_setfield(L, -2, "webdav");
    lua_pop(L, 1);
    luaL_newmetatable(L, RL_RULE);
    lua_pop(L, 1);
    lua_pushcfunction(L, l_new);
    lua_setfield(L, -2, "server");
}
