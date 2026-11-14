#include "binding.h"

static char current;
struct rl_async *rl_async_current(lua_State *L)
{
    struct rl_async *a;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &current);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return NULL;
    }
    lua_pushthread(L);
    lua_rawget(L, -2);
    a = lua_touserdata(L, -1);
    lua_pop(L, 2);
    return a;
}
void rl_async_retain(struct rl_async *a) { ++a->refs; }
void rl_async_release(void *data)
{
    struct rl_async *a = data;
    if (--a->refs != 0)
        return;
    if (a->call.reply_view != NULL)
        a->call.reply_view->native = NULL;
    luaL_unref(a->call.server->L, LUA_REGISTRYINDEX, a->root);
    free(a);
}
static int prepare(lua_State *L)
{
    struct rl_async *a = lua_touserdata(L, 1);
    int thread;
    a->co = lua_newthread(L);
    thread = lua_absindex(L, -1);
    lua_pushvalue(L, thread);
    a->root = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &current);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &current);
    }
    lua_pushvalue(L, thread);
    lua_pushlightuserdata(L, a);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    rl_request_push(L, a->call.request);
    lua_getiuservalue(L, -1, 8);
    lua_pushvalue(L, thread);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    lua_pop(L, 2);
    return 0;
}
static knurl_status advance(struct rl_async *a, int args)
{
    int results, status = lua_resume(a->co, a->call.server->L, args, &results);
    if (status == LUA_YIELD) {
        if (a->waiting != NULL || a->queued)
            return KNURL_AGAIN;
        return rl_fail(&a->error, KNURL_STATE,
                       "route yielded without a pending Knurl operation");
    }
    if (a->waiting != NULL && a->cancel != NULL) {
        a->cancel(a->waiting);
        a->waiting = NULL;
        a->cancel = NULL;
    }
    if (status != LUA_OK)
        return rl_fail(
            &a->error, status == LUA_ERRMEM ? KNURL_NOMEM : KNURL_STATE,
            lua_type(a->co, -1) == LUA_TSTRING ? lua_tostring(a->co, -1)
                                               : "Lua route failed");
    return a->call.status;
}
static void forget(struct rl_async *a)
{
    lua_State *L = a->call.server->L;
    if (a->call.request != NULL && a->call.request->async == a)
        a->call.request->async = NULL;
    if (a->root == LUA_NOREF)
        return;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &current);
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, a->root);
        lua_pushnil(L);
        lua_rawset(L, -3);
    }
    lua_pop(L, 1);
}
static void resume_call(knurl_loop *loop, void *data)
{
    struct rl_async *a = data;
    knurl_request *r;
    knurl_status status;
    (void)loop;
    a->queued = 0;
    if (a->call.native == NULL)
        return;
    status = advance(a, 0);
    if (status == KNURL_AGAIN)
        return;
    r = a->call.native;
    forget(a);
    (void)knurl_request_resume(r, status, a->call.finish, &a->error);
    rl_async_release(a);
}
knurl_status rl_async_schedule(struct rl_async *a, knurl_error *error)
{
    knurl_status status;
    if (a->queued || a->call.native == NULL)
        return KNURL_OK;
    rl_async_retain(a);
    a->queued = 1;
    status = knurl_loop_defer(knurl_request_loop(a->call.native), resume_call,
                              rl_async_release, a, error);
    if (status != KNURL_OK) {
        a->queued = 0;
        rl_async_release(a);
    }
    return status;
}
void rl_async_cancel(struct rl_request *r)
{
    struct rl_async *a = r->async;
    if (a == NULL)
        return;
    forget(a);
    a->call.native = NULL;
    if (a->cancel != NULL)
        a->cancel(a->waiting);
    a->waiting = NULL;
    a->cancel = NULL;
    rl_async_release(a);
}
knurl_status rl_async_call(struct rl_call *call, lua_CFunction fn,
                           knurl_error *error)
{
    struct rl_async *a = calloc(1, sizeof(*a));
    knurl_status status;
    if (a == NULL)
        return rl_fail(error, KNURL_NOMEM, "allocating route coroutine");
    a->call = *call;
    a->call.error = &a->error;
    a->refs = 1;
    a->root = LUA_NOREF;
    status = rl_protect(call->server->L, prepare, a, error);
    if (status != KNURL_OK) {
        forget(a);
        rl_async_release(a);
        return status;
    }
    lua_pushcfunction(a->co, fn);
    lua_pushlightuserdata(a->co, &a->call);
    call->request->async = a;
    status = advance(a, 1);
    if (status == KNURL_AGAIN)
        return status;
    call->finish = a->call.finish;
    call->reply_view = NULL;
    if (error != NULL)
        *error = a->error;
    forget(a);
    rl_async_release(a);
    return status;
}

struct sleep_call {
    struct rl_async *async;
    knurl_timer *timer;
};
static int sleep_continue(lua_State *L, int status, lua_KContext context)
{
    (void)status;
    (void)context;
    lua_pushboolean(L, 1);
    return 1;
}
static void sleep_retired(void *data)
{
    struct sleep_call *s = data;
    rl_async_release(s->async);
    free(s);
}
static void sleep_cancel(void *data)
{
    struct sleep_call *s = data;
    knurl_timer_cancel(s->timer);
}
static void sleep_ready(knurl_timer *timer, void *data)
{
    struct sleep_call *s = data;
    struct rl_async *a = s->async;
    (void)timer;
    a->waiting = NULL;
    a->cancel = NULL;
    if (rl_async_schedule(a, &a->error) != KNURL_OK && a->call.native != NULL)
        (void)knurl_response_abort(knurl_request_response(a->call.native),
                                   a->error.status, NULL);
}
int rl_sleep(lua_State *L)
{
    struct rl_async *a = rl_async_current(L);
    struct sleep_call *s;
    size_t delay;
    knurl_error error;
    if (a == NULL || !lua_isyieldable(L))
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE,
                        "sleep requires a yieldable request handler or hook"),
                &error));
    if (!rl_size(L, 1, &delay) || delay > 0x7fffffffUL)
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "sleep milliseconds must be 0..2147483647"),
                            &error));
    if (delay == 0) {
        if (rl_async_schedule(a, &error) != KNURL_OK)
            return rl_error(L, &error);
    } else {
        s = calloc(1, sizeof(*s));
        if (s == NULL)
            return rl_error(
                L, (rl_fail(&error, KNURL_NOMEM, "allocating sleep timer"),
                    &error));
        s->async = a;
        if (knurl_loop_timer(knurl_request_loop(a->call.native), (long)delay, 0,
                             sleep_ready, sleep_retired, s, &s->timer,
                             &error) != KNURL_OK) {
            free(s);
            return rl_error(L, &error);
        }
        rl_async_retain(a);
        a->waiting = s;
        a->cancel = sleep_cancel;
    }
    return lua_yieldk(L, 0, 0, sleep_continue);
}
