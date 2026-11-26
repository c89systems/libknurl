#include "binding.h"

enum phase {
    HOOK_AUTH,
    HOOK_BEFORE,
    HOOK_AFTER,
    PROXY_PREFLIGHT,
    PROXY_REWRITE,
    PROXY_RESPONSE,
    PROXY_ERROR,
    PROXY_EXCHANGE
};

static const char *const names[] = {
    "auth",    "before",          "after",    "preflight",
    "rewrite", "modify_response", "on_error", "on_exchange"};

struct hooks {
    struct rl_server *server;
    int ref;
    int enabled[RL_COUNT(names)];
};

struct hook_call {
    struct rl_work work;
    const struct hooks *hooks;
    knurl_request *request;
    knurl_request_snapshot *snapshot;
    knurl_reply *headers, *reply, *remove_headers;
    struct rl_proxy_rewrite *rewrite;
    knurl_error gateway_error;
    knurl_proxy_exchange exchange;
    knurl_timer *deadline;
    unsigned int refs;
    enum phase phase;
    int code;
    knurl_status status;
    knurl_error error;
};

static void release(void *data)
{
    struct hook_call *call = data;
    if (--call->refs != 0)
        return;
    knurl_request_snapshot_destroy(call->snapshot);
    (void)knurl_reply_destroy(call->headers, NULL);
    (void)knurl_reply_destroy(call->reply, NULL);
    (void)knurl_reply_destroy(call->remove_headers, NULL);
    rl_proxy_rewrite_free(call->rewrite);
    free((void *)call->exchange.route);
    free((void *)call->exchange.upstream);
    free((void *)call->exchange.kind);
    free((void *)call->exchange.disconnect_side);
    free((void *)call->exchange.error_category);
    free(call);
}

static void push_response(lua_State *L, const struct hook_call *call)
{
    size_t i, count = knurl_reply_header_count(call->headers);
    knurl_header header;
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, call->code);
    lua_setfield(L, -2, "status");
    lua_createtable(L, (int)count, 0);
    for (i = 0; i < count; ++i) {
        (void)knurl_reply_header_at(call->headers, i, &header, NULL);
        lua_createtable(L, 0, 2);
        lua_pushlstring(L, header.name.data, header.name.length);
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, header.value.data, header.value.length);
        lua_setfield(L, -2, "value");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "headers");
}

static knurl_status read_result(lua_State *L, struct hook_call *call)
{
    static const char *const before[] = {"status", "headers", "body",
                                         "content_type"};
    static const char *const after[] = {"status", "headers"};
    knurl_status status;
    int metadata = call->phase == HOOK_AFTER || call->phase == PROXY_RESPONSE;
    size_t code = metadata                     ? 0
                  : call->phase == PROXY_ERROR ? (size_t)call->code
                                               : 403;
    const char *type = "text/plain; charset=utf-8", *name, *value;
    knurl_bytes bytes = {NULL, 0};
    size_t namelen, valuelen, budget = 16384;
    int response = lua_absindex(L, -1), remove;
    if (lua_isnil(L, -1) ||
        (call->phase == HOOK_AUTH && lua_isboolean(L, -1))) {
        if (call->phase == HOOK_AUTH && !lua_toboolean(L, -1))
            return knurl_reply_status(call->reply, 403, &call->error);
        return KNURL_OK;
    }
    if (!rl_fields(L, response, metadata ? after : before,
                   metadata ? RL_COUNT(after) : RL_COUNT(before), &call->error))
        return call->error.status;
    if (!rl_field_size(L, response, "status", &code) ||
        (code != 0 && (code < 200 || code > 599)))
        return rl_fail(&call->error, KNURL_INVALID,
                       "invalid hook response status");
    rl_field(L, response, "headers");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            return rl_fail(&call->error, KNURL_INVALID,
                           "hook headers must be a table");
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            remove = metadata && lua_isboolean(L, -1) && !lua_toboolean(L, -1);
            if (lua_type(L, -2) != LUA_TSTRING ||
                (!remove && lua_type(L, -1) != LUA_TSTRING))
                return rl_fail(
                    &call->error, KNURL_INVALID,
                    "hook header values must be strings or false to remove");
            name = lua_tolstring(L, -2, &namelen);
            valuelen = 0;
            value = remove ? "" : lua_tolstring(L, -1, &valuelen);
            if (budget < 4 || namelen > budget - 4 ||
                valuelen > budget - 4 - namelen)
                return rl_fail(&call->error, KNURL_LIMIT,
                               "hook header edits exceed 16 KiB");
            budget -= namelen + valuelen + 4;
            if (memchr(name, 0, namelen) || memchr(value, 0, valuelen))
                return rl_fail(&call->error, KNURL_INVALID,
                               "hook headers contain NUL");
            if (remove && call->remove_headers == NULL) {
                status = knurl_reply_new(&call->remove_headers, &call->error);
                if (status != KNURL_OK)
                    return status;
            }
            status = knurl_reply_header_set(remove ? call->remove_headers
                                                   : call->reply,
                                            name, value, &call->error);
            if (status != KNURL_OK)
                return status;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (metadata)
        return code == 0
                   ? KNURL_OK
                   : knurl_reply_status(call->reply, (int)code, &call->error);
    rl_field(L, response, "body");
    if (lua_isnil(L, -1))
        return knurl_reply_status(call->reply, (int)code, &call->error);
    if (lua_type(L, -1) != LUA_TSTRING ||
        !rl_field_string(L, response, "content_type", &type))
        return rl_fail(&call->error, KNURL_INVALID,
                       "invalid hook response body or content_type");
    bytes.data = lua_tolstring(L, -1, &bytes.length);
    if (bytes.length > 65536)
        return rl_fail(&call->error, KNURL_LIMIT,
                       "hook response body exceeds 64 KiB");
    return knurl_reply_bytes(call->reply, (int)code, type, bytes, &call->error);
}

