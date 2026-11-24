#include "binding.h"
#include <lonejson.h>

#define JSON_LIMIT (8UL * 1024UL * 1024UL)

struct rl_json {
    lonejson *runtime;
    lonejson_writer writer;
    char *bytes;
    size_t length, capacity;
    int initialized;
};

void rl_json_free(struct rl_json *json)
{
    if (json == NULL)
        return;
    if (json->initialized)
        lonejson_writer_cleanup(&json->writer);
    if (json->runtime != NULL)
        lonejson_free(json->runtime);
    free(json->bytes);
    free(json);
}

static lonejson_status append(void *data, const void *bytes, size_t length,
                              lonejson_error *error)
{
    struct rl_json *json = data;
    size_t needed, capacity;
    char *grown;
    (void)error;
    if (length > JSON_LIMIT - json->length)
        return LONEJSON_STATUS_OVERFLOW;
    if (length == 0)
        return LONEJSON_STATUS_OK;
    needed = json->length + length;
    if (needed > json->capacity) {
        capacity = json->capacity != 0 ? json->capacity : 4096;
        while (capacity < needed)
            capacity *= 2;
        grown = realloc(json->bytes, capacity);
        if (grown == NULL)
            return LONEJSON_STATUS_ALLOCATION_FAILED;
        json->bytes = grown;
        json->capacity = capacity;
    }
    memcpy(json->bytes + json->length, bytes, length);
    json->length += length;
    return LONEJSON_STATUS_OK;
}

static lonejson_status write_value(lua_State *, int, lonejson_writer *,
                                   unsigned int, lonejson_error *);

static lonejson_status write_table(lua_State *L, int index,
                                   lonejson_writer *writer, unsigned int depth,
                                   lonejson_error *error)
{
    size_t length = lua_rawlen(L, index), count = 0, key, i;
    lonejson_status status;
    const char *name;
    int strings = 0, indices = 0;

    if (depth == 32)
        return LONEJSON_STATUS_OVERFLOW;
    if (!lua_checkstack(L, 3))
        return LONEJSON_STATUS_ALLOCATION_FAILED;
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) == LUA_TSTRING)
            strings = 1;
        else if (lua_isinteger(L, -2) && rl_size(L, -2, &key) && key != 0 &&
                 key <= length) {
            indices = 1;
            ++count;
        } else {
            return LONEJSON_STATUS_TYPE_MISMATCH;
        }
        lua_pop(L, 1);
    }
    if (indices) {
        if (strings || count != length)
            return LONEJSON_STATUS_TYPE_MISMATCH;
        status = lonejson_writer_begin_array(writer, error);
        for (i = 0; i < length && status == LONEJSON_STATUS_OK; ++i) {
            lua_rawgeti(L, index, (lua_Integer)i + 1);
            status = write_value(L, -1, writer, depth + 1, error);
            lua_pop(L, 1);
        }
        return status == LONEJSON_STATUS_OK
                   ? lonejson_writer_end_array(writer, error)
                   : status;
    }
    status = lonejson_writer_begin_object(writer, error);
    if (status != LONEJSON_STATUS_OK)
        return status;
    lua_pushnil(L);
    while (lua_next(L, index)) {
        name = lua_tolstring(L, -2, &length);
        status = lonejson_writer_key(writer, name, length, error);
        if (status == LONEJSON_STATUS_OK)
            status = write_value(L, -1, writer, depth + 1, error);
        if (status != LONEJSON_STATUS_OK)
            return status;
        lua_pop(L, 1);
    }
    return lonejson_writer_end_object(writer, error);
}

static lonejson_status write_value(lua_State *L, int index,
                                   lonejson_writer *writer, unsigned int depth,
                                   lonejson_error *error)
{
    const char *text;
    size_t length;
    switch (lua_type(L, index)) {
    case LUA_TSTRING:
        text = lua_tolstring(L, index, &length);
        return lonejson_writer_string(writer, text, length, error);
    case LUA_TBOOLEAN:
        return lonejson_writer_bool(writer, lua_toboolean(L, index), error);
    case LUA_TNUMBER:
        if (lua_isinteger(L, index))
            return lonejson_writer_i64(
                writer, (lonejson_int64)lua_tointeger(L, index), error);
        return lonejson_writer_f64(writer, (double)lua_tonumber(L, index),
                                   error);
    case LUA_TTABLE:
        return write_table(L, index, writer, depth, error);
    default:
        return LONEJSON_STATUS_TYPE_MISMATCH;
    }
}

knurl_status rl_json_reply(lua_State *L, int index, knurl_reply *reply,
                           int code, const char *type, struct rl_json **out,
                           knurl_error *error)
{
    struct rl_json *json;
    lonejson_error diagnostics;
    lonejson_status status;
    knurl_bytes result;
    json = calloc(1, sizeof(*json));
    if (json == NULL)
        return rl_fail(error, KNURL_NOMEM, "allocating JSON response");
    *out = json;
    json->runtime = lonejson_new(NULL, &diagnostics);
    if (json->runtime == NULL)
        return rl_fail(error, KNURL_NOMEM, "initializing JSON response");
    status = lonejson_writer_init_sink(json->runtime, &json->writer, append,
                                       json, &diagnostics);
    if (status == LONEJSON_STATUS_OK) {
        json->initialized = 1;
        status = write_value(L, index, &json->writer, 0, &diagnostics);
    }
    if (status == LONEJSON_STATUS_OK)
        status = lonejson_writer_finish(&json->writer, &diagnostics);
    if (status != LONEJSON_STATUS_OK)
        return rl_fail(
            error, KNURL_STATE,
            "response JSON must be finite, at most 32 levels and 8 MiB");
    result.data = json->bytes;
    result.length = json->length;
    return knurl_reply_bytes(
        reply, code, type != NULL ? type : "application/json", result, error);
}
