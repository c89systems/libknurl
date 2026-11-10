#include "binding.h"
int rl_sse_encode(lua_State *L)
{
    knurl_sse_event event;
    knurl_bytes *values[5];
    int *present[5];
    static const char *const names[] = {"comment", "id", "event", "data",
                                        "frame"};
    knurl_error error;
    size_t capacity = 8192, written, i;
    void *buffer;
    int snapshot;
    knurl_sse_event_init(&event);
    if ((!lua_isnoneornil(L, 2) && !rl_size(L, 2, &capacity)) || capacity == 0)
        goto invalid;
    if (lua_isstring(L, 1)) {
        event.data.data = lua_tolstring(L, 1, &event.data.length);
        event.has_data = 1;
    } else if (lua_istable(L, 1)) {
        values[0] = &event.comment;
        values[1] = &event.id;
        values[2] = &event.event;
        values[3] = &event.data;
        values[4] = &event.frame;
        present[0] = &event.has_comment;
        present[1] = &event.has_id;
        present[2] = &event.has_event;
        present[3] = &event.has_data;
        present[4] = &event.has_frame;
        lua_newtable(L);
        snapshot = lua_absindex(L, -1);
        /* A preformatted frame takes precedence without evaluating other
         * getters. */
        lua_getfield(L, 1, "frame");
        if (!lua_isnil(L, -1)) {
            if (!lua_isstring(L, -1))
                goto invalid;
            event.frame.data = lua_tolstring(L, -1, &event.frame.length);
            event.has_frame = 1;
            lua_setfield(L, snapshot, "frame");
        } else {
            lua_pop(L, 1);
            for (i = 0; i < 4; ++i) {
                lua_getfield(L, 1, names[i]);
                if (!lua_isnil(L, -1)) {
                    if (!lua_isstring(L, -1))
                        goto invalid;
                    values[i]->data = lua_tolstring(L, -1, &values[i]->length);
                    *present[i] = 1;
                }
                lua_setfield(L, snapshot, names[i]);
            }
            lua_getfield(L, 1, "retry");
            if (!lua_isnil(L, -1)) {
                if ((!lua_isinteger(L, -1) &&
                     luaL_testudata(L, -1, RL_COUNT) == NULL) ||
                    !rl_u64(L, -1, &event.retry_ms))
                    goto invalid;
                event.has_retry = 1;
            }
            lua_pop(L, 1);
        }
    } else
        goto invalid;
    buffer = lua_newuserdatauv(L, capacity, 0);
    if (knurl_sse_encode(&event, buffer, capacity, &written, &error) !=
        KNURL_OK)
        return rl_error(L, &error);
    lua_pushlstring(L, buffer, written);
    return 1;
invalid:
    return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                "SSE requires a string/event table, "
                                "nonnegative retry and positive capacity"),
                        &error));
}
