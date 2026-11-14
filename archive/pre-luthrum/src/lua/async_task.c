#include "binding.h"

static char settings_key, used_key;
struct async_task {
    struct rl_async *async;
    knurl_task *task;
    knurl_lua_async_operation op;
    void *state;
    int root;
    knurl_status status;
    knurl_error error;
};
int rl_async_configure(lua_State *L)
{
    static const char *const fields[] = {"threads", "max_tasks"};
    knurl_task_pool_config c;
    knurl_error error;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &used_key);
    if (lua_toboolean(L, -1))
        return luaL_error(L, "async configuration must precede first use");
    lua_pop(L, 1);
    knurl_task_pool_config_init(&c);
    if (!lua_istable(L, 1) || !rl_fields(L, 1, fields, 2, &error) ||
        !rl_field_size(L, 1, "threads", &c.threads) ||
        !rl_field_size(L, 1, "max_tasks", &c.max_tasks) || c.threads > 8 ||
        c.max_tasks > 256)
        return luaL_error(
            L, "async configuration requires threads=0..8, max_tasks=0..256");
    lua_pushlstring(L, (const char *)&c, sizeof(c));
    lua_rawsetp(L, LUA_REGISTRYINDEX, &settings_key);
    lua_pushboolean(L, 1);
    return 1;
}
static void pool_retired(void *data)
{
    struct rl_server *s = data;
    s->async_pool = NULL;
}
static knurl_status pool(lua_State *L, struct rl_async *a, knurl_error *error)
{
    knurl_task_pool_config c;
    knurl_status status;
    if (a->call.server->async_pool != NULL)
        return KNURL_OK;
    knurl_task_pool_config_init(&c);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &settings_key);
    if (lua_type(L, -1) == LUA_TSTRING && lua_rawlen(L, -1) == sizeof(c))
        memcpy(&c, lua_tostring(L, -1), sizeof(c));
    lua_pop(L, 1);
    /* Set before creating native resources: Lua allocation failure cannot
     * strand a successfully created pool. */
    lua_pushboolean(L, 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &used_key);
    c.retired = pool_retired;
    c.userdata = a->call.server;
    status = knurl_task_pool_new(a->call.server->native, &c,
                                 &a->call.server->async_pool, error);
    return status;
}
static void cancel_task(void *data)
{
    struct async_task *c = data;
    if (c->task != NULL)
        (void)knurl_task_cancel(c->task, NULL);
}
static knurl_status execute(knurl_task *task, void *data, knurl_error *error)
{
    struct async_task *c = data;
    return c->op.work(task, c->state, error);
}
static void retired(knurl_task *task, knurl_status status,
                    const knurl_error *error, void *data)
{
    struct async_task *c = data;
    struct rl_async *a = c->async;
    (void)task;
    c->task = NULL;
    c->status = status;
    c->error = *error;
    if (a->call.native != NULL && rl_async_schedule(a, &a->error) != KNURL_OK)
        (void)knurl_response_abort(knurl_request_response(a->call.native),
                                   a->error.status, NULL);
    luaL_unref(a->call.server->L, LUA_REGISTRYINDEX, c->root);
    c->root = LUA_NOREF;
    rl_async_release(a);
}
static int continuation(lua_State *L, int status, lua_KContext context)
{
    struct async_task *c = (struct async_task *)context;
    (void)status;
    c->async->waiting = NULL;
    c->async->cancel = NULL;
    return c->op.complete(L, c->state, c->status, &c->error);
}
int knurl_lua_async_run(lua_State *L, const knurl_lua_async_operation *op,
                        void *state, int owner_index)
{
    struct rl_async *a = rl_async_current(L);
    struct async_task *c;
    knurl_task_config config;
    knurl_error error;
    knurl_status status;
    int owner = lua_absindex(L, owner_index);
    if (a == NULL || !lua_isyieldable(L) || a->waiting != NULL)
        return luaL_error(L,
                          "async operation requires a yieldable Knurl route");
    if (op == NULL || op->work == NULL || op->complete == NULL ||
        (lua_type(L, owner) != LUA_TUSERDATA && !lua_istable(L, owner)))
        return luaL_error(L,
                          "async operation requires callbacks and a Lua owner");
    status = pool(L, a, &error);
    if (status != KNURL_OK)
        return luaL_error(L, "%s", error.message);
    c = lua_newuserdatauv(L, sizeof(*c), 1);
    memset(c, 0, sizeof(*c));
    c->async = a;
    c->op = *op;
    c->state = state;
    lua_pushvalue(L, owner);
    lua_setiuservalue(L, -2, 1);
    lua_pushvalue(L, -1);
    c->root = luaL_ref(L, LUA_REGISTRYINDEX);
    knurl_task_config_init(&config);
    config.work = execute;
    config.retired = retired;
    config.userdata = c;
    config.timeout_ms = op->timeout_ms;
    config.timeout_disabled = op->timeout_disabled;
    status = knurl_task_pool_submit(a->call.server->async_pool, &config,
                                    &c->task, &error);
    if (status != KNURL_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, c->root);
        c->root = LUA_NOREF;
        return luaL_error(L, "%s",
                          status == KNURL_AGAIN
                              ? "async task limit reached or queue busy"
                              : error.message);
    }
    rl_async_retain(a);
    a->waiting = c;
    a->cancel = cancel_task;
    return lua_yieldk(L, 0, (lua_KContext)c, continuation);
}
