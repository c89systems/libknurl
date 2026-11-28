#ifndef KNURL_LUA_SOURCE_H
#define KNURL_LUA_SOURCE_H

#include <knurl/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One Knurl-owned public Lua file. The path is relative to a Lua module
 * directory. The source is an exact byte view and may lack a trailing NUL. */
typedef struct knurl_lua_source {
    const char *path;
    knurl_bytes source;
} knurl_lua_source;

/** Number of Knurl-owned Lua files available from this library. */
KNURL_API size_t knurl_lua_source_count(void);
/** Borrow a zero-based record for the lifetime of the loaded library. NULL
 * means out of range. The order is by path. No Lua runtime is required. */
KNURL_API const knurl_lua_source *knurl_lua_source_at(size_t index);

#ifdef __cplusplus
}
#endif

#endif