static void wide_field(lua_State *L, const char *name, knurl_u64 value)
{
    const lua_Unsigned signed_max = ~(lua_Unsigned)0 >> 1;
    size_t n;
    char decimal[21];
    if (knurl_u64_to_size(value, &n, NULL) == KNURL_OK &&
        n <= (size_t)signed_max)
        lua_pushinteger(L, (lua_Integer)n);
    else {
        (void)knurl_u64_format_decimal(value, decimal, sizeof(decimal), &n,
                                       NULL);
        lua_pushlstring(L, decimal, n);
    }
    lua_setfield(L, -2, name);
}

static void text_field(lua_State *L, const char *name, const char *value)
{
    lua_pushstring(L, value);
    lua_setfield(L, -2, name);
}

static void push_exchange(lua_State *L, const knurl_proxy_exchange *exchange)
{
    lua_createtable(L, 0, 11);
    wide_field(L, "request_id", exchange->request_id);
    wide_field(L, "upstream_bytes", exchange->upstream_bytes);
    wide_field(L, "downstream_bytes", exchange->downstream_bytes);
    wide_field(L, "duration_ms", exchange->duration_ms);
    text_field(L, "route", exchange->route);
    text_field(L, "upstream", exchange->upstream);
    text_field(L, "kind", exchange->kind);
    text_field(L, "disconnect_side", exchange->disconnect_side);
    text_field(L, "error_category", exchange->error_category);
    text_field(L, "cause", knurl_status_string(exchange->cause));
    lua_pushinteger(L, exchange->status);
    lua_setfield(L, -2, "status");
}

static int invoke(lua_State *L)
{
    struct hook_call *call = lua_touserdata(L, 1);
    int argc = 1, config, request;
    lua_rawgeti(L, 2, call->hooks->ref);
    config = lua_absindex(L, -1);
    rl_push_request(L, call->snapshot, 0);
    request = lua_absindex(L, -1);
    if (call->phase == PROXY_PREFLIGHT &&
        call->hooks->enabled[PROXY_PREFLIGHT]) {
        rl_field(L, config, names[PROXY_PREFLIGHT]);
        lua_pushvalue(L, request);
        lua_call(L, 1, 1);
        call->status = read_result(L, call);
        lua_pop(L, 1);
        if (call->status != KNURL_OK || knurl_reply_is_selected(call->reply))
            return 0;
    }
    if (call->phase == PROXY_PREFLIGHT || call->phase == PROXY_REWRITE) {
        if (call->hooks->enabled[PROXY_REWRITE]) {
            rl_field(L, config, names[PROXY_REWRITE]);
            lua_pushvalue(L, request);
            lua_call(L, 1, 1);
            call->status =
                rl_proxy_rewrite_read(L, -1, &call->rewrite, &call->error);
        }
        return 0;
    }
    rl_field(L, config, names[call->phase]);
    lua_pushvalue(L, request);
    if (call->phase == PROXY_EXCHANGE) {
        push_exchange(L, &call->exchange);
        lua_call(L, 2, 0);
        return 0;
    }
    if (call->phase == HOOK_AFTER || call->phase == PROXY_RESPONSE) {
        push_response(L, call);
        ++argc;
    } else if (call->phase == PROXY_ERROR) {
        rl_push_error(L, &call->gateway_error);
        lua_pushinteger(L, call->code);
        argc += 2;
    }
    lua_call(L, argc, 1);
    call->status = read_result(L, call);
    return 0;
}

