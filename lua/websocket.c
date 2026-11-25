#include "binding.h"

#define RL_WEBSOCKET "knurl.websocket"

enum event {
    EVENT_IDLE,
    EVENT_OPEN,
    EVENT_MESSAGE,
    EVENT_WRITABLE,
    EVENT_CLOSE
};

static const char *const callback_names[] = {NULL, "on_open", "on_message",
                                             "on_writable", "on_close"};

struct websocket_route {
    struct rl_server *server;
    knurl_websocket_config config;
    int ref;
    int callbacks[RL_COUNT(callback_names)];
};

struct connection {
    struct rl_work work;
    struct rl_cleanup cleanup;
    struct websocket_route *route;
    knurl_websocket *native;
    knurl_request_snapshot *request;
    unsigned int refs;

    enum event event;
    int writable, held, consumed;
    int handle_ref;
    knurl_status callback_failure;

    unsigned char *message;
    size_t length, capacity;
    knurl_websocket_opcode opcode;
    knurl_websocket_outcome outcome;
    char reason[123];
};

struct command {
    struct connection *ws;
    knurl_websocket_opcode opcode;
    unsigned short code;
    knurl_bytes bytes;
};

static void release(struct connection *ws)
{
    if (__atomic_sub_fetch(&ws->refs, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    knurl_request_snapshot_destroy(ws->request);
    free(ws->message);
    free(ws);
}

static int gc(lua_State *L)
{
    struct connection **handle = luaL_checkudata(L, 1, RL_WEBSOCKET);
    if (*handle != NULL) {
        release(*handle);
        *handle = NULL;
    }
    return 0;
}

static knurl_status execute(void *data, knurl_error *error)
{
    const struct command *command = data;
    knurl_websocket *native = command->ws->native;
    if (native == NULL)
        return rl_fail(error, KNURL_STATE, "WebSocket is closed");
    if (command->opcode == KNURL_WEBSOCKET_CLOSE)
        return knurl_websocket_close(native, command->code, command->bytes,
                                     error);
    return knurl_websocket_send(native, command->opcode, command->bytes.data,
                                command->bytes.length, error);
}

static int websocket_command(lua_State *L)
{
    struct connection **handle = luaL_checkudata(L, 1, RL_WEBSOCKET);
    struct command command;
    knurl_error error;
    knurl_task *task;
    knurl_status status;
    lua_Integer code;
    if (*handle == NULL)
        return rl_result(L, rl_fail(&error, KNURL_STATE, "WebSocket is closed"),
                         &error);
    command.ws = *handle;
    task = command.ws->route->server->websocket_task;
    if (task == NULL)
        return rl_result(L,
                         rl_fail(&error, KNURL_STATE,
                                 "sending requires a WebSocket callback"),
                         &error);
    command.opcode =
        (knurl_websocket_opcode)lua_tointeger(L, lua_upvalueindex(1));
    command.code = 0;
    command.bytes.data = NULL;
    command.bytes.length = 0;
    if (command.opcode == KNURL_WEBSOCKET_CLOSE) {
        code = luaL_optinteger(L, 2, 1000);
        luaL_argcheck(L, code >= 0 && code <= 4999, 2, "invalid close code");
        command.code = (unsigned short)code;
        command.bytes.data = luaL_optlstring(L, 3, "", &command.bytes.length);
    } else
        command.bytes.data = luaL_checklstring(L, 2, &command.bytes.length);
    status = knurl_task_call(task, execute, &command, &error);
    return rl_result(L, status, &error);
}

static void push_handle(lua_State *L, struct connection *ws)
{
    struct connection **handle;
    if (ws->handle_ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ws->handle_ref);
        return;
    }
    handle = lua_newuserdatauv(L, sizeof(*handle), 1);
    *handle = ws;
    (void)__atomic_add_fetch(&ws->refs, 1, __ATOMIC_RELAXED);
    luaL_setmetatable(L, RL_WEBSOCKET);
    rl_push_request(L, ws->request, 0);
    lua_setiuservalue(L, -2, 1);
    lua_pushvalue(L, -1);
    ws->handle_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

static int websocket_request(lua_State *L)
{
    luaL_checkudata(L, 1, RL_WEBSOCKET);
    lua_getiuservalue(L, 1, 1);
    return 1;
}

static int invoke(lua_State *L)
{
    struct connection *ws = lua_touserdata(L, 1);
    int args = 1;
    lua_rawgeti(L, 2, ws->route->ref);
    rl_field(L, -1, callback_names[ws->event]);
    if (lua_isnil(L, -1))
        return 0;
    push_handle(L, ws);
    if (ws->event == EVENT_OPEN) {
        lua_getiuservalue(L, -1, 1);
        ++args;
    } else if (ws->event == EVENT_MESSAGE) {
        lua_pushlstring(L, (const char *)ws->message, ws->length);
        lua_pushstring(L,
                       ws->opcode == KNURL_WEBSOCKET_TEXT ? "text" : "binary");
        args += 2;
    } else if (ws->event == EVENT_CLOSE) {
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, ws->outcome.peer_code);
        lua_setfield(L, -2, "peer_code");
        lua_pushinteger(L, ws->outcome.local_code);
        lua_setfield(L, -2, "local_code");
        lua_pushboolean(L, ws->outcome.clean);
        lua_setfield(L, -2, "clean");
        lua_pushboolean(L, ws->outcome.opened);
        lua_setfield(L, -2, "opened");
        lua_pushstring(L, knurl_status_string(ws->outcome.cause));
        lua_setfield(L, -2, "cause");
        lua_pushlstring(L, ws->reason, ws->outcome.peer_reason.length);
        lua_setfield(L, -2, "peer_reason");
        ++args;
    }
    lua_call(L, args, 0);
    return 0;
}

static knurl_status work(knurl_task *task, struct rl_work *job,
                         knurl_error *error)
{
    struct connection *ws = (struct connection *)job;
    struct rl_server *server = ws->route->server;
    lua_State *L = server->worker;
    knurl_status status = KNURL_OK;
    int result;
    if (!lua_checkstack(L, 3))
        return rl_fail(error, KNURL_NOMEM,
                       "reserving WebSocket callback stack");
    server->websocket_task = task;
    lua_pushcfunction(L, invoke);
    lua_pushlightuserdata(L, ws);
    lua_pushvalue(L, 1);
    result = lua_pcall(L, 2, 0, 0);
    server->websocket_task = NULL;
    if (result != LUA_OK)
        status = rl_lua_failure(L, result, callback_names[ws->event], error);
    if (ws->event == EVENT_CLOSE) {
        luaL_unref(L, LUA_REGISTRYINDEX, ws->handle_ref);
        ws->handle_ref = LUA_NOREF;
    }
    lua_settop(L, 1);
    return status;
}

static knurl_status schedule(struct connection *, knurl_error *);

static void drop_handle(lua_State *L, struct rl_cleanup *cleanup)
{
    struct connection *ws =
        (struct connection *)((char *)cleanup -
                              offsetof(struct connection, cleanup));
    if (L != NULL)
        luaL_unref(L, LUA_REGISTRYINDEX, ws->handle_ref);
    ws->handle_ref = LUA_NOREF;
    release(ws);
}

static void dispose(struct connection *ws)
{
    if (ws->route->callbacks[EVENT_CLOSE])
        --ws->route->server->websocket_close_reservations;
    knurl_request_snapshot_destroy(ws->request);
    ws->request = NULL;
    free(ws->message);
    ws->message = NULL;
    if (ws->handle_ref != LUA_NOREF)
        rl_cleanup_later(ws->work.server, &ws->cleanup, drop_handle);
    else
        release(ws);
}

static void schedule_close(struct connection *ws)
{
    ws->event = EVENT_CLOSE;
    /* This admission was reserved before accepting the handshake. */
    ws->work.terminal = 1;
    if (!ws->route->callbacks[EVENT_CLOSE] || schedule(ws, NULL) != KNURL_OK)
        dispose(ws);
}

static void completed(struct connection *ws, knurl_status status)
{
    enum event event = ws->event;
    ws->event = EVENT_IDLE;
    if (event == EVENT_CLOSE) {
        dispose(ws);
        return;
    }
    if (ws->handle_ref != LUA_NOREF) {
        knurl_request_snapshot_destroy(ws->request);
        ws->request = NULL;
    }
    if (ws->native == NULL) {
        schedule_close(ws);
        return;
    }
    if (status != KNURL_OK) {
        knurl_bytes reason = {NULL, 0};
        ws->callback_failure = status;
        (void)knurl_websocket_close(ws->native, 1011, reason, NULL);
        return;
    }
    if (event == EVENT_MESSAGE) {
        free(ws->message);
        ws->message = NULL;
        ws->length = ws->capacity = 0;
        ws->consumed = 1;
    }
    if (ws->writable) {
        ws->writable = 0;
        ws->event = EVENT_WRITABLE;
        if (schedule(ws, NULL) != KNURL_OK)
            completed(ws, KNURL_STATE);
    } else if (ws->held)
        (void)knurl_websocket_resume(ws->native, NULL);
}

static void retired(struct rl_work *job, knurl_status status,
                    const knurl_error *error)
{
    (void)error;
    completed((struct connection *)job, status);
}

static knurl_status schedule(struct connection *ws, knurl_error *error)
{
    return rl_work_start(&ws->work, error);
}

static knurl_status opened(knurl_websocket *socket, void *data,
                           knurl_error *error)
{
    struct connection *ws = data;
    (void)socket;
    if (!ws->route->callbacks[EVENT_OPEN])
        return KNURL_OK;
    ws->event = EVENT_OPEN;
    {
        knurl_status status = schedule(ws, error);
        if (status != KNURL_OK)
            ws->event = EVENT_IDLE;
        return status;
    }
}

static knurl_status frame(knurl_websocket *socket,
                          knurl_websocket_opcode opcode, int final,
                          knurl_bytes bytes, void *data, knurl_error *error)
{
    struct connection *ws = data;
    unsigned char *buffer;
    size_t n, limit;
    knurl_status status;
    (void)socket;
    if (!ws->route->callbacks[EVENT_MESSAGE])
        return KNURL_OK;
    ws->held = ws->event != EVENT_IDLE;
    if (ws->held)
        return KNURL_AGAIN;
    if (ws->consumed) {
        ws->consumed = 0;
        return KNURL_OK;
    }
    if (opcode != KNURL_WEBSOCKET_CONTINUATION)
        ws->opcode = opcode;
    limit = ws->route->config.max_message_bytes;
    if (bytes.length > limit - ws->length)
        return rl_fail(error, KNURL_LIMIT, "WebSocket message too large");
    n = ws->length + bytes.length;
    if (n > ws->capacity) {
        size_t capacity = ws->capacity > limit / 2 ? limit : ws->capacity * 2;
        if (capacity < n)
            capacity = n;
        buffer = realloc(ws->message, capacity);
        if (buffer == NULL)
            return rl_fail(error, KNURL_NOMEM, "retaining WebSocket message");
        ws->message = buffer;
        ws->capacity = capacity;
    }
    if (bytes.length != 0)
        memcpy(ws->message + ws->length, bytes.data, bytes.length);
    ws->length = n;
    if (!final)
        return KNURL_OK;
    ws->event = EVENT_MESSAGE;
    status = schedule(ws, error);
    if (status != KNURL_OK) {
        ws->event = EVENT_IDLE;
        return status;
    }
    ws->held = 1;
    return KNURL_AGAIN;
}

static void writable(knurl_websocket *socket, void *data)
{
    struct connection *ws = data;
    (void)socket;
    if (ws->event != EVENT_IDLE) {
        ws->writable = 1;
        return;
    }
    ws->event = EVENT_WRITABLE;
    if (schedule(ws, NULL) != KNURL_OK)
        completed(ws, KNURL_STATE);
}

static void closed(knurl_websocket *socket,
                   const knurl_websocket_outcome *outcome, void *data)
{
    struct connection *ws = data;
    (void)socket;
    ws->native = NULL;
    ws->outcome = *outcome;
    if (ws->outcome.cause == KNURL_OK && ws->callback_failure != KNURL_OK)
        ws->outcome.cause = ws->callback_failure;
    if (outcome->peer_reason.length)
        memcpy(ws->reason, outcome->peer_reason.data,
               outcome->peer_reason.length);
    ws->outcome.peer_reason.data = ws->reason;
    if (ws->event == EVENT_IDLE)
        schedule_close(ws);
}

static knurl_status reject(knurl_request *request, knurl_request_plan *plan,
                           unsigned short code, const char *message,
                           knurl_error *error)
{
    knurl_bytes body;
    knurl_status status;
    body.data = message;
    body.length = strlen(message);
    status = knurl_response_bytes(knurl_request_response(request), code,
                                  "text/plain", body, error);
    if (status == KNURL_OK)
        plan->disposition = KNURL_REQUEST_REPLY;
    return status;
}

static knurl_status accept_websocket(knurl_request *request,
                                     knurl_request_plan *plan, void *data,
                                     knurl_error *error)
{
    struct websocket_route *route = data;
    struct connection *ws = NULL;
    knurl_websocket *native;
    knurl_websocket_config config = route->config;
    struct rl_server *server = route->server;
    knurl_status status;
    if (route->callbacks[EVENT_CLOSE] &&
        server->websocket_close_reservations >= server->max_connections)
        return reject(request, plan, 503,
                      "WebSocket close callback capacity exhausted\n", error);
    if (config.on_close != NULL) {
        ws = calloc(1, sizeof(*ws));
        if (ws == NULL)
            return rl_fail(error, KNURL_NOMEM,
                           "allocating WebSocket connection");
        ws->route = route;
        ws->refs = 1;
        ws->work.server = route->server;
        ws->work.run = work;
        ws->work.complete = retired;
        ws->handle_ref = LUA_NOREF;
        config.userdata = ws;
        status = knurl_request_snapshot_copy(request, &ws->request, error);
        if (status != KNURL_OK) {
            release(ws);
            return status;
        }
    }
    status = knurl_websocket_accept(request, &config, &native, error);
    if (status == KNURL_OK) {
        if (ws != NULL) {
            ws->native = native;
            if (route->callbacks[EVENT_CLOSE])
                ++server->websocket_close_reservations;
        }
        plan->disposition = KNURL_REQUEST_WEBSOCKET;
        return status;
    }
    if (ws != NULL)
        release(ws);
    if (status == KNURL_INVALID)
        return reject(request, plan, 400, "invalid WebSocket handshake\n",
                      error);
    return status;
}

static int register_websocket(lua_State *L)
{
    static const char *const fields[] = {"path",
                                         "path_kind",
                                         "acl",
                                         "auth",
                                         "hooks",
                                         "on_open",
                                         "on_message",
                                         "on_writable",
                                         "on_close",
                                         "max_frame_bytes",
                                         "max_message_bytes",
                                         "output_queue_bytes",
                                         "close_timeout_ms",
                                         "protocol"};
    struct rl_server *server = luaL_testudata(L, 1, RL_SERVER);
    struct rl_group *group = luaL_testudata(L, 1, RL_GROUP);
    struct websocket_route *ws;
    struct rl_route *handle;
    knurl_route_config route;
    knurl_async_route_hooks hooks;
    knurl_auth *auth;
    knurl_error error;
    knurl_status status;
    size_t timeout = 0, i;
    int root = 1, callbacks, ctx, result;
    if (group != NULL) {
        server = group->server;
        root = lua_gettop(L) + 1;
        lua_getiuservalue(L, 1, 1);
    }
    if (server == NULL || server->native == NULL || server->running)
        return rl_result(
            L, rl_fail(&error, KNURL_STATE, "inactive server required"),
            &error);
    if (!rl_fields(L, 2, fields, RL_COUNT(fields), &error))
        return rl_error(L, &error);
    knurl_route_config_init(&route);
    if (!rl_route_path(L, 2, &route))
        return rl_result(
            L, rl_fail(&error, KNURL_INVALID, "invalid WebSocket path"),
            &error);
    if (!rl_acl_option(L, 2, &route.acl, &error) ||
        !rl_auth_option(L, 2, &auth, &error))
        return rl_error(L, &error);
    handle = rl_route_new(L, server);
    result = lua_gettop(L);
    lua_getiuservalue(L, root, 1);
    callbacks = lua_absindex(L, -1);
    ws = lua_newuserdatauv(L, sizeof(*ws), 0);
    memset(ws, 0, sizeof(*ws));
    ws->server = server;
    knurl_websocket_config_init(&ws->config);
    ws->config.max_frame_bytes = server->websocket_max_frame_bytes;
    if (!rl_field_size(L, 2, "max_frame_bytes", &ws->config.max_frame_bytes) ||
        ws->config.max_frame_bytes > server->websocket_max_frame_bytes ||
        !rl_field_size(L, 2, "max_message_bytes",
                       &ws->config.max_message_bytes) ||
        !rl_field_size(L, 2, "output_queue_bytes",
                       &ws->config.output_queue_bytes) ||
        !rl_field_size(L, 2, "close_timeout_ms", &timeout) ||
        timeout > LONG_MAX ||
        !rl_field_string(L, 2, "protocol", &ws->config.protocol))
        return rl_result(L,
                         rl_fail(&error, KNURL_INVALID,
                                 "invalid WebSocket limits or protocol"),
                         &error);
    if (!ws->config.max_message_bytes)
        ws->config.max_message_bytes = ws->config.max_frame_bytes != 0
                                           ? ws->config.max_frame_bytes
                                           : server->websocket_max_frame_bytes;
    if (timeout)
        ws->config.close_timeout_ms = (long)timeout;
    lua_createtable(L, 0, 6);
    ctx = lua_absindex(L, -1);
    lua_pushvalue(L, -2);
    lua_setfield(L, ctx, "context");
    rl_field(L, 2, "protocol");
    lua_setfield(L, ctx, "protocol");
    for (i = 1; i < RL_COUNT(callback_names); ++i) {
        rl_field(L, 2, callback_names[i]);
        if (!lua_isnil(L, -1) && !lua_isfunction(L, -1))
            return rl_result(L,
                             rl_fail(&error, KNURL_INVALID,
                                     "WebSocket callbacks must be functions"),
                             &error);
        ws->callbacks[i] = lua_isfunction(L, -1);
        lua_setfield(L, ctx, callback_names[i]);
    }
    if (!rl_hooks_prepare(L, server, &hooks, &error))
        return rl_error(L, &error);
    ws->ref = luaL_ref(L, callbacks);
    if (ws->callbacks[EVENT_OPEN])
        ws->config.on_open = opened;
    if (ws->callbacks[EVENT_MESSAGE])
        ws->config.on_frame = frame;
    if (ws->callbacks[EVENT_WRITABLE])
        ws->config.on_writable = writable;
    if (ws->config.on_open || ws->config.on_frame || ws->config.on_writable ||
        ws->callbacks[EVENT_CLOSE])
        ws->config.on_close = closed;
    route.method = KNURL_HTTP_GET;
    route.on_headers = accept_websocket;
    route.userdata = ws;
    status = group == NULL
                 ? knurl_router_route(knurl_server_router(server->native),
                                      &route, &handle->native, &error)
                 : knurl_route_group_route(group->native, &route,
                                           &handle->native, &error);
    if (status != KNURL_OK) {
        rl_hooks_finish(L, &hooks, status);
        luaL_unref(L, callbacks, ws->ref);
        return rl_error(L, &error);
    }
    if (auth != NULL)
        status = knurl_route_set_auth(handle->native, auth, &error);
    if (status == KNURL_OK && hooks.userdata != NULL)
        status = knurl_route_set_async_hooks(handle->native, &hooks, &error);
    rl_hooks_finish(L, &hooks, status);
    if (status != KNURL_OK)
        return rl_error(L, &error);
    if (ws->config.on_close != NULL)
        server->has_lua = 1;
    lua_pushvalue(L, result);
    return 1;
}

void rl_websocket_open(lua_State *L, int group)
{
    static const char *const methods[] = {"send_text", "send_binary", "close"};
    static const knurl_websocket_opcode opcodes[] = {
        KNURL_WEBSOCKET_TEXT, KNURL_WEBSOCKET_BINARY, KNURL_WEBSOCKET_CLOSE};
    size_t i;
    lua_pushcfunction(L, register_websocket);
    lua_setfield(L, -2, "websocket");
    if (!group && luaL_newmetatable(L, RL_WEBSOCKET)) {
        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");
        lua_createtable(L, 0, 4);
        for (i = 0; i < RL_COUNT(methods); ++i) {
            lua_pushinteger(L, opcodes[i]);
            lua_pushcclosure(L, websocket_command, 1);
            lua_setfield(L, -2, methods[i]);
        }
        lua_pushcfunction(L, websocket_request);
        lua_setfield(L, -2, "request");
        lua_setfield(L, -2, "__index");
    } else if (group)
        return;
    lua_pop(L, 1);
}
