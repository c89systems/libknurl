/* Vectis's buffered REST response: encode once through public LoneJSON Lua. */
#include "binding.h"
#include <knurl/lonejson.h>

#define RL_REST "knurl.rest_buffer"
struct rest_buffer {
    unsigned char *data;
    size_t length, capacity, bound;
    knurl_status failure;
};
static int rest_close(lua_State *L)
{
    struct rest_buffer *b = luaL_testudata(L, 1, RL_REST);
    if (b != NULL) {
        free(b->data);
        b->data = NULL;
    }
    return 0;
}
static int rest_sink(lua_State *L)
{
    struct rest_buffer *b = lua_touserdata(L, lua_upvalueindex(1));
    size_t length, needed, capacity;
    const char *chunk = luaL_checklstring(L, 1, &length);
    unsigned char *grown;
    if (length > b->bound - b->length) {
        b->failure = KNURL_LIMIT;
        return luaL_error(L, "REST JSON output exceeds max_bytes");
    }
    needed = b->length + length;
    if (needed > b->capacity) {
        capacity = b->capacity ? b->capacity : 4096;
        if (capacity > b->bound)
            capacity = b->bound;
        while (capacity < needed) {
            if (capacity > b->bound / 2) {
                capacity = b->bound;
                break;
            }
            capacity *= 2;
        }
        grown = realloc(b->data, capacity);
        if (grown == NULL) {
            b->failure = KNURL_NOMEM;
            return luaL_error(L, "allocating REST JSON output");
        }
        b->data = grown;
        b->capacity = capacity;
    }
    if (length)
        memcpy(b->data + b->length, chunk, length);
    b->length = needed;
    return 0;
}
static int rest_json(lua_State *L)
{
    struct rest_buffer *b;
    knurl_rest_reply_config config;
    knurl_error error;
    size_t status = 200;
    int owner, options, newline = 1, code;
    static const char *const fields[] = {"status",       "status_code",
                                         "content_type", "headers",
                                         "newline",      "max_bytes"};
    lua_settop(L, 2);
    if (lua_isnil(L, 2)) {
        lua_newtable(L);
        lua_replace(L, 2);
    }
    if (!lua_istable(L, 2))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "REST JSON response options required"),
                            &error));
    if (!rl_fields(L, 2, fields, sizeof(fields) / sizeof(fields[0]), &error))
        return rl_error(L, &error);
    rl_table_copy(L, 2);
    options = lua_absindex(L, -1);
    knurl_rest_reply_config_init(&config);
    if (!rl_field_size(L, options, "status_code", &status) ||
        !rl_field_size(L, options, "status", &status) || status < 100 ||
        status > 599 ||
        !rl_field_size(L, options, "max_bytes", &config.max_bytes) ||
        !rl_field_bool(L, options, "newline", &newline) ||
        !rl_field_string(L, options, "content_type", &config.content_type))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "invalid REST JSON response options"),
                            &error));
    config.status = (int)status;
    rl_field(L, options, "headers");
    if (!lua_isnil(L, -1) && !lua_istable(L, -1))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "REST response headers must be a table"),
                            &error));
    lua_pop(L, 1);
    b = lua_newuserdatauv(L, sizeof(*b), 0);
    memset(b, 0, sizeof(*b));
    b->bound = config.max_bytes ? config.max_bytes : (size_t)-1;
    owner = lua_absindex(L, -1);
    luaL_setmetatable(L, RL_REST);
    lua_toclose(L, owner);
    /* The released provider owns generic Lua JSON traversal and validation.
     * Its sink stops at the configured bound, before a large value is built. */
    lua_getglobal(L, "require");
    lua_pushliteral(L, "lonejson");
    lua_call(L, 1, 1);
    lua_getfield(L, -1, "encode_json_to_sink");
    lua_pushvalue(L, 1);
    lua_pushvalue(L, owner);
    lua_pushcclosure(L, rest_sink, 1);
    code = lua_pcall(L, 2, 0, 0);
    if (code == LUA_OK && newline) {
        lua_pushvalue(L, owner);
        lua_pushcclosure(L, rest_sink, 1);
        lua_pushliteral(L, "\n");
        code = lua_pcall(L, 1, 0, 0);
    }
    if (code != LUA_OK) {
        rl_fail(&error,
                b->failure           ? b->failure
                : code == LUA_ERRMEM ? KNURL_NOMEM
                                     : KNURL_INVALID,
                lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1)
                                               : "REST JSON encoding failed");
        error.source = KNURL_ERROR_LONEJSON;
        return rl_error(L, &error);
    }
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, config.status);
    lua_setfield(L, -2, "status");
    lua_pushstring(L, config.content_type != NULL
                          ? config.content_type
                          : "application/json; charset=utf-8");
    lua_setfield(L, -2, "content_type");
    lua_pushlstring(L, (const char *)b->data, b->length);
    lua_setfield(L, -2, "body");
    rl_field(L, options, "headers");
    lua_setfield(L, -2, "headers");
    return 1;
}
struct rest_call {
    knurl_bytes body;
    struct rl_rule *rule;
    knurl_request *request;
    knurl_reply *reply;
    const knurl_error *cause;
    knurl_error *error;
    knurl_status status;
};
static int decode_call(lua_State *L)
{
    struct rest_call *call = lua_touserdata(L, 1);
    struct rl_request *request = knurl_request_userdata(call->request);
    rl_rule_options(L, call->rule);
    rl_field(L, -1, "decode");
    rl_request_push(L, request);
    if (knurl_body_kind(knurl_request_body(call->request)) == KNURL_BODY_FILE)
        lua_pushlstring(L, call->body.data != NULL ? call->body.data : "",
                        call->body.length);
    else
        lua_pushnil(L);
    lua_call(L, 2, 2);
    if (!lua_toboolean(L, -2))
        call->status =
            rl_callback_error(L, -1, call->error, "REST JSON decoding failed");
    return 0;
}
static knurl_status rest_decode(knurl_request *request, knurl_bytes body,
                                void **decoded, void *data, knurl_error *error)
{
    struct rest_call call;
    knurl_status status;
    memset(&call, 0, sizeof(call));
    call.rule = data;
    call.request = request;
    call.error = error;
    call.body = body;
    *decoded = NULL;
    /* The facade stores the decoded value on its existing request table. Core
     * owns phase sequencing; this callback transfers no Lua handle to Core. */
    status = rl_protect(call.rule->server->L, decode_call, &call, error);
    return status == KNURL_OK ? call.status : status;
}
static int error_call(lua_State *L)
{
    struct rest_call *call = lua_touserdata(L, 1);
    struct rl_request *request = knurl_request_userdata(call->request);
    int value, view;
    rl_request_push(L, request);
    view = lua_absindex(L, -1);
    rl_rule_options(L, call->rule);
    rl_field(L, -1, "on_error");
    lua_pushvalue(L, view);
    rl_push_error(L, call->cause);
    lua_call(L, 2, 1);
    value = lua_absindex(L, -1);
    lua_pushvalue(L, value);
    lua_setiuservalue(L, view, 2);
    request->native_reply = 0;
    call->status = rl_reply(L, value, call->reply, call->request, call->error);
    return 0;
}
static knurl_status rest_error(knurl_request *request, knurl_rest_phase phase,
                               const knurl_error *cause, knurl_reply *reply,
                               void *data, knurl_error *error)
{
    struct rest_call call;
    knurl_status status;
    if (phase != KNURL_REST_DECODE) {
        if (error != NULL)
            *error = *cause;
        return cause->status;
    }
    memset(&call, 0, sizeof(call));
    call.rule = data;
    call.request = request;
    call.reply = reply;
    call.cause = cause;
    call.error = error;
    status = rl_protect(call.rule->server->L, error_call, &call, error);
    return status == KNURL_OK ? call.status : status;
}
static knurl_status rest_handler(knurl_request *request, const void *decoded,
                                 knurl_reply *reply, void *data,
                                 knurl_error *error)
{
    (void)decoded;
    return rl_handler(request, reply, data, error);
}
int rl_rest_route(lua_State *L)
{
    static const char *const fields[] = {"path",
                                         "path_kind",
                                         "method",
                                         "methods",
                                         "body",
                                         "handler",
                                         "auth",
                                         "before",
                                         "after",
                                         "decode",
                                         "decode_disabled",
                                         "validate",
                                         "on_error"};
    static const char *const callbacks[] = {"handler", "decode", "validate",
                                            "on_error"};
    struct rl_rule *rule;
    knurl_route_config route;
    knurl_rest_route_config config;
    knurl_error error;
    knurl_status status;
    int index, root;
    size_t i;
    if (!lua_istable(L, 2) ||
        !rl_fields(L, 2, fields, sizeof(fields) / sizeof(fields[0]), &error))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "REST route declaration required"),
                            &error));
    for (i = 0; i < sizeof(callbacks) / sizeof(callbacks[0]); ++i) {
        rl_field(L, 2, callbacks[i]);
        if (!lua_isfunction(L, -1))
            return rl_error(L,
                            (rl_fail(&error, KNURL_INVALID,
                                     "REST route callbacks must be functions"),
                             &error));
        lua_pop(L, 1);
    }
    rule = rl_rule_new(L, &error);
    if (rule == NULL)
        return rl_error(L, &error);
    index = lua_absindex(L, -1);
    knurl_route_config_init(&route);
    knurl_rest_route_config_init(&config);
    if (!rl_route_path(L, 2, &route) ||
        !rl_methods(L, 2, KNURL_HTTP_METHODS_GET, &config.methods) ||
        !rl_field_bool(L, 2, "decode_disabled", &config.decode_disabled))
        goto invalid;
    config.path = route.path;
    config.path_kind = route.path_kind;
    /* Facade body declarations preserve ordinary route inheritance. */
    config.body = route.body;
    rl_field(L, 2, "body");
    if (!lua_isnil(L, -1) && !rl_body_policy(L, -1, &config.body, 0, &error))
        return rl_error(L, &error);
    lua_pop(L, 1);
    config.decode = rest_decode;
    config.validate = rl_rest_validate;
    config.handler = rest_handler;
    config.on_error = rest_error;
    config.userdata = rule;
    rl_rule_hooks(&config.hooks, rule);
    root = rl_rule_root(L, index, rule);
    status =
        rule->group != NULL
            ? knurl_route_group_rest_route(rule->group, &config, NULL, &error)
            : knurl_router_rest_route(knurl_server_router(rule->server->native),
                                      &config, NULL, &error);
    return rl_rule_finish(L, index, root, rule, status, &error);
invalid:
    return rl_error(
        L,
        (rl_fail(&error, KNURL_INVALID, "invalid REST route values"), &error));
}
void rl_rest_open(lua_State *L)
{
    luaL_newmetatable(L, RL_REST);
    lua_pushcfunction(L, rest_close);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, rest_close);
    lua_setfield(L, -2, "__close");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, rest_json);
    lua_setfield(L, -2, "json");
    lua_pushinteger(L, KNURL_ERROR_LONEJSON);
    lua_setfield(L, -2, "ERROR_SOURCE_LONEJSON");
    lua_setfield(L, -2, "rest");
}
