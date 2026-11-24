#include "binding.h"

#define WORKER_START "on_worker_start"
#define WORKER_STOP "on_worker_stop"

struct application {
    struct rl_server *server;
    int reference;
};

struct request_call {
    const struct application *app;
    const knurl_request_snapshot *request;
    knurl_reply *reply;
    struct rl_json *json;
    knurl_error *error;
    knurl_status status;
};

struct reader {
    knurl_source *source;
};

static knurl_status lua_failure(lua_State *L, int result, const char *operation,
                                knurl_error *error)
{
    const char *message = lua_type(L, -1) == LUA_TSTRING
                              ? lua_tostring(L, -1)
                              : "Lua callback failed";
    knurl_status status = result == LUA_ERRMEM ? KNURL_NOMEM : KNURL_STATE;
    if (error != NULL) {
        rl_fail(error, status, message);
        snprintf(error->operation, sizeof(error->operation), "%s", operation);
    }
    return status;
}

static int worker_hook(lua_State *L)
{
    const char *name = lua_touserdata(L, 2);
    rl_field(L, 1, name);
    if (!lua_isnil(L, -1))
        lua_call(L, 0, 0);
    return 0;
}

static knurl_status worker_callback(struct rl_server *server, const char *name,
                                    knurl_error *error)
{
    lua_State *L = server->worker;
    int result;
    knurl_status status = KNURL_OK;
    if (!lua_checkstack(L, 3))
        return rl_fail(error, KNURL_NOMEM, "reserving worker callback stack");
    lua_pushcfunction(L, worker_hook);
    lua_pushvalue(L, 1);
    lua_pushlightuserdata(L, (void *)name);
    result = lua_pcall(L, 2, 0, 0);
    if (result != LUA_OK)
        status = lua_failure(L, result, name, error);
    lua_settop(L, 1);
    return status;
}

static knurl_status start(void *data, knurl_error *error)
{
    return worker_callback(data, WORKER_START, error);
}

static knurl_status stop(void *data, knurl_error *error)
{
    return worker_callback(data, WORKER_STOP, error);
}

knurl_status rl_server_run(lua_State *L, struct rl_server *server,
                           knurl_error *error)
{
    knurl_status status;
    if (server->has_lua) {
        lua_getiuservalue(L, 1, 1);
        server->worker = lua_newthread(L);
        /* The blocked parent's call frame roots this coroutine and its
         * callback table. Each fork exclusively executes it on its pool thread.
         */
        lua_pushvalue(L, -2);
        lua_xmove(L, server->worker, 1);
        server->pool.on_thread_start = start;
        server->pool.on_thread_stop = stop;
        server->pool.userdata = server;
        status = knurl_server_set_blocking_pool(server->native, &server->pool,
                                                error);
        if (status != KNURL_OK) {
            server->worker = NULL;
            return status;
        }
    }
    server->running = 1;
    status = knurl_server_run(server->native, NULL, error);
    server->running = 0;
    server->worker = NULL;
    return status;
}

static int read_body(lua_State *L)
{
    struct reader *reader = lua_touserdata(L, lua_upvalueindex(1));
    size_t capacity = 16384, length = 0;
    luaL_Buffer buffer;
    void *bytes;
    knurl_status status;
    knurl_error error;
    if (reader->source == NULL)
        return rl_result(L, rl_fail(&error, KNURL_STATE, "request has ended"),
                         &error);
    if ((!lua_isnoneornil(L, 2) && !rl_size(L, 2, &capacity)) ||
        capacity == 0 || capacity > 65536UL)
        return rl_result(
            L, rl_fail(&error, KNURL_INVALID, "read size must be in 1..65536"),
            &error);
    bytes = luaL_buffinitsize(L, &buffer, capacity);
    status =
        knurl_source_read(reader->source, bytes, capacity, &length, &error);
    luaL_pushresultsize(&buffer, status == KNURL_OK ? length : 0);
    if (status == KNURL_OK)
        return 1;
    if (status == KNURL_EOF) {
        lua_pushnil(L);
        return 1;
    }
    return rl_result(L, status, &error);
}

static int cancelled(lua_State *L)
{
    struct reader *reader = lua_touserdata(L, lua_upvalueindex(1));
    lua_pushboolean(L, reader->source == NULL ||
                           knurl_source_cancelled(reader->source));
    return 1;
}

static int new_reader(lua_State *L)
{
    struct reader *reader = lua_newuserdatauv(L, sizeof(*reader), 0);
    reader->source = NULL;
    return 1;
}

static void field_bytes(lua_State *L, const char *field, knurl_bytes bytes)
{
    lua_pushlstring(L, bytes.data, bytes.length);
    lua_setfield(L, -2, field);
}

