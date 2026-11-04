/* Vectis's buffered REST JSON response, through the public JSON/reply API. */
#include "binding.h"
#include <rotmos/lonejson.h>

#define RL_REST "rotmos.rest_buffer"
struct rest_buffer {
    unsigned char *data;
    size_t length, capacity, bound, offset;
    rotmos_status failure;
    lonejson *runtime;
    lonejson_json_value value;
    rotmos_reply *reply;
};
static int rest_close(lua_State *L)
{
    struct rest_buffer *b = luaL_testudata(L, 1, RL_REST);
    if (b != NULL) {
        rotmos_reply_destroy(b->reply, NULL);
        b->reply = NULL;
        if (b->runtime != NULL) {
            lonejson_json_value_cleanup(&b->value);
            lonejson_free(b->runtime);
            b->runtime = NULL;
        }
        free(b->data);
        b->data = NULL;
    }
    return 0;
}
static int rest_sink(lua_State *L)
{
    struct rest_buffer *b = lua_touserdata(L, lua_upvalueindex(1));
    size_t length, needed, capacity;
    const char *chunk = luaL_checklstring(L, 1, &length);
    unsigned char *grown;
    if (length > b->bound - b->length) {
        b->failure = ROTMOS_LIMIT;
        return luaL_error(L, "REST JSON output exceeds max_bytes");
    }
    needed = b->length + length;
    if (needed > b->capacity) {
        capacity = b->capacity ? b->capacity : 4096;
        if (capacity > b->bound)
            capacity = b->bound;
        while (capacity < needed) {
            if (capacity > b->bound / 2) {
                capacity = b->bound;
                break;
            }
            capacity *= 2;
        }
        grown = realloc(b->data, capacity);
        if (grown == NULL) {
            b->failure = ROTMOS_NOMEM;
            return luaL_error(L, "allocating REST JSON output");
        }
        b->data = grown;
        b->capacity = capacity;
    }
    if (length)
        memcpy(b->data + b->length, chunk, length);
    b->length = needed;
    return 0;
}
static lonejson_read_result rest_read(void *data, unsigned char *buffer,
                                      size_t capacity)
{
    struct rest_buffer *b = data;
    lonejson_read_result result;
    size_t n = b->length - b->offset;
    memset(&result, 0, sizeof(result));
    if (n > capacity)
        n = capacity;
    if (n)
        memcpy(buffer, b->data + b->offset, n);
    b->offset += n;
    result.bytes_read = n;
    result.eof = b->offset == b->length;
    return result;
}
static int rest_json(lua_State *L)
{
    struct rest_buffer *b;
    rotmos_rest_reply_config config;
    rotmos_error error;
    lonejson_error cause;
    rotmos_bytes body, type;
    size_t status = 200;
    int owner, options, newline = 1, code;
    static const char *const fields[] = {"status",       "status_code",
                                         "content_type", "headers",
                                         "newline",      "max_bytes"};
    lua_settop(L, 2);
    if (lua_isnil(L, 2)) {
        lua_newtable(L);
        lua_replace(L, 2);
    }
    if (!lua_istable(L, 2))
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "REST JSON response options required"),
                            &error));
    if (!rl_fields(L, 2, fields, sizeof(fields) / sizeof(fields[0]), &error))
        return rl_error(L, &error);
    rl_table_copy(L, 2);
    options = lua_absindex(L, -1);
    rotmos_rest_reply_config_init(&config);
    if (!rl_field_size(L, options, "status_code", &status) ||
        !rl_field_size(L, options, "status", &status) || status < 100 ||
        status > 599 ||
        !rl_field_size(L, options, "max_bytes", &config.max_bytes) ||
        !rl_field_bool(L, options, "newline", &newline) ||
        !rl_field_string(L, options, "content_type", &config.content_type))
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "invalid REST JSON response options"),
                            &error));
    config.status = (int)status;
    config.newline_disabled = !newline;
    rl_field(L, options, "headers");
    if (!lua_isnil(L, -1) && !lua_istable(L, -1))
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "REST response headers must be a table"),
                            &error));
    lua_pop(L, 1);
    b = lua_newuserdatauv(L, sizeof(*b), 0);
    memset(b, 0, sizeof(*b));
    b->bound = config.max_bytes ? config.max_bytes : 10485760UL;
    owner = lua_absindex(L, -1);
    luaL_setmetatable(L, RL_REST);
    lua_toclose(L, owner);
    /* The released provider owns generic Lua JSON traversal and validation.
     * Its sink stops at the configured bound, before a large value is built. */
    lua_getglobal(L, "require");
    lua_pushliteral(L, "lonejson");
    lua_call(L, 1, 1);
    lua_getfield(L, -1, "encode_json_to_sink");
    lua_pushvalue(L, 1);
    lua_pushvalue(L, owner);
    lua_pushcclosure(L, rest_sink, 1);
    code = lua_pcall(L, 2, 0, 0);
    if (code != LUA_OK) {
        rl_fail(&error,
                b->failure           ? b->failure
                : code == LUA_ERRMEM ? ROTMOS_NOMEM
                                     : ROTMOS_INVALID,
                lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1)
                                               : "REST JSON encoding failed");
        error.source = ROTMOS_ERROR_LONEJSON;
        return rl_error(L, &error);
    }
    b->runtime = lonejson_new(NULL, &cause);
    if (b->runtime == NULL)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_NOMEM, "allocating REST JSON runtime"),
                &error));
    lonejson_json_value_init(b->runtime, &b->value);
    if (lonejson_json_value_set_reader(&b->value, rest_read, b, &cause) !=
        LONEJSON_STATUS_OK)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_INVALID, cause.message), &error));
    if (rotmos_reply_new(&b->reply, &error) != ROTMOS_OK ||
        rotmos_reply_rest_json(b->reply, &b->value, &config, &error) !=
            ROTMOS_OK)
        return rl_error(L, &error);
    body = rotmos_reply_body(b->reply);
    type = rotmos_reply_content_type(b->reply);
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, rotmos_reply_status_code(b->reply));
    lua_setfield(L, -2, "status");
    lua_pushlstring(L, type.data, type.length);
    lua_setfield(L, -2, "content_type");
    lua_pushlstring(L, body.data, body.length);
    lua_setfield(L, -2, "body");
    rl_field(L, options, "headers");
    lua_setfield(L, -2, "headers");
    return 1;
}
void rl_rest_open(lua_State *L)
{
    luaL_newmetatable(L, RL_REST);
    lua_pushcfunction(L, rest_close);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, rest_close);
    lua_setfield(L, -2, "__close");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, rest_json);
    lua_setfield(L, -2, "json");
    lua_pushinteger(L, ROTMOS_ERROR_LONEJSON);
    lua_setfield(L, -2, "ERROR_SOURCE_LONEJSON");
    lua_setfield(L, -2, "rest");
}
