/* An independent downstream-style module: installed APIs only. */
#include <knurl/lua.h>
#include <lua.h>
#include <lauxlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define OWNER "test.async.owner"
struct state {
    pthread_t owner;
    knurl_task *task;
    int mode, callbacks, native, acknowledged;
};
static knurl_status fail(knurl_error *e, knurl_status status, const char *text)
{
    knurl_error_clear(e);
    e->status = status;
    snprintf(e->message, sizeof(e->message), "%s", text);
    return status;
}
static knurl_status on_owner(void *data, knurl_error *error)
{
    struct state *s = data;
    if (!pthread_equal(s->owner, pthread_self()))
        return fail(error, KNURL_STATE, "callback ran on native thread");
    if (knurl_task_call(s->task, on_owner, s, error) != KNURL_STATE)
        return fail(error, KNURL_STATE, "task call accepted owner thread");
    ++s->callbacks;
    if (s->mode == 3)
        return fail(error, KNURL_IO, "owner-callback-marker");
    if (s->mode == 4)
        return s->task->cancel(s->task, error);
    knurl_error_clear(error);
    return KNURL_OK;
}
static knurl_status work(knurl_task *task, void *data, knurl_error *error)
{
    struct state *s = data;
    knurl_status status;
#ifdef KNURL_TEST_SMALL_STACK
    pthread_attr_t attr;
    size_t stack;
    if (pthread_getattr_np(pthread_self(), &attr) != 0)
        return fail(error, KNURL_STATE, "cannot inspect native stack");
    if (pthread_attr_getstacksize(&attr, &stack) != 0) {
        pthread_attr_destroy(&attr);
        return fail(error, KNURL_STATE, "cannot inspect native stack size");
    }
    pthread_attr_destroy(&attr);
    if (stack > 140UL * 1024UL)
        return fail(error, KNURL_STATE,
                    "native task exceeded musl stack budget");
#endif
    if (pthread_equal(s->owner, pthread_self()))
        return fail(error, KNURL_STATE, "work ran on Lua owner thread");
    s->native = 1;
    s->task = task;
    if (s->mode == 2) {
        while (!task->cancelled(task))
            usleep(1000);
        if (task->call(task, on_owner, s, error) != KNURL_CANCELLED)
            return fail(error, KNURL_STATE,
                        "cancelled task delivered callback");
        return KNURL_CANCELLED;
    }
    usleep(20000);
    if (s->mode == 1)
        return fail(error, KNURL_IO, "native-error-marker");
    status = task->call(task, on_owner, s, error);
    if (s->mode == 4 && status != KNURL_CANCELLED)
        return fail(error, KNURL_STATE, "reentrant cancellation was lost");
    return status;
}
static int complete(lua_State *L, void *data, knurl_status status,
                    const knurl_error *error)
{
    struct state *s = data;
    if (!pthread_equal(s->owner, pthread_self()))
        return luaL_error(L, "completion ran on native thread");
    if (!s->native)
        return luaL_error(L, "work never ran");
    s->acknowledged = 1;
    lua_pushinteger(L, status);
    lua_pushinteger(L, s->callbacks);
    lua_pushstring(L, error->message);
    return 3;
}
static int run(lua_State *L)
{
    knurl_lua_async_operation op;
    int mode = (int)luaL_checkinteger(L, 1);
    struct state *s = lua_newuserdatauv(L, sizeof(*s), 0);
    memset(s, 0, sizeof(*s));
    s->owner = pthread_self();
    s->mode = mode;
    memset(&op, 0, sizeof(op));
    op.work = work;
    op.complete = complete;
    op.timeout_ms = mode == 2 ? 20 : 1000;
    return knurl_lua_async_run(L, &op, s, -1);
}
KNURL_API int luaopen_knurl_async_fixture(lua_State *);
int luaopen_knurl_async_fixture(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, run);
    lua_setfield(L, -2, "run");
    return 1;
}