static knurl_status work(knurl_task *task, struct rl_work *job,
                         knurl_error *error)
{
    struct hook_call *call = (struct hook_call *)job;
    lua_State *L = call->hooks->server->worker;
    int result;
    (void)task;
    if (!lua_checkstack(L, 3))
        return rl_fail(error, KNURL_NOMEM, "reserving route hook stack");
    lua_pushcfunction(L, invoke);
    lua_pushlightuserdata(L, call);
    lua_pushvalue(L, 1);
    result = lua_pcall(L, 2, 0, 0);
    if (result != LUA_OK)
        call->status =
            rl_lua_failure(L, result, names[call->phase], &call->error);
    lua_settop(L, 1);
    *error = call->error;
    return call->status;
}

static void revoke(void *data)
{
    struct hook_call *call = data;
    call->request = NULL;
    knurl_timer_cancel(call->deadline);
    call->deadline = NULL;
    rl_work_cancel(&call->work);
}

static void completed(struct rl_work *job, knurl_status status,
                      const knurl_error *error)
{
    struct hook_call *call = (struct hook_call *)job;
    knurl_request *request = call->request;
    if (error != NULL &&
        (call->phase == PROXY_ERROR || call->phase == PROXY_EXCHANGE) &&
        strcmp(error->operation, names[call->phase]) == 0)
        knurl_server_log_error(job->server->native, "lua_proxy", error);
    if (request != NULL) {
        knurl_reply *reply = call->reply;
        knurl_header field;
        size_t i;
        knurl_error edit_error;
        for (i = 0; status == KNURL_OK &&
                    i < knurl_reply_header_count(call->remove_headers);
             ++i) {
            (void)knurl_reply_header_at(call->remove_headers, i, &field, NULL);
            status = knurl_request_hook_remove_header(request, field.name.data,
                                                      &edit_error);
            if (status != KNURL_OK)
                error = &edit_error;
        }
        if (status == KNURL_OK && call->rewrite != NULL) {
            status = knurl_request_proxy_rewrite_take(
                request, rl_proxy_rewrite_apply, call->rewrite,
                rl_proxy_rewrite_free, &edit_error);
            if (status == KNURL_OK)
                call->rewrite = NULL;
            else
                error = &edit_error;
        }
        if (call->phase == PROXY_ERROR) {
            if (status == KNURL_OK && knurl_reply_is_selected(reply))
                (void)knurl_request_proxy_gateway_reply(request, reply, NULL);
            status = KNURL_OK;
            reply = NULL;
        }
        revoke(call);
        (void)knurl_request_hook_resume(request, status, reply, error, NULL);
    }
    release(call);
}

static void expired(knurl_timer *timer, void *data)
{
    struct hook_call *call = data;
    knurl_request *request = call->request;
    (void)timer;
    if (request != NULL) {
        revoke(call);
        (void)knurl_request_hook_resume(
            request, call->phase == PROXY_ERROR ? KNURL_OK : KNURL_TIMEOUT,
            NULL, NULL, NULL);
    }
}

static knurl_status dispatch(knurl_request *request, const knurl_reply *headers,
                             int code, void *data, enum phase phase,
                             knurl_error *error)
{
    struct hook_call *call = calloc(1, sizeof(*call));
    const struct hooks *hooks = data;
    knurl_status status;
    knurl_header header;
    size_t i;
    long timeout = hooks->server->pool.timeout_ms;
    if (call == NULL)
        return rl_fail(error, KNURL_NOMEM, "allocating route hook");
    call->refs = 1;
    call->hooks = hooks;
    call->phase = phase;
    call->code = code;
    if (phase == PROXY_ERROR)
        call->gateway_error = *knurl_request_proxy_gateway_error(request);
    call->request = request;
    call->work.server = hooks->server;
    call->work.run = work;
    call->work.complete = completed;
    status = knurl_request_snapshot_copy(request, &call->snapshot, error);
    if (status == KNURL_OK)
        status = knurl_reply_new(&call->reply, error);
    if (status == KNURL_OK && headers != NULL) {
        status = knurl_reply_new(&call->headers, error);
        for (i = 0; status == KNURL_OK && i < knurl_reply_header_count(headers);
             ++i) {
            (void)knurl_reply_header_at(headers, i, &header, NULL);
            status = knurl_reply_header_add(call->headers, header.name.data,
                                            header.value.data, error);
        }
    }
    if (status == KNURL_OK)
        status = rl_work_start(&call->work, error);
    if (status != KNURL_OK) {
        release(call);
        return status;
    }
    status =
        knurl_loop_timer(knurl_request_loop(request), timeout ? timeout : 30000,
                         0, expired, release, call, &call->deadline, error);
    if (status == KNURL_OK) {
        ++call->refs;
        status = knurl_request_hook_wait(request, revoke, call, error);
    }
    if (status != KNURL_OK) {
        revoke(call);
        return status;
    }
    return KNURL_AGAIN;
}

