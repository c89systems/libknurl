#include "binding.h"

#define MAX_CHUNK 65536

struct stream {
    struct rl_work work;
    struct rl_cleanup cleanup;
    knurl_source *source;
    knurl_error error;
    knurl_status status;
    int ref, sse;
    unsigned char *chunk;
    size_t capacity, length, offset;
};

static int event_field(lua_State *L, int index, const char *name,
                       knurl_bytes *bytes, int *present)
{
    rl_field(L, index, name);
    *present = !lua_isnil(L, -1);
    if (*present) {
        if (lua_type(L, -1) != LUA_TSTRING)
            return 0;
        bytes->data = lua_tolstring(L, -1, &bytes->length);
    }
    lua_pop(L, 1);
    return 1;
}

static knurl_status encode_event(lua_State *L, struct stream *s,
                                 knurl_error *error)
{
    static const char *const fields[] = {"comment", "id", "event", "data",
                                         "retry_ms"};
    knurl_sse_event event;
    size_t retry = 0;
    int index = lua_absindex(L, -1);
    knurl_sse_event_init(&event);
    if (lua_type(L, index) == LUA_TSTRING) {
        event.has_data = 1;
        event.data.data = lua_tolstring(L, index, &event.data.length);
    } else {
        if (!rl_fields(L, index, fields, RL_COUNT(fields), error))
            return error->status;
        if (!event_field(L, index, "comment", &event.comment,
                         &event.has_comment) ||
            !event_field(L, index, "id", &event.id, &event.has_id) ||
            !event_field(L, index, "event", &event.event, &event.has_event) ||
            !event_field(L, index, "data", &event.data, &event.has_data))
            return rl_fail(error, KNURL_INVALID,
                           "SSE event fields must be strings");
        rl_field(L, index, "retry_ms");
        event.has_retry = !lua_isnil(L, -1);
        if (event.has_retry && !rl_size(L, -1, &retry))
            return rl_fail(error, KNURL_INVALID, "invalid SSE retry_ms");
        lua_pop(L, 1);
        if ((event.has_id && memchr(event.id.data, 0, event.id.length)) ||
            (event.has_event &&
             memchr(event.event.data, 0, event.event.length)))
            return rl_fail(error, KNURL_INVALID,
                           "SSE id and event cannot contain NUL");
        if (event.has_retry &&
            knurl_u64_from_size(retry, &event.retry_ms, error) != KNURL_OK)
            return error->status;
    }
    return knurl_sse_encode(&event, s->chunk, s->capacity, &s->length, error);
}

static int pull(lua_State *L)
{
    struct stream *s = lua_touserdata(L, 1);
    knurl_bytes bytes;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->ref);
    lua_pushinteger(L, (lua_Integer)s->capacity);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) {
        s->status = KNURL_EOF;
    } else if (s->sse) {
        s->status = encode_event(L, s, &s->error);
    } else if (lua_type(L, -1) == LUA_TSTRING) {
        bytes.data = lua_tolstring(L, -1, &bytes.length);
        if (bytes.length == 0 || bytes.length > s->capacity)
            s->status = rl_fail(&s->error, KNURL_LIMIT,
                                "stream chunk length must be in 1..capacity");
        else {
            memcpy(s->chunk, bytes.data, bytes.length);
            s->length = bytes.length;
        }
    } else
        s->status = rl_fail(&s->error, KNURL_INVALID,
                            "stream producer must return bytes or nil");
    return 0;
}

static knurl_status work(knurl_task *task, struct rl_work *job,
                         knurl_error *error)
{
    struct stream *s = (struct stream *)job;
    lua_State *L = job->server->worker;
    int result;
    (void)task;
    if (!lua_checkstack(L, 2))
        s->status =
            rl_fail(&s->error, KNURL_NOMEM, "reserving Lua stream stack");
    else {
        lua_pushcfunction(L, pull);
        lua_pushlightuserdata(L, s);
        result = lua_pcall(L, 1, 0, 0);
        if (result != LUA_OK)
            s->status = rl_lua_failure(L, result, "lua_stream", &s->error);
        lua_settop(L, 1);
    }
    if (s->status != KNURL_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, s->ref);
        s->ref = LUA_NOREF;
    }
    if (s->status != KNURL_OK && s->status != KNURL_EOF) {
        if (error != NULL)
            *error = s->error;
        return s->status;
    }
    return KNURL_OK;
}

