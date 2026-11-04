#include "binding.h"
struct source_call {
    struct rl_source *source;
    void *buffer;
    size_t capacity, *produced;
    const char *name;
    rotmos_status outcome, status;
    rotmos_error *error;
};
static int source_trampoline(lua_State *L)
{
    struct source_call *c = lua_touserdata(L, 1);
    struct rl_source *s = c->source;
    int view;
    const char *bytes;
    size_t length, n;
    if (s->root == LUA_NOREF)
        lua_pushvalue(L, 2);
    else
        lua_rawgeti(L, LUA_REGISTRYINDEX, s->root);
    view = lua_absindex(L, -1);
    if (!strcmp(c->name, "read")) {
        lua_getiuservalue(L, view, 2);
        if (lua_type(L, -1) == LUA_TSTRING)
            goto chunk;
        lua_pop(L, 1);
    }
    lua_getiuservalue(L, view, 1);
    rl_field(L, -1, c->name);
    if (lua_isnil(L, -1)) {
        c->status = !strcmp(c->name, "reset")
                        ? rl_fail(c->error, ROTMOS_NOT_SUPPORTED,
                                  "source is not resettable")
                        : ROTMOS_OK;
        return 0;
    }
    if (!lua_isfunction(L, -1)) {
        c->status = rl_fail(c->error, ROTMOS_INVALID,
                            "source callback must be a function");
        return 0;
    }
    if (!strcmp(c->name, "read")) {
        rl_push_size(L, c->capacity);
        lua_call(L, 1, 2);
        if (lua_isnil(L, -2) && rl_is_sentinel(L, -1, ROTMOS_AGAIN)) {
            c->status = ROTMOS_AGAIN;
            return 0;
        }
        if (lua_isnil(L, -2) && rl_is_sentinel(L, -1, ROTMOS_EOF)) {
            c->status = ROTMOS_EOF;
            return 0;
        }
        if (s->legacy && lua_isnil(L, -1) &&
            (lua_isnil(L, -2) ||
             (lua_isboolean(L, -2) && !lua_toboolean(L, -2)) ||
             (lua_type(L, -2) == LUA_TSTRING && lua_rawlen(L, -2) == 0))) {
            c->status = ROTMOS_EOF;
            return 0;
        }
        if (!lua_isnil(L, -1)) {
            c->status = rl_callback_error(L, -1, c->error,
                                          "source read returned an error");
            return 0;
        }
        if (s->legacy && lua_type(L, -2) == LUA_TNUMBER)
            (void)lua_tolstring(L, -2, NULL);
        if (lua_type(L, -2) != LUA_TSTRING) {
            c->status =
                rl_fail(c->error, ROTMOS_INVALID,
                        "producer requires bytes or explicit EOF/AGAIN");
            return 0;
        }
        lua_pop(L, 1);
        s->offset = 0;
        lua_pushvalue(L, -1);
        lua_setiuservalue(L, view, 2);
    chunk:
        bytes = lua_tolstring(L, -1, &length);
        if (length == 0) {
            c->status = rl_fail(c->error, ROTMOS_INVALID,
                                "producer returned an empty success");
            return 0;
        }
        n = length - s->offset;
        if (n > c->capacity)
            n = c->capacity;
        memcpy(c->buffer, bytes + s->offset, n);
        s->offset += n;
        *c->produced = n;
        if (s->offset == length) {
            lua_pushnil(L);
            lua_setiuservalue(L, view, 2);
            s->offset = 0;
        }
        return 0;
    }
    if (!strcmp(c->name, "close")) {
        if (!s->legacy)
            lua_pushinteger(L, c->outcome);
        lua_call(L, s->legacy ? 0 : 1, 0);
    } else {
        lua_call(L, 0, 2);
        if ((lua_isboolean(L, -2) && !lua_toboolean(L, -2)) ||
            !lua_isnil(L, -1))
            c->status =
                rl_callback_error(L, -1, c->error, "source reset failed");
        else {
            lua_pushnil(L);
            lua_setiuservalue(L, view, 2);
            s->offset = 0;
            s->initial_reset = 0;
        }
    }
    return 0;
}
static rotmos_status call_source(struct source_call *c)
{
    rotmos_error local;
    rotmos_status status;
    if (c->error == NULL)
        c->error = &local;
    status = rl_protect_value(c->source->L, source_trampoline, c,
                              c->source->root == LUA_NOREF ? 1 : 0, c->error);
    if (status == ROTMOS_OK)
        status = c->status;
    if (status != ROTMOS_OK && status != ROTMOS_EOF && status != ROTMOS_AGAIN &&
        c->source->error.status == ROTMOS_OK)
        c->source->error = *c->error;
    return status;
}
static rotmos_status read_source(void *buffer, size_t capacity,
                                 size_t *produced, void *data,
                                 rotmos_error *error)
{
    struct source_call c;
    memset(&c, 0, sizeof(c));
    c.source = data;
    c.buffer = buffer;
    c.capacity = capacity;
    c.produced = produced;
    c.name = "read";
    c.error = error;
    if (c.source->initial_reset) {
        rotmos_status status;
        c.name = "reset";
        status = call_source(&c);
        if (status != ROTMOS_OK)
            return status;
        c.name = "read";
    }
    return call_source(&c);
}
static rotmos_status reset_source(void *data, rotmos_error *error)
{
    struct source_call c;
    memset(&c, 0, sizeof(c));
    c.source = data;
    c.name = "reset";
    c.error = error;
    return call_source(&c);
}
static void close_source(void *data, rotmos_status cause)
{
    struct rl_source *s = data;
    struct source_call c;
    rotmos_error error;
    rotmos_status status;
    s->native = NULL;
    if (s->request_body != NULL || s->result_body != NULL) {
        if (s->delegate != NULL)
            rotmos_source_destroy(s->delegate, NULL);
        s->delegate = NULL;
        if (s->result_body != NULL)
            rl_body_result_retire(s->result_body);
        s->result_body = NULL;
        s->request_body = NULL;
        goto unroot;
    }
    memset(&c, 0, sizeof(c));
    c.source = s;
    c.name = "close";
    c.outcome = cause;
    c.error = &error;
    status = call_source(&c);
    if (status != ROTMOS_OK) {
        if (s->error.status == ROTMOS_OK)
            s->error = error;
    }
unroot:
    if (s->root != LUA_NOREF)
        luaL_unref(s->L, LUA_REGISTRYINDEX, s->root);
    s->root = LUA_NOREF;
}
rotmos_status rl_source_create(lua_State *L, int index, int legacy,
                               rotmos_error *error)
{
    struct rl_source *s;
    rotmos_source_config config;
    rotmos_status status;
    static const char *names[] = {"read", "reset", "close"};
    int callbacks, i;
    index = lua_absindex(L, index);
    if (!lua_istable(L, index))
        return rl_fail(error, ROTMOS_INVALID,
                       "source callbacks table required");
    s = lua_newuserdatauv(L, sizeof(*s), 2);
    memset(s, 0, sizeof(*s));
    s->root = LUA_NOREF;
    s->L = L;
    s->owned = 1;
    s->legacy = legacy;
    luaL_setmetatable(L, RL_SOURCE);
    lua_newtable(L);
    callbacks = lua_absindex(L, -1);
    for (i = 0; i < 3; ++i) {
        lua_getfield(L, index, names[i]);
        if (!lua_isfunction(L, -1) && (i == 0 || !lua_isnil(L, -1)))
            return rl_fail(error, ROTMOS_INVALID,
                           "source callbacks must be functions");
        if (i == 1)
            s->resettable = lua_isfunction(L, -1);
        lua_setfield(L, callbacks, names[i]);
    }
    lua_setiuservalue(L, -2, 1);
    s->initial_reset = s->legacy && s->resettable;
    rotmos_source_config_init(&config);
    config.read = read_source;
    config.reset = s->resettable ? reset_source : NULL;
    config.close = close_source;
    config.userdata = s;
    status = rotmos_source_new(&config, &s->native, error);
    return status;
}
static int source_new(lua_State *L)
{
    rotmos_error error;
    if (rl_source_create(L, 1, 0, &error) != ROTMOS_OK)
        return rl_error(L, &error);
    return 1;
}
rotmos_status rl_source_take(lua_State *L, int index, rotmos_reply *reply,
                             int code, const char *type, rotmos_error *error)
{
    struct rl_source *s = luaL_testudata(L, index, RL_SOURCE);
    rotmos_status status;
    if (s == NULL || !s->owned || s->native == NULL)
        return rl_fail(error, ROTMOS_STATE,
                       "source is closed or already transferred");
    lua_pushvalue(L, index);
    s->root = luaL_ref(L, LUA_REGISTRYINDEX);
    s->L = L;
    status = rotmos_reply_source_take(reply, code, type, s->native, error);
    if (status == ROTMOS_OK)
        s->owned = 0;
    else {
        luaL_unref(L, LUA_REGISTRYINDEX, s->root);
        s->root = LUA_NOREF;
    }
    return status;
}
rotmos_status rl_source_spool(lua_State *L, int index, rotmos_reply *reply,
                              int code, const char *type, const char *directory,
                              rotmos_error *error)
{
    struct rl_source *s = luaL_testudata(L, index, RL_SOURCE);
    rotmos_error cleanup;
    rotmos_status status, closed;
    if (s == NULL || !s->owned || s->native == NULL)
        return rl_fail(error, ROTMOS_STATE,
                       "source is closed or already transferred");
    lua_pushvalue(L, index);
    s->root = luaL_ref(L, LUA_REGISTRYINDEX);
    s->L = L;
    s->owned = 0;
    status = rotmos_reply_source_spooled(reply, code, type, s->native,
                                         directory, error);
    closed = rotmos_source_destroy(s->native, &cleanup);
    if (closed != ROTMOS_OK) {
        /* Public source destruction normally cannot fail after the synchronous
         * borrow has returned. Preserve ownership if the core rejects it. */
        s->owned = 1;
        luaL_unref(L, LUA_REGISTRYINDEX, s->root);
        s->root = LUA_NOREF;
        if (status == ROTMOS_OK) {
            status = closed;
            if (error != NULL)
                *error = cleanup;
        }
    }
    return status;
}
static int source_close(lua_State *L)
{
    struct rl_source *s = luaL_testudata(L, 1, RL_SOURCE);
    rotmos_error error;
    rotmos_status status;
    if (s == NULL)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_INVALID, "source required"), &error));
    if (s->native != NULL) {
        if (!s->owned)
            return rl_error(L, (rl_fail(&error, ROTMOS_STATE,
                                        "source is owned by transport"),
                                &error));
        s->L = L;
        status = rotmos_source_destroy(s->native, &error);
        if (status != ROTMOS_OK)
            return rl_error(L, &error);
    }
    if (s->error.status != ROTMOS_OK)
        return rl_error(L, &s->error);
    lua_pushboolean(L, 1);
    return 1;
}
static int source_gc(lua_State *L)
{
    struct rl_source *s = luaL_testudata(L, 1, RL_SOURCE);
    if (s != NULL && s->owned && s->native != NULL) {
        s->L = L;
        s->root = LUA_NOREF; /* GC cannot resurrect an owning registry root. */
        rotmos_source_destroy(s->native, NULL);
    }
    return 0;
}
static int source_resume(lua_State *L)
{
    struct rl_source *s = luaL_testudata(L, 1, RL_SOURCE);
    rotmos_error error;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_STATE, "source is closed"), &error));
    if (rotmos_source_resume(s->native, &error) != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
void rl_source_expire(struct rl_source *s)
{
    if (s != NULL && s->request_body != NULL && s->delegate != NULL) {
        rotmos_source_destroy(s->delegate, NULL);
        s->delegate = NULL;
    }
}
static rotmos_status reader_ready(struct rl_source *s, rotmos_error *error)
{
    if (s->delegate == NULL ||
        (s->request_body != NULL && s->request_body->native == NULL))
        return rl_fail(error, ROTMOS_STATE, "request body scope has closed");
    return ROTMOS_OK;
}
static rotmos_status reader_read(void *buffer, size_t capacity,
                                 size_t *produced, void *data,
                                 rotmos_error *error)
{
    struct rl_source *s = data;
    rotmos_status status = reader_ready(s, error);
    return status == ROTMOS_OK ? rotmos_source_read(s->delegate, buffer,
                                                    capacity, produced, error)
                               : status;
}
static rotmos_status reader_reset(void *data, rotmos_error *error)
{
    struct rl_source *s = data;
    rotmos_status status = reader_ready(s, error);
    return status == ROTMOS_OK ? rotmos_source_reset(s->delegate, error)
                               : status;
}
int rl_source_wrap_reader(lua_State *L, int owner, struct rl_request *request,
                          struct rl_body_result *result,
                          const rotmos_body_result *body, rotmos_error *error)
{
    struct rl_source *s;
    rotmos_source_config config;
    rotmos_status status;
    int index;
    owner = lua_absindex(L, owner);
    s = lua_newuserdatauv(L, sizeof(*s), 2);
    memset(s, 0, sizeof(*s));
    s->L = L;
    s->root = LUA_NOREF;
    s->owned = s->resettable = 1;
    s->request_body = request;
    s->result_body = result;
    index = lua_absindex(L, -1);
    luaL_setmetatable(L, RL_SOURCE);
    lua_pushvalue(L, owner);
    lua_setiuservalue(L, index, 1);
    if (request != NULL) {
        lua_getiuservalue(L, owner, 5);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setiuservalue(L, owner, 5);
        }
        lua_pushvalue(L, index);
        lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
        lua_pop(L, 1);
    }
    status = rotmos_body_result_open_reader(body, &s->delegate, error);
    if (status != ROTMOS_OK)
        return 0;
    rotmos_source_config_init(&config);
    config.read = reader_read;
    config.reset = reader_reset;
    config.close = close_source;
    config.userdata = s;
    status = rotmos_source_new(&config, &s->native, error);
    if (status != ROTMOS_OK) {
        rotmos_source_destroy(s->delegate, NULL);
        s->delegate = NULL;
        return 0;
    }
    if (result != NULL)
        ++result->readers;
    return 1;
}
static int source_read(lua_State *L)
{
    struct rl_source *s = luaL_testudata(L, 1, RL_SOURCE);
    rotmos_error error;
    rotmos_status status;
    size_t capacity = 8192, produced = 0;
    void *buffer;
    if (s == NULL || s->native == NULL || !s->owned)
        return rl_error(L, (rl_fail(&error, ROTMOS_STATE,
                                    "source is closed or transferred"),
                            &error));
    if ((!lua_isnoneornil(L, 2) && !rl_size(L, 2, &capacity)) || capacity == 0)
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "positive read capacity required"),
                            &error));
    buffer = lua_newuserdatauv(L, capacity, 0);
    s->L = L;
    status = rotmos_source_read(s->native, buffer, capacity, &produced, &error);
    if (status == ROTMOS_EOF || status == ROTMOS_AGAIN) {
        lua_pushnil(L);
        rl_sentinel(L, status);
        return 2;
    }
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushlstring(L, buffer, produced);
    return 1;
}
static int source_reset(lua_State *L)
{
    struct rl_source *s = luaL_testudata(L, 1, RL_SOURCE);
    rotmos_error error;
    if (s == NULL || s->native == NULL || !s->owned)
        return rl_error(L, (rl_fail(&error, ROTMOS_STATE,
                                    "source is closed or transferred"),
                            &error));
    s->L = L;
    if (rotmos_source_reset(s->native, &error) != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
void rl_sources_open(lua_State *L)
{
    static const luaL_Reg methods[] = {{"close", source_close},
                                       {"resume", source_resume},
                                       {"read", source_read},
                                       {"reset", source_reset},
                                       {NULL, NULL}};
    luaL_newmetatable(L, RL_SOURCE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pushcfunction(L, source_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    lua_pushcfunction(L, source_new);
    lua_setfield(L, -2, "source");
}
