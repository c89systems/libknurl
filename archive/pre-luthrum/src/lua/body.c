#include "binding.h"

void rl_body_result_retire(struct rl_body_result *body)
{
    --body->readers;
    if (body->closed && body->readers == 0)
        knurl_body_result_cleanup(&body->value);
}
static int result_gc(lua_State *L)
{
    struct rl_body_result *body = luaL_testudata(L, 1, RL_BODY_RESULT);
    if (body != NULL) {
        body->closed = 1;
        if (body->readers == 0)
            knurl_body_result_cleanup(&body->value);
    }
    return 0;
}
static int result_close(lua_State *L)
{
    struct rl_body_result *body = luaL_testudata(L, 1, RL_BODY_RESULT);
    knurl_error error;
    if (body == NULL || body->readers != 0)
        return rl_error(L, (rl_fail(&error, KNURL_STATE,
                                    "body result still has open readers"),
                            &error));
    result_gc(L);
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 1);
    lua_pushboolean(L, 1);
    return 1;
}
static struct rl_body_result *result_check(lua_State *L, knurl_error *error)
{
    struct rl_body_result *body = luaL_testudata(L, 1, RL_BODY_RESULT);
    if (body == NULL || body->closed) {
        rl_fail(error, KNURL_STATE, "body result is closed");
        return NULL;
    }
    return body;
}
static int result_snapshot(lua_State *L)
{
    knurl_error error;
    struct rl_body_result *body = result_check(L, &error);
    if (body == NULL)
        return rl_error(L, &error);
    lua_createtable(L, 0, 4);
    lua_pushstring(L, body->value.kind == KNURL_BODY_FILE ? "file" : "memory");
    lua_setfield(L, -2, "kind");
    rl_push_u64(L, body->value.length);
    lua_setfield(L, -2, "length");
    if (body->value.kind == KNURL_BODY_FILE) {
        lua_pushstring(L, body->value.path);
        lua_setfield(L, -2, "path");
    } else {
        lua_pushlstring(
            L, body->value.memory.data == NULL ? "" : body->value.memory.data,
            body->value.memory.length);
        lua_setfield(L, -2, "memory");
    }
    return 1;
}
static int result_reader(lua_State *L)
{
    knurl_error error;
    struct rl_body_result *body = result_check(L, &error);
    if (body == NULL ||
        !rl_source_wrap_reader(L, 1, NULL, body, &body->value, &error))
        return rl_error(L, &error);
    return 1;
}
int rl_request_body_reader(lua_State *L)
{
    struct rl_request *r;
    knurl_body *body;
    knurl_body_result view;
    knurl_error error;
    const char *path;
    int fd;
    if (!rl_request_check(L, 1, &r, &error))
        return rl_error(L, &error);
    body = knurl_request_body(r->native);
    knurl_body_result_init(&view);
    view.kind = knurl_body_kind(body);
    if (knurl_body_length(body, &view.length, &error) != KNURL_OK)
        return rl_error(L, &error);
    if (view.kind == KNURL_BODY_FILE) {
        if (knurl_body_file(body, &fd, &path, &error) != KNURL_OK)
            return rl_error(L, &error);
        view.path =
            (char *)path; /* Borrowed view; never cleaned or unlinked. */
    } else if (knurl_body_bytes(body, &view.memory, &error) != KNURL_OK)
        return rl_error(L, &error);
    if (!rl_source_wrap_reader(L, 1, r, NULL, &view, &error))
        return rl_error(L, &error);
    return 1;
}
static int materialize(lua_State *L, int operation)
{
    struct rl_request *r;
    struct rl_body_result *body;
    knurl_body_materialize_config config;
    knurl_server_effective effective;
    knurl_error error;
    knurl_status status;
    size_t capacity = 0;
    int result;
    static const char *const fields[] = {"capacity", "memory_limit_bytes",
                                         "directory", "prefix"};
    if (!rl_request_check(L, 1, &r, &error))
        return rl_error(L, &error);
    knurl_body_materialize_config_init(&config);
    if (knurl_server_effective_config(knurl_request_server(r->native),
                                      &effective, &error) != KNURL_OK)
        return rl_error(L, &error);
    config.directory = effective.config.limits.body_spool_directory;
    if (operation == 1) {
        if (!lua_isnoneornil(L, 2) && !rl_size(L, 2, &capacity))
            goto invalid;
    } else if (!lua_isnoneornil(L, 2)) {
        if (!lua_istable(L, 2))
            goto invalid;
        if (!rl_fields(L, 2, fields, sizeof(fields) / sizeof(fields[0]),
                       &error))
            return rl_error(L, &error);
        if (!rl_field_size(L, 2, "capacity", &capacity) ||
            !rl_field_size(L, 2, "memory_limit_bytes",
                           &config.memory_limit_bytes) ||
            !rl_field_string(L, 2, "directory", &config.directory) ||
            !rl_field_string(L, 2, "prefix", &config.prefix))
            goto invalid;
    }
    body = lua_newuserdatauv(L, sizeof(*body), 1);
    memset(body, 0, sizeof(*body));
    knurl_body_result_init(&body->value);
    result = lua_absindex(L, -1);
    luaL_setmetatable(L, RL_BODY_RESULT);
    if (capacity != 0) {
        config.buffer = lua_newuserdatauv(L, capacity, 0);
        config.capacity = capacity;
        lua_setiuservalue(L, result, 1);
    }
    status =
        operation == 1
            ? knurl_request_body_copy(r->native, config.buffer, config.capacity,
                                      &body->value, &error)
        : operation == 2
            ? knurl_request_body_spill(r->native, &config, &body->value, &error)
            : knurl_request_body_materialize(r->native, &config, &body->value,
                                             &error);
    if (status != KNURL_OK)
        return rl_error(L, &error);
    return 1;
invalid:
    return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                "invalid body materialization options"),
                        &error));
}
int rl_request_body_materialize(lua_State *L) { return materialize(L, 0); }
int rl_request_body_copy(lua_State *L) { return materialize(L, 1); }
int rl_request_body_spill(lua_State *L) { return materialize(L, 2); }
void rl_bodies_open(lua_State *L)
{
    static const luaL_Reg methods[] = {{"snapshot", result_snapshot},
                                       {"reader", result_reader},
                                       {"close", result_close},
                                       {NULL, NULL}};
    luaL_newmetatable(L, RL_BODY_RESULT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pushcfunction(L, result_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}