static void free_stream(struct stream *s)
{
    free(s->chunk);
    free(s);
}

static void drop_producer(lua_State *L, struct rl_cleanup *cleanup)
{
    struct stream *s =
        (struct stream *)((char *)cleanup - offsetof(struct stream, cleanup));
    if (L != NULL)
        luaL_unref(L, LUA_REGISTRYINDEX, s->ref);
    free_stream(s);
}

static void dispose(struct stream *s)
{
    if (s->ref != LUA_NOREF) {
        free(s->chunk);
        s->chunk = NULL;
        rl_cleanup_later(s->work.server, &s->cleanup, drop_producer);
    } else
        free_stream(s);
}

static void completed(struct rl_work *work, knurl_status status,
                      const knurl_error *error)
{
    struct stream *s = (struct stream *)work;
    if (s->source == NULL) {
        dispose(s);
        return;
    }
    if (status != KNURL_OK) {
        s->status = status;
        s->error = *error;
    }
    (void)knurl_source_resume(s->source, NULL);
}

static knurl_status read_chunk(void *buffer, size_t capacity, size_t *produced,
                               void *data, knurl_error *error)
{
    struct stream *s = data;
    knurl_status status;
    size_t count;
    if (s->work.task != NULL || s->work.pending)
        return KNURL_AGAIN;
    if (s->status != KNURL_OK) {
        if (s->status != KNURL_EOF && error != NULL)
            *error = s->error;
        return s->status;
    }
    if (s->offset != s->length) {
        count = s->length - s->offset;
        if (count > capacity)
            count = capacity;
        memcpy(buffer, s->chunk + s->offset, count);
        s->offset += count;
        *produced = count;
        return KNURL_OK;
    }
    s->capacity = capacity < MAX_CHUNK ? capacity : MAX_CHUNK;
    if (s->chunk == NULL) {
        s->chunk = malloc(MAX_CHUNK);
        if (s->chunk == NULL)
            return rl_fail(error, KNURL_NOMEM, "allocating stream chunk");
    }
    s->offset = s->length = 0;
    status = rl_work_start(&s->work, error);
    return status == KNURL_OK ? KNURL_AGAIN : status;
}

static void close_source(void *data, knurl_status outcome)
{
    struct stream *s = data;
    (void)outcome;
    s->source = NULL;
    if (s->work.task != NULL || s->work.pending)
        rl_work_cancel(&s->work);
    else
        dispose(s);
}

knurl_status rl_stream_reply(lua_State *L, int index, struct rl_server *server,
                             knurl_reply *reply, int code, const char *type,
                             int sse, knurl_error *error)
{
    knurl_source_config config;
    struct stream *s;
    knurl_status status;
    index = lua_absindex(L, index);
    if (!lua_isfunction(L, index))
        return rl_fail(error, KNURL_INVALID,
                       "stream or SSE requires a producer function");
    lua_pushvalue(L, index);
    {
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        s = calloc(1, sizeof(*s));
        if (s == NULL) {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return rl_fail(error, KNURL_NOMEM, "allocating Lua stream");
        }
        s->ref = ref;
    }
    s->work.server = server;
    s->work.run = work;
    s->work.complete = completed;
    s->sse = sse;
    knurl_source_config_init(&config);
    config.read = read_chunk;
    config.close = close_source;
    config.userdata = s;
    status = knurl_source_new(&config, &s->source, error);
    if (status == KNURL_OK)
        status = knurl_reply_source_take(reply, code,
                                         type != NULL ? type
                                         : sse ? "text/event-stream"
                                               : "application/octet-stream",
                                         s->source, error);
    if (status != KNURL_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, s->ref);
        s->ref = LUA_NOREF;
        if (s->source != NULL)
            (void)knurl_source_destroy(s->source, NULL);
        else
            free_stream(s);
    }
    return status;
}
