/* Host-owned Lua callbacks over the public TLS material and ACME lifecycle. */
#include "binding.h"

struct tls_callback {
    struct rl_server *server;
    lua_Integer slot;
};
struct material_call {
    struct tls_callback *callback;
    rotmos_source **out;
    rotmos_status status;
    rotmos_error *error;
};
struct acme_call {
    struct tls_callback *callback;
    const rotmos_process_context *process;
    rotmos_acme_event event;
    rotmos_bytes domain, directory;
    rotmos_status status;
    rotmos_error *error;
};
static struct tls_callback *retain_callback(lua_State *L, int owner,
                                            int function)
{
    struct tls_callback *callback;
    int index;
    owner = lua_absindex(L, owner);
    function = lua_absindex(L, function);
    callback = lua_newuserdatauv(L, sizeof(*callback), 2);
    callback->server = rl_server(L, owner);
    index = lua_absindex(L, -1);
    lua_pushvalue(L, function);
    lua_setiuservalue(L, index, 1);
    lua_pushvalue(L, owner);
    lua_setiuservalue(L, index, 2);
    lua_getiuservalue(L, owner, 5);
    callback->slot = (lua_Integer)lua_rawlen(L, -1) + 1;
    lua_pushvalue(L, index);
    lua_rawseti(L, -2, callback->slot);
    lua_pop(L, 1);
    return callback;
}
static int push_callback(lua_State *L, struct tls_callback *callback)
{
    if (callback->server->root == LUA_NOREF)
        return 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, callback->server->root);
    lua_getiuservalue(L, -1, 5);
    lua_rawgeti(L, -1, callback->slot);
    lua_getiuservalue(L, -1, 1);
    return lua_isfunction(L, -1);
}
static int material_invoke(lua_State *L)
{
    struct material_call *call = lua_touserdata(L, 1);
    struct rl_source *source;
    struct rl_server *server = call->callback->server;
    int index;
    if (server->callback_error.status != ROTMOS_OK) {
        *call->error = server->callback_error;
        call->status = call->error->status;
        return 0;
    }
    if (!push_callback(L, call->callback)) {
        call->status = rl_fail(call->error, ROTMOS_STATE,
                               "TLS material callback has no active owner");
        return 0;
    }
    lua_call(L, 0, 2);
    if (!lua_isnil(L, -1)) {
        call->status = rl_callback_error(L, -1, call->error,
                                         "TLS material open returned an error");
        return 0;
    }
    lua_pop(L, 1);
    index = lua_absindex(L, -1);
    if (lua_istable(L, index)) {
        call->status = rl_source_create(L, index, 1, call->error);
        if (call->status != ROTMOS_OK)
            return 0;
        index = lua_absindex(L, -1);
    }
    source = luaL_testudata(L, index, RL_SOURCE);
    if (source == NULL || !source->owned || source->native == NULL ||
        source->active) {
        call->status =
            rl_fail(call->error, ROTMOS_STATE,
                    "TLS material opener must return an unowned, idle source");
        return 0;
    }
    lua_pushvalue(L, index);
    source->root = luaL_ref(L, LUA_REGISTRYINDEX);
    source->L = L;
    source->material_server = server;
    source->owned = 0;
    *call->out = source->native;
    return 0;
}
static rotmos_status material_open(rotmos_source **out, void *data,
                                   rotmos_error *error)
{
    struct tls_callback *callback = data;
    struct material_call call;
    rotmos_status status;
    memset(&call, 0, sizeof(call));
    call.callback = callback;
    call.out = out;
    call.error = error;
    *out = NULL;
    status = rl_protect(callback->server->L, material_invoke, &call, error);
    return status == ROTMOS_OK ? call.status : status;
}
int rl_material_config(lua_State *L, int owner, int index,
                       rotmos_tls_material *material, rotmos_error *error)
{
    int top = lua_gettop(L);
    rl_field(L, index, "open");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        rl_fail(error, ROTMOS_INVALID,
                "TLS callback material requires an open function");
        return 0;
    }
    material->kind = ROTMOS_MATERIAL_CALLBACK;
    material->open = material_open;
    material->userdata = retain_callback(L, owner, -1);
    lua_settop(L, top);
    return 1;
}
static int acme_invoke(lua_State *L)
{
    static const char *const names[] = {"restore", "persist", "error"};
    struct acme_call *call = lua_touserdata(L, 1);
    if (!push_callback(L, call->callback)) {
        call->status = rl_fail(call->error, ROTMOS_STATE,
                               "ACME callback has no active owner");
        return 0;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, call->callback->server->root);
    lua_createtable(L, 0, 5);
    lua_pushstring(L, names[call->event]);
    lua_setfield(L, -2, "event");
    rl_process_push(L, call->process);
    lua_setfield(L, -2, "process");
    lua_pushlstring(L, call->domain.data, call->domain.length);
    lua_setfield(L, -2, "domain");
    lua_pushlstring(L, call->directory.data, call->directory.length);
    lua_setfield(L, -2, "runtime_directory");
    if (call->event == ROTMOS_ACME_ERROR) {
        rl_push_error(L, call->error);
        lua_setfield(L, -2, "error");
    }
    lua_call(L, 2, 2);
    if (!lua_isnil(L, -1))
        call->status = rl_callback_error(
            L, -1, call->error, "ACME state callback returned an error");
    else if (lua_isboolean(L, -2) && !lua_toboolean(L, -2))
        call->status = rl_fail(call->error, ROTMOS_STATE,
                               "ACME state callback rejected operation");
    return 0;
}
static rotmos_status acme_state(rotmos_server *server,
                                const rotmos_process_context *process,
                                rotmos_acme_event event, rotmos_bytes domain,
                                rotmos_bytes directory, void *data,
                                rotmos_error *error)
{
    struct tls_callback *callback = data;
    struct acme_call call;
    rotmos_status status;
    (void)server;
    memset(&call, 0, sizeof(call));
    call.callback = callback;
    call.process = process;
    call.event = event;
    call.domain = domain;
    call.directory = directory;
    call.error = error;
    status = rl_protect(callback->server->L, acme_invoke, &call, error);
    return status == ROTMOS_OK ? call.status : status;
}
int rl_acme_config(lua_State *L, int owner, int index,
                   rotmos_acme_config *config, rotmos_error *error)
{
    int top = lua_gettop(L);
    rl_field(L, index, "state");
    if (!lua_isnil(L, -1)) {
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            rl_fail(error, ROTMOS_INVALID,
                    "ACME state callback must be a function");
            return 0;
        }
        config->state = acme_state;
        config->userdata = retain_callback(L, owner, -1);
    }
    lua_settop(L, top);
    return 1;
}
