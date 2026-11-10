#include "binding.h"
#define N(a) (sizeof(a) / sizeof((a)[0]))
/* Vectis's Lua proxy callbacks, using public borrowed views exclusively. */
struct proxy_view {
    const knurl_proxy_inbound *in;
    knurl_proxy_outbound *out;
    knurl_proxy_response *response;
    knurl_error failure;
    int active;
};
struct proxy_call {
    struct rl_rule *rule;
    const knurl_proxy_inbound *in;
    knurl_proxy_outbound *out;
    knurl_proxy_response *response;
    knurl_proxy_local_response *local;
    const knurl_error *cause;
    struct proxy_view *view;
    int phase, default_status, root;
    knurl_status status;
    knurl_error *error;
};
static const char *const method_names[] = {
    "ANY",  "GET",     "POST",     "PUT",   "PATCH", "DELETE",
    "HEAD", "OPTIONS", "PROPFIND", "MKCOL", "COPY",  "MOVE"};
enum edit {
    TARGET,
    METHOD,
    PATH,
    QUERY,
    HOST,
    ADD_HEADER,
    SET_HEADER,
    REMOVE_HEADER,
    RESPONSE_STATUS,
    RESPONSE_ADD,
    RESPONSE_SET,
    RESPONSE_REMOVE
};
static int string_value(lua_State *L, int index, const char **out)
{
    size_t length;
    if (lua_type(L, index) != LUA_TSTRING)
        return 0;
    *out = lua_tolstring(L, index, &length);
    return memchr(*out, 0, length) == NULL;
}
static struct proxy_view *view_check(lua_State *L, knurl_error *error)
{
    struct proxy_view *view = lua_touserdata(L, lua_upvalueindex(1));
    if (view == NULL || !view->active) {
        rl_fail(error, KNURL_STATE, "proxy callback scope has closed");
        return NULL;
    }
    return view;
}
static int param(lua_State *L)
{
    knurl_error error;
    struct proxy_view *view = view_check(L, &error);
    const char *name;
    knurl_bytes value;
    int index = lua_istable(L, 1) ? 2 : 1;
    if (view == NULL)
        return rl_error(L, &error);
    if (!string_value(L, index, &name))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "parameter name must be a NUL-free string"),
                            &error));
    value = knurl_proxy_inbound_path_param(view->in, name);
    if (value.data == NULL)
        lua_pushnil(L);
    else
        lua_pushlstring(L, value.data, value.length);
    return 1;
}
static int edit(lua_State *L)
{
    knurl_error error;
    struct proxy_view *view = view_check(L, &error);
    int index = lua_istable(L, 1) ? 2 : 1;
    int operation = (int)lua_tointeger(L, lua_upvalueindex(2));
    size_t value;
    const char *first = NULL, *second = NULL;
    knurl_http_method method;
    knurl_status status;
    if (view == NULL)
        return rl_error(L, &error);
    if (operation == TARGET || operation == RESPONSE_STATUS) {
        if (!rl_size(L, index, &value))
            goto invalid;
    } else {
        if (!((operation == QUERY || operation == HOST) &&
              lua_isnil(L, index)) &&
            !string_value(L, index, &first))
            goto invalid;
        if ((operation == ADD_HEADER || operation == SET_HEADER ||
             operation == RESPONSE_ADD || operation == RESPONSE_SET) &&
            !string_value(L, index + 1, &second))
            goto invalid;
    }
    switch (operation) {
    case TARGET:
        status = knurl_proxy_outbound_select_target(
            view->out, value == 0 ? (size_t)-1 : value - 1, &error);
        break;
    case METHOD:
        if (!rl_method_value(L, index, &method) ||
            strcmp(first, method_names[method]))
            goto invalid;
        status = knurl_proxy_outbound_set_method(view->out, method, &error);
        break;
    case PATH:
        status = knurl_proxy_outbound_set_path(view->out, first, &error);
        break;
    case QUERY:
        status = knurl_proxy_outbound_set_query(view->out, first, &error);
        break;
    case HOST:
        status = knurl_proxy_outbound_set_host(view->out, first, &error);
        break;
    case ADD_HEADER:
        status =
            knurl_proxy_outbound_add_header(view->out, first, second, &error);
        break;
    case SET_HEADER:
        status =
            knurl_proxy_outbound_set_header(view->out, first, second, &error);
        break;
    case REMOVE_HEADER:
        status = knurl_proxy_outbound_remove_header(view->out, first, &error);
        break;
    case RESPONSE_STATUS:
        status = knurl_proxy_response_set_status(
            view->response, value > 599 ? 600 : (int)value, &error);
        break;
    case RESPONSE_ADD:
        status = knurl_proxy_response_add_header(view->response, first, second,
                                                 &error);
        break;
    case RESPONSE_SET:
        status = knurl_proxy_response_set_header(view->response, first, second,
                                                 &error);
        break;
    case RESPONSE_REMOVE:
        status =
            knurl_proxy_response_remove_header(view->response, first, &error);
        break;
    default:
        goto invalid;
    }
    if (status != KNURL_OK)
        goto failed;
    lua_pushboolean(L, 1);
    return 1;
invalid:
    rl_fail(&error, KNURL_INVALID,
            "invalid proxy setter argument; metadata strings must be NUL-free");
failed:
    if (view->failure.status == KNURL_OK)
        view->failure = error;
    return rl_error(L, &error);
}
static void method_field(lua_State *L, int view, const char *name,
                         int operation)
{
    lua_pushvalue(L, view);
    lua_pushinteger(L, operation);
    lua_pushcclosure(L, edit, 2);
    lua_setfield(L, -2, name);
}
static void headers_push(lua_State *L, const struct proxy_view *view)
{
    const char *name, *value;
    size_t i, count = view->in != NULL
                          ? knurl_proxy_inbound_header_count(view->in)
                          : knurl_proxy_response_header_count(view->response);
    lua_createtable(L, (int)count, 0);
    for (i = 0; i < count; ++i) {
        if (view->in != NULL)
            knurl_proxy_inbound_header_at(view->in, i, &name, &value);
        else
            knurl_proxy_response_header_at(view->response, i, &name, &value);
        lua_createtable(L, 0, 2);
        lua_pushstring(L, name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, value);
        lua_setfield(L, -2, "value");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "headers");
}
static void inbound_push(lua_State *L, int index, const struct proxy_view *view)
{
    lua_createtable(L, 0, 7);
    lua_pushstring(L, method_names[knurl_proxy_inbound_method(view->in)]);
    lua_setfield(L, -2, "method");
    lua_pushstring(L, knurl_proxy_inbound_path(view->in));
    lua_setfield(L, -2, "path");
    lua_pushstring(L, knurl_proxy_inbound_query(view->in));
    lua_setfield(L, -2, "query");
    lua_pushstring(L, knurl_proxy_inbound_host(view->in));
    lua_setfield(L, -2, "host");
    lua_pushboolean(L, knurl_proxy_inbound_websocket(view->in));
    lua_setfield(L, -2, "websocket");
    lua_pushvalue(L, index);
    lua_pushcclosure(L, param, 1);
    lua_setfield(L, -2, "param");
    headers_push(L, view);
}
static void outbound_push(lua_State *L, int index)
{
    static const char *const names[] = {
        "select_target", "set_method", "set_path",   "set_query",
        "set_host",      "add_header", "set_header", "remove_header"};
    size_t i;
    lua_createtable(L, 0, (int)N(names));
    for (i = 0; i < N(names); ++i)
        method_field(L, index, names[i], (int)i);
}
static void response_push(lua_State *L, int index,
                          const struct proxy_view *view)
{
    static const char *const names[] = {"set_status", "add_header",
                                        "set_header", "remove_header"};
    size_t i;
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, knurl_proxy_response_status(view->response));
    lua_setfield(L, -2, "status");
    headers_push(L, view);
    for (i = 0; i < N(names); ++i)
        method_field(L, index, names[i], RESPONSE_STATUS + (int)i);
}
static knurl_status local_result(lua_State *L, int index,
                                 knurl_proxy_local_response *local,
                                 knurl_error *error)
{
    int headers, entry;
    size_t code, length = 0, count, i;
    const char *body = NULL, *name, *value;
    knurl_status status;
    if (lua_isnil(L, index) ||
        (lua_isboolean(L, index) && lua_toboolean(L, index)))
        return KNURL_OK;
    index = lua_absindex(L, index);
    if (!lua_istable(L, index))
        goto invalid;
    lua_getfield(L, index, "status");
    if (!rl_size(L, -1, &code))
        goto invalid;
    lua_pop(L, 1);
    lua_getfield(L, index, "body");
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING)
            goto invalid;
        body = lua_tolstring(L, -1, &length);
    }
    status = knurl_proxy_local_respond(local, code > 599 ? 600 : (int)code,
                                       body, length, error);
    if (status != KNURL_OK)
        return status;
    lua_pop(L, 1);
    lua_getfield(L, index, "headers");
    if (lua_isnil(L, -1))
        return KNURL_OK;
    if (!lua_istable(L, -1))
        goto invalid;
    headers = lua_absindex(L, -1);
    count = lua_rawlen(L, headers);
    if (count > 100)
        goto invalid;
    for (i = 0; i < count; ++i) {
        lua_rawgeti(L, headers, (lua_Integer)i + 1);
        if (!lua_istable(L, -1))
            goto invalid;
        entry = lua_absindex(L, -1);
        lua_getfield(L, entry, "name");
        lua_getfield(L, entry, "value");
        if (!string_value(L, -2, &name) || !string_value(L, -1, &value))
            goto invalid;
        status = knurl_proxy_local_add_header(local, name, value, error);
        if (status != KNURL_OK)
            return status;
        lua_pop(L, 3);
    }
    return KNURL_OK;