static void push_request(lua_State *L, const knurl_request_snapshot *request)
{
    size_t i;
    lua_createtable(L, 0, 8);
    field_bytes(L, "method", request->method_token);
    field_bytes(L, "path", request->path);
    field_bytes(L, "raw_path", request->raw_path);
    field_bytes(L, "query", request->query);
    lua_createtable(L, 0, (int)request->param_count);
    for (i = 0; i < request->param_count; ++i) {
        const knurl_header *pair = &request->params[i];
        lua_pushlstring(L, pair->name.data, pair->name.length);
        lua_pushlstring(L, pair->value.data, pair->value.length);
        lua_rawset(L, -3);
    }
    lua_setfield(L, -2, "params");
    lua_createtable(L, (int)request->header_count, 0);
    for (i = 0; i < request->header_count; ++i) {
        lua_createtable(L, 0, 2);
        field_bytes(L, "name", request->headers[i].name);
        field_bytes(L, "value", request->headers[i].value);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "headers");
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, read_body, 1);
    lua_setfield(L, -2, "read");
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, cancelled, 1);
    lua_setfield(L, -2, "cancelled");
}

static knurl_status response_headers(lua_State *L, knurl_reply *reply,
                                     knurl_error *error)
{
    int table;
    const char *name, *value;
    size_t name_size, value_size;
    knurl_status status;
    rl_field(L, -1, "headers");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return KNURL_OK;
    }
    if (!lua_istable(L, -1))
        return rl_fail(error, KNURL_STATE, "response headers must be a table");
    table = lua_absindex(L, -1);
    lua_pushnil(L);
    while (lua_next(L, table)) {
        if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING)
            return rl_fail(error, KNURL_STATE,
                           "response headers must be strings");
        name = lua_tolstring(L, -2, &name_size);
        value = lua_tolstring(L, -1, &value_size);
        if (memchr(name, 0, name_size) || memchr(value, 0, value_size))
            return rl_fail(error, KNURL_STATE, "response headers contain NUL");
        status = knurl_reply_header_add(reply, name, value, error);
        if (status != KNURL_OK)
            return status;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return KNURL_OK;
}

static knurl_status stage_response(lua_State *L, struct request_call *call)
{
    static const char *const fields[] = {"status", "body", "content_type",
                                         "headers", "json"};
    const char *type = NULL;
    size_t status_code = 200;
    knurl_status status;
    knurl_bytes bytes;
    int response;
    if (lua_isnil(L, -1))
        return knurl_reply_status(call->reply, 204, call->error);
    if (lua_type(L, -1) == LUA_TSTRING) {
        bytes.data = lua_tolstring(L, -1, &bytes.length);
        return knurl_reply_bytes(call->reply, 200, "text/plain; charset=utf-8",
                                 bytes, call->error);
    }
    response = lua_absindex(L, -1);
    if (!rl_fields(L, response, fields, RL_COUNT(fields), call->error))
        return call->error->status;
    if (!rl_field_size(L, response, "status", &status_code) ||
        status_code < 200 || status_code > 599 ||
        !rl_field_string(L, response, "content_type", &type))
        return rl_fail(call->error, KNURL_STATE, "invalid Lua response fields");
    status = response_headers(L, call->reply, call->error);
    if (status != KNURL_OK)
        return status;

    rl_field(L, response, "body");
    rl_field(L, response, "json");
    if (!lua_isnil(L, -1)) {
        if (!lua_isnil(L, -2))
            return rl_fail(call->error, KNURL_STATE,
                           "choose a response body or JSON value");
        return rl_json_reply(L, -1, call->reply, (int)status_code, type,
                             &call->json, call->error);
    }
    lua_pop(L, 1);
    if (lua_isnil(L, -1))
        return knurl_reply_status(call->reply, (int)status_code, call->error);
    if (lua_type(L, -1) != LUA_TSTRING)
        return rl_fail(call->error, KNURL_STATE, "response body must be bytes");
    bytes.data = lua_tolstring(L, -1, &bytes.length);
    return knurl_reply_bytes(call->reply, (int)status_code,
                             type != NULL ? type : "text/plain; charset=utf-8",
                             bytes, call->error);
}

static int invoke_handler(lua_State *L)
{
    struct request_call *call = lua_touserdata(L, 1);
    lua_rawgeti(L, 3, call->app->reference);
    lua_getiuservalue(L, -1, 1);
    push_request(L, call->request);
    lua_call(L, 1, 1);
    call->status = stage_response(L, call);
    return 0;
}

