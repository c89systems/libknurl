#include "binding.h"
enum item_kind { DEFER, TIMER, WATCH, WAKER };
struct loop_item {
    struct rl_server *server;
    knurl_loop *loop;
    knurl_timer *timer;
    knurl_watch *watch;
    knurl_waker *waker;
    int root, kind, active, closing;
    long process_id;
    unsigned long run_epoch;
    knurl_error error;
};
struct loop_call {
    struct loop_item *item;
    unsigned int ready;
    int retired;
};
static void roots(lua_State *L, struct rl_server *server)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, server->root);
    lua_getiuservalue(L, -1, 3);
}
void rl_loops_process_start(lua_State *L, struct rl_server *server)
{
    struct loop_item *item;
    int top = lua_gettop(L), table, inherited = 0;
    roots(L, server);
    table = lua_absindex(L, -1);
    lua_pushnil(L);
    while (lua_next(L, table)) {
        item = luaL_testudata(L, -1, RL_LOOP_ITEM);
        if (item != NULL && item->process_id != server->process_id)
            inherited = 1;
        lua_pop(L, 1);
    }
    if (inherited) {
        lua_newtable(L); /* Allocate before invalidating inherited owners. */
        lua_pushnil(L);
        while (lua_next(L, table)) {
            item = luaL_testudata(L, -1, RL_LOOP_ITEM);
            if (item != NULL) {
                item->active = 0;
                item->closing = 1;
                item->root = LUA_NOREF;
                item->timer = NULL;
                item->watch = NULL;
                item->waker = NULL;
                item->loop = NULL;
            }
            lua_pop(L, 1);
        }
        lua_setiuservalue(L, table - 1, 3);
    }
    lua_settop(L, top);
}
static void cancel(struct loop_item *item)
{
    if (!item->active || item->closing)
        return;
    item->closing = 1;
    if (item->timer != NULL)
        knurl_timer_cancel(item->timer);
    if (item->watch != NULL)
        knurl_watch_cancel(item->watch);
    if (item->waker != NULL)
        knurl_waker_destroy(item->waker, NULL);
}
static int trampoline(lua_State *L)
{
    struct loop_call *call = lua_touserdata(L, 1);
    int view;
    roots(L, call->item->server);
    lua_rawgeti(L, -1, call->item->root);
    view = lua_absindex(L, -1);
    lua_getiuservalue(L, view, 2);
    lua_rawgeti(L, -1, call->retired ? 2 : 1);
    if (lua_isnil(L, -1))
        return 0;
    lua_pushvalue(L, view);
    if (call->retired) {
        if (call->item->error.status == KNURL_OK)
            lua_pushnil(L);
        else
            rl_push_error(L, &call->item->error);
    } else if (call->item->kind == WATCH)
        lua_pushinteger(L, call->ready);
    lua_call(L, call->retired || call->item->kind == WATCH ? 2 : 1, 0);
    return 0;
}
static void deliver(struct loop_item *item, unsigned int ready, int retired)
{
    struct loop_call call;
    knurl_error error;
    knurl_status status;
    if (!retired && item->closing)
        return;
    call.item = item;
    call.ready = ready;
    call.retired = retired;
    status = rl_protect(item->server->L, trampoline, &call, &error);
    if (status != KNURL_OK) {
        if (item->error.status == KNURL_OK)
            item->error = error;
        if (!retired)
            cancel(item);
    }
}
static void retired(void *data)
{
    struct loop_item *item = data;
    item->active = 0;
    item->timer = NULL;
    item->watch = NULL;
    item->waker = NULL;
    item->loop = NULL;
    deliver(item, 0, 1);
    roots(item->server->L, item->server);
    luaL_unref(item->server->L, -1, item->root);
    lua_pop(item->server->L, 2);
    item->root = LUA_NOREF;
}
static void timer_callback(knurl_timer *timer, void *data)
{
    (void)timer;
    deliver(data, 0, 0);
}
static void watch_callback(knurl_watch *watch, unsigned int ready, void *data)
{
    (void)watch;
    deliver(data, ready, 0);
}
static void loop_callback(knurl_loop *loop, void *data)
{
    (void)loop;
    deliver(data, 0, 0);
}
static int checked(lua_State *L, struct loop_item **out, knurl_error *error)
{
    knurl_u64 now;
    *out = luaL_testudata(L, 1, RL_LOOP_ITEM);
    if (*out == NULL || !(*out)->active || (*out)->closing ||
        (*out)->server->native == NULL ||
        (*out)->process_id != (*out)->server->process_id ||
        (*out)->run_epoch != (*out)->server->run_epoch ||
        (*out)->loop != knurl_server_loop((*out)->server->native)) {
        rl_fail(error, KNURL_STATE, "loop registration is retired or closing");
        return 0;
    }
    return knurl_loop_now((*out)->loop, &now, error) == KNURL_OK;
}
static int close_item(lua_State *L)
{
    struct loop_item *item = luaL_testudata(L, 1, RL_LOOP_ITEM);
    knurl_error error;
    if (item == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_INVALID, "loop registration required"),
                &error));
    if (item->active && !item->closing) {
        if (!checked(L, &item, &error))
            return rl_error(L, &error);
        cancel(item);
    }
    if (item->error.status != KNURL_OK)
        return rl_error(L, &item->error);
    lua_pushboolean(L, 1);
    return 1;
}
static int update(lua_State *L)
{
    struct loop_item *item;
    knurl_error error;
    size_t interest;
    if (!checked(L, &item, &error))
        return rl_error(L, &error);
    if (item->kind != WATCH || !rl_size(L, 2, &interest) || interest > UINT_MAX)
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "watch and valid interest flags required"),
                            &error));
    if (knurl_watch_update(item->watch, (unsigned int)interest, &error) !=
        KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int signal_item(lua_State *L)
{
    struct loop_item *item;
    knurl_error error;
    if (!checked(L, &item, &error))
        return rl_error(L, &error);
    if (item->kind != WAKER)
        return rl_error(
            L, (rl_fail(&error, KNURL_INVALID, "waker required"), &error));
    if (knurl_waker_signal(item->waker, &error) != KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int create(lua_State *L, int kind)
{
    static const char *const fields[] = {"callback",  "retired", "delay_ms",
                                         "repeat_ms", "fd",      "interest"};
    struct rl_server *server = rl_server(L, 1);
    struct loop_item *item;
    knurl_error error;
    knurl_status status;
    knurl_loop *loop;
    size_t delay = 0, repeat = 0, fd = 0, interest = 0;
    int index, i;
    if (server == NULL || server->native == NULL ||
        (loop = knurl_server_loop(server->native)) == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "active server loop required"),
                &error));
    if (!lua_istable(L, 2))
        goto invalid;
    if (!rl_fields(L, 2, fields,
                   kind == DEFER || kind == WAKER ? 2
                   : kind == TIMER                ? 4
                                                  : 6,
                   &error))
        return rl_error(L, &error);
    if (kind == WATCH) {
        rl_field(L, 2, "fd");
        if (!rl_size(L, -1, &fd) || fd > INT_MAX)
            goto invalid;
        lua_pop(L, 1);
        if (!rl_field_size(L, 2, "interest", &interest) || interest > UINT_MAX)
            goto invalid;
        /* Timer fields do not apply to a watch. */
        for (i = 2; i < 4; ++i) {
            rl_field(L, 2, fields[i]);
            if (!lua_isnil(L, -1))
                goto invalid;
            lua_pop(L, 1);
        }
    }
    if (kind == TIMER &&
        (!rl_field_size(L, 2, "delay_ms", &delay) || delay > LONG_MAX ||
         !rl_field_size(L, 2, "repeat_ms", &repeat) || repeat > LONG_MAX))
        goto invalid;
    item = lua_newuserdatauv(L, sizeof(*item), 2);
    memset(item, 0, sizeof(*item));
    item->server = server;
    item->loop = loop;
    item->kind = kind;
    item->process_id = server->process_id;
    item->run_epoch = server->run_epoch;
    item->root = LUA_NOREF;
    index = lua_absindex(L, -1);
    luaL_setmetatable(L, RL_LOOP_ITEM);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, index, 1);
    lua_createtable(L, 2, 0);
    for (i = 0; i < 2; ++i) {
        rl_field(L, 2, fields[i]);
        if (!lua_isfunction(L, -1) && (i == 0 || !lua_isnil(L, -1)))
            goto invalid;
        lua_rawseti(L, -2, i + 1);
    }
    lua_setiuservalue(L, index, 2);
    lua_getiuservalue(L, 1, 3);
    lua_pushvalue(L, index);
    item->root = luaL_ref(L, -2);
    lua_pop(L, 1);
    if (kind == TIMER)
        status =
            knurl_loop_timer(loop, (long)delay, (long)repeat, timer_callback,
                             retired, item, &item->timer, &error);
    else if (kind == WATCH)
        status = knurl_loop_watch(loop, (int)fd, (unsigned int)interest,
                                  watch_callback, retired, item, &item->watch,
                                  &error);
    else if (kind == WAKER)
        status = knurl_waker_new(loop, loop_callback, retired, item,
                                 &item->waker, &error);
    else
        status = knurl_loop_defer(loop, loop_callback, retired, item, &error);
    if (status != KNURL_OK) {
        lua_getiuservalue(L, 1, 3);
        luaL_unref(L, -1, item->root);
        lua_pop(L, 1);
        item->root = LUA_NOREF;
        return rl_error(L, &error);
    }
    item->active = 1;
    return 1;
invalid:
    return rl_error(L,
                    (rl_fail(&error, KNURL_INVALID,
                             "invalid loop declaration; callback function and "
                             "valid delay/descriptor/interest values required"),
                     &error));
}
int rl_defer(lua_State *L) { return create(L, DEFER); }
int rl_timer(lua_State *L) { return create(L, TIMER); }
int rl_watch(lua_State *L) { return create(L, WATCH); }
int rl_waker(lua_State *L) { return create(L, WAKER); }
int rl_now(lua_State *L)
{
    struct rl_server *server = rl_server(L, 1);
    knurl_u64 now;
    knurl_error error;
    if (server == NULL || server->native == NULL ||
        knurl_server_loop(server->native) == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "active server loop required"),
                &error));
    if (knurl_loop_now(knurl_server_loop(server->native), &now, &error) !=
        KNURL_OK)
        return rl_error(L, &error);
    rl_push_u64(L, now);
    return 1;
}
void rl_loops_open(lua_State *L)
{
    static const luaL_Reg methods[] = {{"close", close_item},
                                       {"cancel", close_item},
                                       {"update", update},
                                       {"signal", signal_item},
                                       {NULL, NULL}};
    luaL_newmetatable(L, RL_LOOP_ITEM);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushinteger(L, KNURL_READABLE);
    lua_setfield(L, -2, "READABLE");
    lua_pushinteger(L, KNURL_WRITABLE);
    lua_setfield(L, -2, "WRITABLE");
    lua_pushinteger(L, KNURL_READ_CLOSED);
    lua_setfield(L, -2, "READ_CLOSED");
    lua_pushinteger(L, KNURL_EVENT_ERROR);
    lua_setfield(L, -2, "ERROR");
    lua_setfield(L, -2, "loop");
    lua_pushcfunction(L, rl_sse_encode);
    lua_setfield(L, -2, "sse_encode");
}