invalid:
    return rl_fail(error, KNURL_INVALID,
                   "proxy local hook requires nil, true, or status/body and up "
                   "to 100 ordered header pairs");
}
static int trampoline(lua_State *L)
{
    static const char *const names[] = {"preflight", "rewrite",
                                        "modify_response", "on_error"};
    struct proxy_call *call = lua_touserdata(L, 1);
    struct proxy_view *view;
    int index, options;
    rl_rule_options(L, call->rule);
    options = lua_absindex(L, -1);
    view = lua_newuserdatauv(L, sizeof(*view), 0);
    memset(view, 0, sizeof(*view));
    index = lua_absindex(L, -1);
    lua_pushvalue(L, index);
    call->root = luaL_ref(L, LUA_REGISTRYINDEX);
    call->view = view;
    view->in = call->in;
    view->out = call->out;
    view->response = call->response;
    view->active = 1;
    rl_field(L, options, names[call->phase]);
    if (call->in != NULL)
        inbound_push(L, index, view);
    else if (call->response != NULL)
        response_push(L, index, view);
    else {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, call->default_status);
        lua_setfield(L, -2, "status");
        if (call->cause != NULL) {
            lua_pushinteger(L, call->cause->status);
            lua_setfield(L, -2, "code");
            lua_pushstring(L, call->cause->message);
            lua_setfield(L, -2, "message");
        }
    }
    if (call->out != NULL)
        outbound_push(L, index);
    lua_call(L, call->out != NULL ? 2 : 1, 1);
    view->active = 0;
    if (call->local != NULL)
        call->status = local_result(L, -1, call->local, call->error);
    else if (!(lua_isnil(L, -1) ||
               (lua_isboolean(L, -1) && lua_toboolean(L, -1))))
        call->status =
            rl_fail(call->error, KNURL_INVALID,
                    "proxy rewrite/response hook must return nil or true");
    return 0;
}
static knurl_status invoke(struct proxy_call *call)
{
    lua_State *L = call->rule->server->L;
    knurl_error local;
    knurl_status status;
    if (call->error == NULL)
        call->error = &local;
    call->root = LUA_NOREF;
    status = rl_protect(L, trampoline, call, call->error);
    if (status == KNURL_OK)
        status = call->status;
    if (call->view != NULL) {
        call->view->active = 0;
        call->view->in = NULL;
        call->view->out = NULL;
        call->view->response = NULL;
        if (status == KNURL_OK && call->view->failure.status != KNURL_OK) {
            *call->error = call->view->failure;
            status = call->error->status;
        }
    }
    if (call->root != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, call->root);
    return status;
}
static knurl_status preflight(const knurl_proxy_inbound *in,
                              knurl_proxy_local_response *local, void *data,
                              knurl_error *error)
{
    struct proxy_call call;
    memset(&call, 0, sizeof(call));
    call.rule = data;
    call.in = in;
    call.local = local;
    call.error = error;
    return invoke(&call);
}
static knurl_status rewrite(const knurl_proxy_inbound *in,
                            knurl_proxy_outbound *out, void *data,
                            knurl_error *error)
{
    struct proxy_call call;
    memset(&call, 0, sizeof(call));
    call.rule = data;
    call.in = in;
    call.out = out;
    call.error = error;
    call.phase = 1;
    return invoke(&call);
}
static knurl_status modify_response(knurl_proxy_response *response, void *data,
                                    knurl_error *error)
{
    struct proxy_call call;
    memset(&call, 0, sizeof(call));
    call.rule = data;
    call.response = response;
    call.error = error;
    call.phase = 2;
    return invoke(&call);
}
static knurl_status on_error(const knurl_error *cause, int code,
                             knurl_proxy_local_response *local, void *data,
                             knurl_error *error)
{
    struct proxy_call call;
    memset(&call, 0, sizeof(call));
    call.rule = data;
    call.cause = cause;
    call.local = local;
    call.default_status = code;
    call.error = error;
    call.phase = 3;
    return invoke(&call);
}
struct terminal_call {
    struct rl_rule *rule;
    const knurl_proxy_exchange *exchange;
};
static int terminal_trampoline(lua_State *L)
{
    struct terminal_call *call = lua_touserdata(L, 1);
    const knurl_proxy_exchange *e = call->exchange;
    rl_rule_options(L, call->rule);
    rl_field(L, -1, "on_exchange");
    lua_createtable(L, 0, 11);
#define TEXT(name)                                                             \
    lua_pushstring(L, e->name);                                                \
    lua_setfield(L, -2, #name)
#define COUNT(name)                                                            \
    rl_push_u64(L, e->name);                                                   \
    lua_setfield(L, -2, #name)
    TEXT(route);
    TEXT(upstream);
    TEXT(kind);
    TEXT(disconnect_side);
    TEXT(error_category);
    COUNT(request_id);
    COUNT(upstream_bytes);
    COUNT(downstream_bytes);
    COUNT(duration_ms);
#undef TEXT
#undef COUNT
    lua_pushinteger(L, e->status);
    lua_setfield(L, -2, "status");
    lua_pushinteger(L, e->cause);
    lua_setfield(L, -2, "cause");
    lua_call(L, 1, 0);
    return 0;
}
static void terminal(knurl_request *request, const knurl_proxy_exchange *e,
                     void *data)
{
    struct terminal_call call;
    knurl_error error;
    struct rl_rule *rule = data;
    (void)request;
    call.rule = rule;
    call.exchange = e;
    if (rl_protect(rule->server->L, terminal_trampoline, &call, &error) !=
            KNURL_OK &&
        rule->server->callback_error.status == KNURL_OK)
        rule->server->callback_error = error;
}
int rl_proxy(lua_State *L)
{
    static const char *const fields[] = {"path",
                                         "target",
                                         "path_kind",
                                         "method",
                                         "methods",
                                         "alternate_targets",
                                         "upstream_http_version",
                                         "connect_timeout_ms",
                                         "idle_timeout_ms",
                                         "total_timeout_ms",
                                         "buffer_limit_bytes",
                                         "tls_ca_pem",
                                         "tls_client_cert_pem",
                                         "tls_client_key_pem",
                                         "preflight",
                                         "rewrite",
                                         "modify_response",
                                         "on_error",
                                         "on_exchange"};
    static const char *const callbacks[] = {
        "preflight", "rewrite", "modify_response", "on_error", "on_exchange"};
    knurl_proxy_config config;
    knurl_route_config route;
    struct rl_rule *rule;
    knurl_error error;
    knurl_status status;
    const char *version = NULL, *kind = NULL;
    const char **alternates;
    size_t count, i, connect_ms = 0, idle_ms = 0, total_ms = 0;
    int index, root, selected[5];
    if (!lua_istable(L, 2))
        goto invalid;
    if (!rl_fields(L, 2, fields, N(fields), &error))
        return rl_error(L, &error);
    knurl_proxy_config_init(&config);
    knurl_route_config_init(&route);
    if (!rl_field_string(L, 2, "path", &config.path) ||
        !rl_field_string(L, 2, "target", &config.target) ||
        !rl_field_string(L, 2, "path_kind", &kind) ||
        !rl_field_string(L, 2, "upstream_http_version", &version) ||
        !rl_field_string(L, 2, "tls_ca_pem", &config.tls_ca_pem) ||
        !rl_field_string(L, 2, "tls_client_cert_pem",
                         &config.tls_client_cert_pem) ||
        !rl_field_string(L, 2, "tls_client_key_pem",
                         &config.tls_client_key_pem) ||
        !rl_methods(L, 2, KNURL_HTTP_METHODS_ALL, &config.methods) ||
        !rl_field_size(L, 2, "connect_timeout_ms", &connect_ms) ||
        connect_ms > LONG_MAX ||
        !rl_field_size(L, 2, "idle_timeout_ms", &idle_ms) ||
        idle_ms > LONG_MAX ||
        !rl_field_size(L, 2, "total_timeout_ms", &total_ms) ||
        total_ms > LONG_MAX ||
        !rl_field_size(L, 2, "buffer_limit_bytes", &config.buffer_limit_bytes))
        goto invalid;
    if (kind != NULL) {
        if (!rl_route_path(L, 2, &route))
            goto invalid;
        config.path_kind = route.path_kind;
    }
    if (version != NULL) {
        if (!strcmp(version, "http1"))
            config.upstream_http_version = KNURL_PROXY_HTTP_1_1;
        else if (strcmp(version, "auto"))
            goto invalid;
    }
    config.connect_timeout_ms = (long)connect_ms;
    config.idle_timeout_ms = (long)idle_ms;
    config.total_timeout_ms = (long)total_ms;
    for (i = 0; i < N(callbacks); ++i) {
        rl_field(L, 2, callbacks[i]);
        selected[i] = lua_isfunction(L, -1);
        if (!selected[i] && !lua_isnil(L, -1))
            goto invalid;
        lua_pop(L, 1);
    }
    rl_field(L, 2, "alternate_targets");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            goto invalid;
        count = lua_rawlen(L, -1);
        if (count > 15)
            goto invalid;
        alternates = lua_newuserdatauv(
            L, (count == 0 ? 1 : count) * sizeof(*alternates), 0);
        for (i = 0; i < count; ++i) {
            lua_rawgeti(L, -2, (lua_Integer)i + 1);
            if (!string_value(L, -1, &alternates[i]))
                goto invalid;
            lua_pop(L, 1);
        }
        config.alternate_targets = alternates;
        config.alternate_target_count = count;
    }
    rule = rl_rule_new(L, &error);
    if (rule == NULL)
        return rl_error(L, &error);
    if (rule->group != NULL)
        return rl_error(L, (rl_fail(&error, KNURL_NOT_SUPPORTED,
                                    "proxy registers on a server router"),
                            &error));
    index = lua_absindex(L, -1);
    if (selected[0]) {
        config.preflight = preflight;
        config.preflight_userdata = rule;
    }
    if (selected[1]) {
        config.rewrite = rewrite;
        config.rewrite_userdata = rule;
    }
    if (selected[2]) {
        config.modify_response = modify_response;
        config.modify_response_userdata = rule;
    }
    if (selected[3]) {
        config.on_error = on_error;
        config.on_error_userdata = rule;
    }
    if (selected[4]) {
        config.on_exchange = terminal;
        config.on_exchange_userdata = rule;
    }
    root = rl_rule_root(L, index, rule);
    status = knurl_router_proxy(knurl_server_router(rule->server->native),
                                &config, NULL, &error);
    return rl_rule_finish(L, index, root, rule, status, &error);
invalid:
    return rl_error(L,
                    (rl_fail(&error, KNURL_INVALID,
                             "invalid proxy declaration; supply path, target "
                             "and valid callback/target/TLS/limit values"),
                     &error));
}