static knurl_status application_handler(const knurl_request_snapshot *request,
                                        knurl_source *source,
                                        knurl_reply *reply, void *data,
                                        knurl_error *error)
{
    const struct application *app = data;
    lua_State *L = app->server->worker;
    struct request_call call;
    struct reader *reader;
    int result;

    if (!lua_checkstack(L, 5))
        return rl_fail(error, KNURL_NOMEM,
                       "reserving application callback stack");
    lua_pushcfunction(L, new_reader);
    result = lua_pcall(L, 0, 1, 0);
    if (result != LUA_OK) {
        knurl_status status = lua_failure(L, result, "lua_handler", error);
        lua_settop(L, 1);
        return status;
    }

    memset(&call, 0, sizeof(call));
    call.app = app;
    call.request = request;
    call.reply = reply;
    call.error = error;
    /* Anchor the reader below the protected call so it survives error unwind.
     */
    reader = lua_touserdata(L, 2);
    reader->source = source;
    lua_pushcfunction(L, invoke_handler);
    lua_pushlightuserdata(L, &call);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 1);
    result = lua_pcall(L, 3, 0, 0);
    reader->source = NULL;
    rl_json_free(call.json);
    if (result != LUA_OK)
        call.status = lua_failure(L, result, "lua_handler", error);
    lua_settop(L, 1);
    return call.status;
}

static int register_application(lua_State *L)
{
    static const char *const fields[] = {
        "path", "path_kind", "method",         "methods",
        "acl",  "handler",   "max_body_bytes", "buffer_bytes"};
    struct rl_server *server = luaL_testudata(L, 1, RL_SERVER);
    struct rl_group *group = luaL_testudata(L, 1, RL_GROUP);
    struct application *app;
    knurl_route_config path;
    knurl_blocking_route_config route;
    knurl_error error;
    knurl_status status;
    int callback, owner = 1, callbacks;

    if (group != NULL) {
        server = group->server;
        owner = lua_gettop(L) + 1;
        lua_getiuservalue(L, 1, 1);
    }
    if (server == NULL || server->native == NULL || server->running)
        return rl_result(
            L,
            rl_fail(&error, KNURL_STATE,
                    "application routes require an inactive server"),
            &error);
    if (!rl_fields(L, 2, fields, RL_COUNT(fields), &error))
        return rl_error(L, &error);
    knurl_route_config_init(&path);
    knurl_blocking_route_config_init(&route);
    if (!rl_route_path(L, 2, &path) ||
        !rl_methods(L, 2, KNURL_HTTP_METHODS_GET, &route.methods) ||
        !rl_field_size(L, 2, "buffer_bytes", &route.buffer_bytes))
        return rl_result(
            L,
            rl_fail(&error, KNURL_INVALID, "invalid application route fields"),
            &error);
    if (!rl_body_limit(L, 2, server, &route.body.max_bytes, &error) ||
        !rl_acl_option(L, 2, &route.acl, &error))
        return rl_error(L, &error);
    route.path = path.path;
    route.path_kind = path.path_kind;
    rl_field(L, 2, "handler");
    if (!lua_isfunction(L, -1))
        return rl_result(
            L,
            rl_fail(&error, KNURL_INVALID,
                    "application route requires a handler function"),
            &error);
    callback = lua_absindex(L, -1);
    lua_getiuservalue(L, owner, 1);
    callbacks = lua_absindex(L, -1);
    app = lua_newuserdatauv(L, sizeof(*app), 1);
    app->server = server;
    lua_pushvalue(L, callback);
    lua_setiuservalue(L, -2, 1);
    app->reference = luaL_ref(L, callbacks);
    route.handler = application_handler;
    route.userdata = app;
    status =
        group != NULL
            ? knurl_route_group_blocking_route(group->native, &route, NULL,
                                               &error)
            : knurl_router_blocking_route(knurl_server_router(server->native),
                                          &route, NULL, &error);
    if (status == KNURL_OK)
        server->has_lua = 1;
    else
        luaL_unref(L, callbacks, app->reference);
    return rl_result(L, status, &error);
}

static int register_worker_hook(lua_State *L)
{
    struct rl_server *server = luaL_testudata(L, 1, RL_SERVER);
    knurl_error error;
    if (server == NULL || server->native == NULL || server->running)
        return rl_result(L,
                         rl_fail(&error, KNURL_STATE,
                                 "worker hooks require an inactive server"),
                         &error);
    if (!lua_isfunction(L, 2))
        return rl_result(
            L, rl_fail(&error, KNURL_INVALID, "worker hook must be a function"),
            &error);
    lua_getiuservalue(L, 1, 1);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 2);
    lua_rawset(L, -3);
    server->has_lua = 1;
    return rl_result(L, KNURL_OK, NULL);
}

void rl_application_open(lua_State *L, int group)
{
    lua_pushcfunction(L, register_application);
    lua_setfield(L, -2, "route");
    if (!group) {
        lua_pushliteral(L, WORKER_START);
        lua_pushcclosure(L, register_worker_hook, 1);
        lua_setfield(L, -2, WORKER_START);
        lua_pushliteral(L, WORKER_STOP);
        lua_pushcclosure(L, register_worker_hook, 1);
        lua_setfield(L, -2, WORKER_STOP);
    }
}