static knurl_status auth(knurl_request *req, const knurl_reply *head, int code,
                         void *data, knurl_error *error)
{
    return dispatch(req, head, code, data, HOOK_AUTH, error);
}

static knurl_status before(knurl_request *req, const knurl_reply *head,
                           int code, void *data, knurl_error *error)
{
    return dispatch(req, head, code, data, HOOK_BEFORE, error);
}

static knurl_status after(knurl_request *req, const knurl_reply *head, int code,
                          void *data, knurl_error *error)
{
    return dispatch(req, head, code, data, HOOK_AFTER, error);
}

static knurl_status proxy_before(knurl_request *req, const knurl_reply *head,
                                 int code, void *data, knurl_error *error)
{
    const struct hooks *hooks = data;
    return dispatch(req, head, code, data,
                    hooks->enabled[PROXY_PREFLIGHT] ? PROXY_PREFLIGHT
                                                    : PROXY_REWRITE,
                    error);
}

static knurl_status proxy_after(knurl_request *req, const knurl_reply *head,
                                int code, void *data, knurl_error *error)
{
    const struct hooks *hooks = data;
    if (hooks->enabled[PROXY_ERROR] &&
        knurl_request_proxy_gateway_error(req) != NULL) {
        knurl_status status =
            dispatch(req, NULL, code, data, PROXY_ERROR, error);
        return status == KNURL_AGAIN ? status : KNURL_OK;
    }
    if (hooks->enabled[PROXY_RESPONSE] &&
        knurl_request_proxy_upstream_response(req))
        return dispatch(req, head, code, data, PROXY_RESPONSE, error);
    return KNURL_OK;
}

static int copy_text(const char *value, const char **out)
{
    *out = value != NULL ? strdup(value) : NULL;
    return value == NULL || *out != NULL;
}

static void proxy_exchange(knurl_request *req,
                           const knurl_proxy_exchange *event, void *data)
{
    struct hook_call *call = calloc(1, sizeof(*call));
    const struct hooks *hooks = data;
    if (call == NULL)
        return;
    call->refs = 1;
    call->hooks = hooks;
    call->phase = PROXY_EXCHANGE;
    call->work.server = hooks->server;
    call->work.run = work;
    call->work.complete = completed;
    call->exchange.request_id = event->request_id;
    call->exchange.upstream_bytes = event->upstream_bytes;
    call->exchange.downstream_bytes = event->downstream_bytes;
    call->exchange.duration_ms = event->duration_ms;
    call->exchange.status = event->status;
    call->exchange.cause = event->cause;
    if (!copy_text(event->route, &call->exchange.route) ||
        !copy_text(event->upstream, &call->exchange.upstream) ||
        !copy_text(event->kind, &call->exchange.kind) ||
        !copy_text(event->disconnect_side, &call->exchange.disconnect_side) ||
        !copy_text(event->error_category, &call->exchange.error_category) ||
        knurl_request_snapshot_copy(req, &call->snapshot, NULL) != KNURL_OK ||
        rl_work_start(&call->work, NULL) != KNURL_OK)
        release(call);
}

static int callback_table(lua_State *L)
{
    lua_getiuservalue(L, 1, 1);
    if (luaL_testudata(L, 1, RL_GROUP))
        lua_getiuservalue(L, -1, 1);
    return lua_absindex(L, -1);
}

