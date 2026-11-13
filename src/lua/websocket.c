#include "binding.h"
#define RL_WS_DECL "knurl.websocket_declaration"
struct ws_declaration {
    knurl_websocket_config config;
    knurl_reply *fields;
};
struct ws_view {
    knurl_websocket *native;
    struct rl_rule *rule;
    int root;
    knurl_error error;
};
struct ws_call {
    struct ws_view *view;
    const knurl_websocket_outcome *outcome;
    knurl_websocket_opcode opcode;
    knurl_bytes bytes;
    int phase;
    int final;
    knurl_status status;
    knurl_error *error;
};
static struct ws_view *checked(lua_State *L, knurl_error *error)
{
    struct ws_view *view = luaL_testudata(L, 1, RL_WEBSOCKET);
    if (view == NULL || view->native == NULL) {
        rl_fail(error, KNURL_STATE, "WebSocket has closed");
        return NULL;
    }
    return view;
}
static int send_value(lua_State *L, int opcode, int explicit_opcode)
{
    knurl_error error;
    struct ws_view *view = checked(L, &error);
    const char *bytes = NULL;
    size_t length = 0, value;
    int index = explicit_opcode == 2 ? 4 : explicit_opcode ? 3 : 2;
    int final = 1;
    knurl_status status;
    if (view == NULL)
        return rl_error(L, &error);
    if (explicit_opcode) {
        if (!rl_size(L, 2, &value) || value > 15)
            goto invalid;
        opcode = (int)value;
    }
    if (explicit_opcode == 2) {
        if (lua_type(L, 3) != LUA_TBOOLEAN)
            goto invalid;
        final = lua_toboolean(L, 3);
    }
    if (!lua_isnoneornil(L, index)) {
        if (!lua_isstring(L, index))
            goto invalid;
        bytes = lua_tolstring(L, index, &length);
    } else if (!explicit_opcode)
        goto invalid;
    status =
        knurl_websocket_send_frame(view->native, (knurl_websocket_opcode)opcode,
                                   final, bytes, length, &error);
    if (status == KNURL_AGAIN) {
        lua_pushnil(L);
        rl_sentinel(L, status);
        return 2;
    }
    if (status != KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
invalid:
    return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                "valid opcode and binary string required"),
                        &error));
}
static int send(lua_State *L) { return send_value(L, 0, 1); }
static int send_frame(lua_State *L) { return send_value(L, 0, 2); }
static int send_text(lua_State *L)
{
    return send_value(L, KNURL_WEBSOCKET_TEXT, 0);
}
static int send_binary(lua_State *L)
{
    return send_value(L, KNURL_WEBSOCKET_BINARY, 0);
}
static int close_ws(lua_State *L)
{
    struct ws_view *view = luaL_testudata(L, 1, RL_WEBSOCKET);
    knurl_error error;
    knurl_bytes reason = {0};
    size_t code = 0;
    knurl_status status;
    if (view == NULL)
        goto invalid;
    if (view->native != NULL) {
        if ((!lua_isnoneornil(L, 2) && !rl_size(L, 2, &code)) || code > 65535)
            goto invalid;
        if (!lua_isnoneornil(L, 3)) {
            if (lua_type(L, 3) != LUA_TSTRING)
                goto invalid;
            reason.data = lua_tolstring(L, 3, &reason.length);
        }
        status = knurl_websocket_close(view->native, (unsigned short)code,
                                       reason, &error);
        if (status == KNURL_AGAIN) {
            lua_pushnil(L);
            rl_sentinel(L, status);
            return 2;
        }
        if (status != KNURL_OK)
            return rl_error(L, &error);
    }
    if (view->error.status != KNURL_OK)
        return rl_error(L, &view->error);
    lua_pushboolean(L, 1);
    return 1;
invalid:
    return rl_error(L,
                    (rl_fail(&error, KNURL_INVALID,
                             "WebSocket, close code and UTF-8 reason required"),
                     &error));
}
static int resume(lua_State *L)
{
    knurl_error error;
    struct ws_view *view = checked(L, &error);
    if (view == NULL ||
        knurl_websocket_resume(view->native, &error) != KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int state(lua_State *L)
{
    static const char *const names[] = {"connecting", "open", "closing",
                                        "closed"};
    struct ws_view *view = luaL_testudata(L, 1, RL_WEBSOCKET);
    lua_pushstring(L, view == NULL || view->native == NULL
                          ? "closed"
                          : names[knurl_websocket_state(view->native)]);
    return 1;
}
static int queued(lua_State *L)
{
    knurl_error error;
    struct ws_view *view = checked(L, &error);
    if (view == NULL)
        return rl_error(L, &error);
    rl_push_size(L, knurl_websocket_queued_bytes(view->native));
    return 1;
}
static int callback_trampoline(lua_State *L)
{
    static const char *const names[] = {"connect", "message", "writable",
                                        "disconnect"};
    struct ws_call *call = lua_touserdata(L, 1);
    int arguments = 1;
    rl_rule_options(L, call->view->rule);
    rl_field(L, -1, names[call->phase]);
    if (lua_isnil(L, -1))
        return 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, call->view->root);
    if (call->phase == 1) {
        lua_pushinteger(L, call->opcode);
        lua_pushlstring(L, call->bytes.data == NULL ? "" : call->bytes.data,
                        call->bytes.length);
        lua_pushboolean(L, call->final);
        arguments = 4;
    } else if (call->phase == 3) {
        lua_createtable(L, 0, 7);
        lua_pushinteger(L, call->outcome->cause);
        lua_setfield(L, -2, "cause");
        lua_pushboolean(L, call->outcome->opened);
        lua_setfield(L, -2, "opened");
        lua_pushboolean(L, call->outcome->clean);
        lua_setfield(L, -2, "clean");
        lua_pushinteger(L, call->outcome->local_code);
        lua_setfield(L, -2, "local_code");
        lua_pushinteger(L, call->outcome->peer_code);
        lua_setfield(L, -2, "peer_code");
        lua_pushlstring(L,
                        call->outcome->peer_reason.data == NULL
                            ? ""
                            : call->outcome->peer_reason.data,
                        call->outcome->peer_reason.length);
        lua_setfield(L, -2, "peer_reason");
        if (call->view->error.status != KNURL_OK) {
            rl_push_error(L, &call->view->error);
            lua_setfield(L, -2, "error");
        }
        arguments = 2;
    }
    lua_call(L, arguments, 2);
    if (call->phase == 1 && lua_isnil(L, -2) &&
        rl_is_sentinel(L, -1, KNURL_AGAIN))
        call->status = KNURL_AGAIN;
    else if (!lua_isnil(L, -1))
        call->status = rl_callback_error(
            L, -1, call->error, "WebSocket callback returned an error");
    else if (lua_isboolean(L, -2) && !lua_toboolean(L, -2))
        call->status = rl_fail(call->error, KNURL_INVALID,
                               "WebSocket callback rejected delivery");
    return 0;
}
static knurl_status call_ws(struct ws_call *call)
{
    knurl_error local;
    knurl_status status;
    if (call->error == NULL)
        call->error = &local;
    status = rl_protect(call->view->rule->server->L, callback_trampoline, call,
                        call->error);
    if (status == KNURL_OK)
        status = call->status;
    if (status != KNURL_OK && status != KNURL_AGAIN &&
        call->view->error.status == KNURL_OK)
        call->view->error = *call->error;
    return status;
}
static knurl_status opened(knurl_websocket *native, void *data,
                           knurl_error *error)
{
    struct ws_call call;
    (void)native;
    memset(&call, 0, sizeof(call));
    call.view = data;
    call.error = error;
    return call_ws(&call);
}
static knurl_status frame(knurl_websocket *native,
                          knurl_websocket_opcode opcode, int final,
                          knurl_bytes bytes, void *data, knurl_error *error)
{
    struct ws_call call;
    (void)native;
    memset(&call, 0, sizeof(call));
    call.view = data;
    call.error = error;
    call.phase = 1;
    call.opcode = opcode;
    call.bytes = bytes;
    call.final = final;
    return call_ws(&call);
}
static void writable(knurl_websocket *native, void *data)
{
    struct ws_call call;
    knurl_bytes empty = {0};
    memset(&call, 0, sizeof(call));
    call.view = data;
    call.phase = 2;
    if (call_ws(&call) != KNURL_OK)
        knurl_websocket_close(native, 1011, empty, NULL);
}
static void closed(knurl_websocket *native,
                   const knurl_websocket_outcome *outcome, void *data)
{
    struct ws_view *view = data;
    struct ws_call call;
    (void)native;
    memset(&call, 0, sizeof(call));
    call.view = view;
    call.phase = 3;
    call.outcome = outcome;
    (void)call_ws(&call);
    view->native = NULL;
    luaL_unref(view->rule->server->L, LUA_REGISTRYINDEX, view->root);
    view->root = LUA_NOREF;
}
struct header_call {
    struct rl_rule *rule;
    knurl_request *request;
    knurl_request_plan *plan;
    knurl_status status;
    knurl_error *error;
};
static int headers_trampoline(lua_State *L)
{
    struct header_call *call = lua_touserdata(L, 1);
    struct ws_declaration *declaration;
    struct ws_view *view;
    knurl_websocket_config config;
    knurl_bytes body;
    int owner, index;
    rl_rule_options(L, call->rule);
    lua_pop(L, 1);
    owner = lua_absindex(L, -1);
    lua_getiuservalue(L, owner, 3);
    declaration = lua_touserdata(L, -1);
    config = declaration->config;
    view = lua_newuserdatauv(L, sizeof(*view), 1);
    memset(view, 0, sizeof(*view));
    view->rule = call->rule;
    view->root = LUA_NOREF;
    index = lua_absindex(L, -1);
    luaL_setmetatable(L, RL_WEBSOCKET);
    lua_pushvalue(L, owner);
    lua_setiuservalue(L, index, 1);
    lua_pushvalue(L, index);
    view->root = luaL_ref(L, LUA_REGISTRYINDEX);
    config.userdata = view;
    call->status = knurl_websocket_accept(call->request, &config, &view->native,
                                          call->error);
    if (call->status == KNURL_OK)
        call->plan->disposition = KNURL_REQUEST_WEBSOCKET;
    else {
        luaL_unref(L, LUA_REGISTRYINDEX, view->root);
        view->root = LUA_NOREF;
        view->native = NULL;
        if (call->status == KNURL_INVALID) {
            body.data = "invalid WebSocket handshake\n";
            body.length = strlen(body.data);
            call->status =
                knurl_response_bytes(knurl_request_response(call->request), 400,
                                     "text/plain", body, call->error);
            if (call->status == KNURL_OK)
                call->plan->disposition = KNURL_REQUEST_REPLY;
        }
    }
    return 0;
}
static knurl_status headers(knurl_request *request, knurl_request_plan *plan,
                            void *data, knurl_error *error)
{
    struct header_call call;
    knurl_status status;
    memset(&call, 0, sizeof(call));
    call.rule = data;
    call.request = request;
    call.plan = plan;
    call.error = error;
    status = rl_protect(call.rule->server->L, headers_trampoline, &call, error);
    return status == KNURL_OK ? call.status : status;
}
static int declaration_gc(lua_State *L)
{
    struct ws_declaration *declaration = luaL_testudata(L, 1, RL_WS_DECL);
    if (declaration != NULL && declaration->fields != NULL) {
        knurl_reply_destroy(declaration->fields, NULL);
        declaration->fields = NULL;
    }
    return 0;
}
int rl_websocket(lua_State *L)
{
    static const char *const fields[] = {"path",
                                         "path_kind",
                                         "connect",
                                         "message",
                                         "writable",
                                         "disconnect",
                                         "max_frame_bytes",
                                         "max_message_bytes",
                                         "output_queue_bytes",
                                         "close_timeout_ms",
                                         "protocol",
                                         "headers"};
    struct rl_rule *rule;
    struct ws_declaration *declaration;
    knurl_route_config route;
    knurl_header *copy;
    knurl_error error;
    knurl_status status;
    size_t timeout = 0, count, i;
    int index, declaration_index, root;
    const char *kind = NULL;
    if (!lua_istable(L, 2))
        goto invalid;
    if (!rl_fields(L, 2, fields, sizeof(fields) / sizeof(fields[0]), &error))
        return rl_error(L, &error);
    for (i = 2; i < 6; ++i) {
        rl_field(L, 2, fields[i]);
        if (!lua_isfunction(L, -1) && (i == 3 || !lua_isnil(L, -1)))
            goto invalid;
        lua_pop(L, 1);
    }
    rule = rl_rule_new(L, &error);
    if (rule == NULL)
        return rl_error(L, &error);
    if (rule->group != NULL)
        goto invalid;
    index = lua_absindex(L, -1);
    declaration = lua_newuserdatauv(L, sizeof(*declaration), 1);
    memset(declaration, 0, sizeof(*declaration));
    declaration_index = lua_absindex(L, -1);
    luaL_setmetatable(L, RL_WS_DECL);
    knurl_websocket_config_init(&declaration->config);
    knurl_route_config_init(&route);
    if (!rl_route_path(L, 2, &route) ||
        !rl_field_string(L, 2, "path_kind", &kind) ||
        !rl_field_size(L, 2, "max_frame_bytes",
                       &declaration->config.max_frame_bytes) ||
        !rl_field_size(L, 2, "max_message_bytes",
                       &declaration->config.max_message_bytes) ||
        !rl_field_size(L, 2, "output_queue_bytes",
                       &declaration->config.output_queue_bytes) ||
        !rl_field_size(L, 2, "close_timeout_ms", &timeout) ||
        timeout > LONG_MAX ||
        !rl_field_string(L, 2, "protocol", &declaration->config.protocol))
        goto invalid;
    if (kind == NULL)
        route.path_kind = KNURL_PATH_LITERAL;
    declaration->config.close_timeout_ms = (long)timeout;
    declaration->config.on_open = opened;
    declaration->config.on_frame = frame;
    declaration->config.on_writable = writable;
    declaration->config.on_close = closed;
    if (knurl_reply_new(&declaration->fields, &error) != KNURL_OK)
        return rl_error(L, &error);
    rl_field(L, 2, "headers");
    if (!lua_isnil(L, -1) &&
        (!lua_istable(L, -1) ||
         rl_reply_headers(L, -1, declaration->fields, &error) != KNURL_OK)) {
        if (!lua_istable(L, -1))
            goto invalid;
        return rl_error(L, &error);
    }
    lua_pop(L, 1);
    count = knurl_reply_header_count(declaration->fields);
    if (count != 0) {
        copy = lua_newuserdatauv(L, count * sizeof(*copy), 0);
        for (i = 0; i < count; ++i)
            knurl_reply_header_at(declaration->fields, i, &copy[i], NULL);
        declaration->config.headers = copy;
        declaration->config.header_count = count;
        lua_setiuservalue(L, declaration_index, 1);
    }
    lua_pushvalue(L, declaration_index);
    lua_setiuservalue(L, index, 3);
    route.method = KNURL_HTTP_GET;
    route.on_headers = headers;
    route.userdata = rule;
    root = rl_rule_root(L, index, rule);
    status = knurl_router_route(knurl_server_router(rule->server->native),
                                &route, NULL, &error);
    return rl_rule_finish(L, index, root, rule, status, &error);
invalid:
    return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                "WebSocket path/message callback and valid "
                                "connection options required"),
                        &error));
}
void rl_websockets_open(lua_State *L)
{
    static const luaL_Reg methods[] = {{"send", send},
                                       {"send_frame", send_frame},
                                       {"send_text", send_text},
                                       {"send_binary", send_binary},
                                       {"close", close_ws},
                                       {"resume", resume},
                                       {"state", state},
                                       {"queued_bytes", queued},
                                       {NULL, NULL}};
    static const char *const names[] = {"CONTINUATION", "TEXT", "BINARY",
                                        "CLOSE",        "PING", "PONG"};
    static const int values[] = {0, 1, 2, 8, 9, 10};
    size_t i;
    luaL_newmetatable(L, RL_WS_DECL);
    lua_pushcfunction(L, declaration_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newmetatable(L, RL_WEBSOCKET);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pop(L, 1);
    lua_newtable(L);
    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        lua_pushinteger(L, values[i]);
        lua_setfield(L, -2, names[i]);
    }
    lua_setfield(L, -2, "websocket");
}
