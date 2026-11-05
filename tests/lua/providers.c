/* Public-API host provider fixture; no Vectis or core-private layouts. */
#include <rotmos/rotmos.h>
#include <rotmos/lua.h>
#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>
#define OWNER "rotmos.test.packed_owner"
struct owner {
    rotmos_asset_provider *assets;
    rotmos_storage_provider *storage;
};
struct opened {
    int kind, next, read;
};
static int owners_closed, assets_opened, assets_closed, range_reads;
static size_t maximum_read, reads;
static int owner_gc(lua_State *L)
{
    struct owner *owner = luaL_checkudata(L, 1, OWNER);
    if (owner->storage != NULL)
        rotmos_storage_provider_close(owner->storage, NULL);
    if (owner->assets != NULL)
        rotmos_asset_provider_close(owner->assets, NULL);
    owner->storage = NULL;
    owner->assets = NULL;
    ++owners_closed;
    return 0;
}
static void metadata(rotmos_asset_info *info, int kind)
{
    memset(info, 0, sizeof(*info));
    info->kind = kind == 0 ? ROTMOS_ASSET_DIRECTORY : ROTMOS_ASSET_FILE;
    info->size.low = kind == 1 ? 6 : kind == 2 ? 131072 : kind == 3 ? 3 : 0;
    if (kind != 0) {
        info->etag = "\"pack\"";
        info->content_type = "application/octet-stream";
    }
}
static rotmos_status read_asset(rotmos_u64 offset, void *buffer,
                                size_t capacity, size_t *produced, void *data,
                                rotmos_error *error)
{
    struct opened *opened = data;
    size_t position, count, length;
    rotmos_status status = rotmos_u64_to_size(offset, &position, error);
    *produced = 0;
    if (status != ROTMOS_OK)
        return status;
    length = opened->kind == 1 ? 6 : opened->kind == 2 ? 131072 : 3;
    if (!opened->read++ && position != 0) {
        if (position != 2)
            return ROTMOS_STATE;
        ++range_reads;
    }
    if (position >= length)
        return ROTMOS_EOF;
    count = length - position;
    if (count > capacity)
        count = capacity;
    if (count > maximum_read)
        maximum_read = count;
    ++reads;
    if (opened->kind == 2)
        memset(buffer, 'p', count);
    else
        memcpy(buffer, (opened->kind == 1 ? "packed" : "A\0B") + position,
               count);
    *produced = count;
    return ROTMOS_OK;
}
static rotmos_status next_entry(rotmos_asset_entry *entry, void *data,
                                rotmos_error *error)
{
    static const char *const names[] = {"asset", "large", "binary"};
    struct opened *opened = data;
    (void)error;
    if (opened->next == 3)
        return ROTMOS_EOF;
    entry->name = names[opened->next];
    metadata(&entry->info, ++opened->next);
    return ROTMOS_OK;
}
static void close_asset(void *data)
{
    ++assets_closed;
    free(data);
}
static rotmos_status open_asset(const char *path, rotmos_asset **out,
                                void *data, rotmos_error *error)
{
    struct opened *opened;
    rotmos_asset_config config;
    rotmos_status status;
    int kind = !strcmp(path, "/")                                        ? 0
               : !strcmp(path, "/asset") || !strcmp(path, "/index.html") ? 1
               : !strcmp(path, "/large")                                 ? 2
               : !strcmp(path, "/binary")                                ? 3
                                                                         : -1;
    (void)data;
    if (kind < 0)
        return ROTMOS_EOF;
    opened = calloc(1, sizeof(*opened));
    if (opened == NULL)
        return ROTMOS_NOMEM;
    opened->kind = kind;
    rotmos_asset_config_init(&config);
    metadata(&config.info, kind);
    config.read = kind ? read_asset : NULL;
    config.next = kind ? NULL : next_entry;
    config.close = close_asset;
    config.userdata = opened;
    status = rotmos_asset_new(&config, out, error);
    if (status != ROTMOS_OK)
        free(opened);
    else
        ++assets_opened;
    return status;
}
static int create(lua_State *L)
{
    struct owner *owner;
    rotmos_asset_provider_config config;
    rotmos_error error;
    int index, top;
    owner = lua_newuserdatauv(L, sizeof(*owner), 0);
    memset(owner, 0, sizeof(*owner));
    luaL_setmetatable(L, OWNER);
    index = lua_absindex(L, -1);
    rotmos_asset_provider_config_init(&config);
    config.open = open_asset;
    if (rotmos_asset_provider_new(&config, &owner->assets, &error) !=
            ROTMOS_OK ||
        rotmos_storage_provider_assets(owner->assets, &owner->storage,
                                       &error) != ROTMOS_OK)
        return luaL_error(L, "%s", error.message);
    top = lua_gettop(L);
    if (rotmos_lua_push_asset_provider(L, owner->assets, top + 100, &error) !=
            ROTMOS_INVALID ||
        lua_gettop(L) != top ||
        rotmos_lua_push_storage_provider(L, NULL, index, &error) !=
            ROTMOS_INVALID ||
        lua_gettop(L) != top)
        return luaL_error(L,
                          "failed interop must preserve stack and ownership");
    if (rotmos_lua_push_asset_provider(L, owner->assets, index, &error) !=
            ROTMOS_OK ||
        rotmos_lua_push_storage_provider(L, owner->storage, index, &error) !=
            ROTMOS_OK)
        return luaL_error(L, "%s", error.message);
    lua_pushvalue(L, index);
    return 3;
}
static int stats(lua_State *L)
{
    lua_newtable(L);
#define COUNT(name, value)                                                     \
    do {                                                                       \
        lua_pushinteger(L, (lua_Integer)(value));                              \
        lua_setfield(L, -2, name);                                             \
    } while (0)
    COUNT("owners_closed", owners_closed);
    COUNT("assets_opened", assets_opened);
    COUNT("assets_closed", assets_closed);
    COUNT("maximum_read", maximum_read);
    COUNT("reads", reads);
    COUNT("range_reads", range_reads);
#undef COUNT
    return 1;
}
ROTMOS_API int luaopen_rotmos_provider_fixture(lua_State *L);
int luaopen_rotmos_provider_fixture(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"create", create}, {"stats", stats}, {NULL, NULL}};
    luaL_newmetatable(L, OWNER);
    lua_pushcfunction(L, owner_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newlib(L, functions);
    return 1;
}