static struct hooks *anchor(lua_State *L, struct rl_server *server, int input,
                            size_t first, size_t end)
{
    int callbacks = callback_table(L);
    int copy;
    size_t i;
    struct hooks *hooks = lua_newuserdatauv(L, sizeof(*hooks), 0);
    memset(hooks, 0, sizeof(*hooks));
    hooks->server = server;
    lua_createtable(L, 0, (int)(end - first + 1));
    copy = lua_absindex(L, -1);
    lua_pushvalue(L, -2);
    lua_setfield(L, copy, "context");
    for (i = first; i < end; ++i) {
        rl_field(L, input, names[i]);
        hooks->enabled[i] = lua_isfunction(L, -1);
        lua_setfield(L, copy, names[i]);
    }
    hooks->ref = luaL_ref(L, callbacks);
    return hooks;
}

int rl_hooks_prepare(lua_State *L, struct rl_server *server,
                     knurl_async_route_hooks *config, knurl_error *error)
{
    static knurl_async_route_hook_fn const functions[] = {auth, before, after};
    knurl_async_route_hook_fn *slots[RL_COUNT(functions)];
    size_t i, count = 0;
    int top = lua_gettop(L), input;
    memset(config, 0, sizeof(*config));
    rl_field(L, 2, "hooks");
    input = lua_absindex(L, -1);
    if (lua_isnil(L, input)) {
        lua_settop(L, top);
        return 1;
    }
    if (!rl_fields(L, input, names, RL_COUNT(functions), error))
        goto invalid;
    slots[0] = &config->auth;
    slots[1] = &config->before;
    slots[2] = &config->after;
    for (i = 0; i < RL_COUNT(functions); ++i) {
        rl_field(L, input, names[i]);
        if (!lua_isnil(L, -1)) {
            if (!lua_isfunction(L, -1)) {
                rl_fail(error, KNURL_INVALID, "route hook must be a function");
                snprintf(error->detail, sizeof(error->detail), "%s", names[i]);
                goto invalid;
            }
            *slots[i] = functions[i];
            ++count;
        }
        lua_pop(L, 1);
    }
    if (count != 0)
        config->userdata = anchor(L, server, input, 0, RL_COUNT(functions));
    lua_settop(L, top);
    return 1;
invalid:
    if (error->detail[0]) {
        char name[sizeof(error->detail)];
        snprintf(name, sizeof(name), "%s", error->detail);
        snprintf(error->detail, sizeof(error->detail), "hooks.%.*s",
                 (int)sizeof(error->detail) - 7, name);
    } else
        snprintf(error->detail, sizeof(error->detail), "hooks");
    lua_settop(L, top);
    return 0;
}

int rl_proxy_hooks_prepare(lua_State *L, struct rl_server *server,
                           knurl_proxy_config *proxy,
                           knurl_async_route_hooks *config, knurl_error *error)
{
    struct hooks *hooks;
    size_t i, count = 0;
    int top = lua_gettop(L);
    memset(config, 0, sizeof(*config));
    for (i = PROXY_PREFLIGHT; i < RL_COUNT(names); ++i) {
        rl_field(L, 2, names[i]);
        if (!lua_isnil(L, -1)) {
            if (!lua_isfunction(L, -1)) {
                rl_fail(error, KNURL_INVALID,
                        "proxy callback must be a function");
                snprintf(error->detail, sizeof(error->detail), "%s", names[i]);
                lua_settop(L, top);
                return 0;
            }
            ++count;
        }
        lua_pop(L, 1);
    }
    if (count == 0)
        return 1;
    hooks = anchor(L, server, 2, PROXY_PREFLIGHT, RL_COUNT(names));
    config->userdata = hooks;
    if (hooks->enabled[PROXY_PREFLIGHT] || hooks->enabled[PROXY_REWRITE])
        config->before = proxy_before;
    if (hooks->enabled[PROXY_RESPONSE] || hooks->enabled[PROXY_ERROR])
        config->after = proxy_after;
    if (hooks->enabled[PROXY_EXCHANGE]) {
        proxy->on_exchange = proxy_exchange;
        proxy->on_exchange_userdata = hooks;
    }
    lua_settop(L, top);
    return 1;
}

void rl_hooks_finish(lua_State *L, const knurl_async_route_hooks *config,
                     knurl_status status)
{
    struct hooks *hooks = config->userdata;
    int top;
    if (hooks == NULL)
        return;
    if (status == KNURL_OK)
        hooks->server->has_lua = 1;
    else {
        top = lua_gettop(L);
        luaL_unref(L, callback_table(L), hooks->ref);
        lua_settop(L, top);
    }
}
