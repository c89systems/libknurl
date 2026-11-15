#include "binding.h"
#define RL_ACL "knurl.acl"
#define N(a) (sizeof(a) / sizeof((a)[0]))
struct acl_owner {
    knurl_acl *native;
};
static int strings(lua_State *L, int table, const char *key,
                   const char *const **out, size_t *count, knurl_error *error)
{
    const char **items;
    size_t n, i, length, array_key;
    int list;
    rl_field(L, table, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    if (!lua_istable(L, -1) || (n = lua_rawlen(L, -1)) > 1024)
        goto invalid;
    list = lua_absindex(L, -1);
    lua_pushnil(L);
    while (lua_next(L, list)) {
        if (!rl_size(L, -2, &array_key) || array_key == 0 || array_key > n)
            goto invalid;
        lua_pop(L, 1);
    }
    items = lua_newuserdatauv(L, n * sizeof(*items), 0);
    for (i = 0; i < n; ++i) {
        lua_rawgeti(L, list, (lua_Integer)i + 1);
        if (lua_type(L, -1) != LUA_TSTRING)
            goto invalid;
        items[i] = lua_tolstring(L, -1, &length);
        if (memchr(items[i], 0, length))
            goto invalid;
        lua_pop(L, 1);
    }
    *out = items;
    *count = n;
    return 1;
invalid:
    rl_fail(error, KNURL_INVALID,
            "ACL rules require arrays of at most 1024 strings without NUL");
    return 0;
}
static int rules(lua_State *L, int table, const char *key, knurl_acl_rules *out,
                 knurl_error *error)
{
    static const char *const fields[] = {"addresses", "countries", "unknown"};
    int index;
    rl_field(L, table, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    index = lua_absindex(L, -1);
    if (!lua_istable(L, index) ||
        !rl_fields(L, index, fields, N(fields), error) ||
        !rl_field_bool(L, index, "unknown", &out->unknown) ||
        !strings(L, index, "addresses", &out->addresses, &out->address_count,
                 error) ||
        !strings(L, index, "countries", &out->countries, &out->country_count,
                 error))
        return 0;
    return 1;
}
static int build(lua_State *L, int options, knurl_error *error)
{
    static const char *const fields[] = {"policy",         "allow",
                                         "deny",           "use_peer",
                                         "dns_refresh_ms", "dns_max_addresses",
                                         "dns_timeout_ms", "country_database",
                                         "max_heap_bytes"};
    knurl_acl_config config;
    struct acl_owner *owner;
    const char *policy = NULL;
    size_t refresh = 0, timeout = 0;
    int top = lua_gettop(L);
    options = lua_absindex(L, options);
    knurl_error_clear(error);
    knurl_acl_config_init(&config);
    if (!lua_istable(L, options) ||
        !rl_fields(L, options, fields, N(fields), error) ||
        !rl_field_string(L, options, "policy", &policy) ||
        !rl_field_string(L, options, "country_database",
                         &config.country_database) ||
        !rl_field_size(L, options, "dns_refresh_ms", &refresh) ||
        refresh > LONG_MAX ||
        !rl_field_size(L, options, "dns_timeout_ms", &timeout) ||
        timeout > LONG_MAX ||
        !rl_field_size(L, options, "dns_max_addresses",
                       &config.dns_max_addresses) ||
        !rl_field_size(L, options, "max_heap_bytes", &config.max_heap_bytes) ||
        !rl_field_bool(L, options, "use_peer", &config.use_peer))
        goto invalid;
    if (policy) {
        if (!strcmp(policy, "deny"))
            config.policy = KNURL_ACL_DENY;
        else if (strcmp(policy, "allow"))
            goto invalid;
    }
    config.dns_refresh_ms = (long)refresh;
    config.dns_timeout_ms = (long)timeout;
    if (!rules(L, options, "allow", &config.allow, error) ||
        !rules(L, options, "deny", &config.deny, error))
        goto invalid;
    owner = lua_newuserdatauv(L, sizeof(*owner), 0);
    owner->native = NULL;
    luaL_setmetatable(L, RL_ACL);
    if (knurl_acl_new(&config, &owner->native, error) != KNURL_OK) {
        lua_settop(L, top);
        return 0;
    }
    lua_rotate(L, top + 1, 1);
    lua_settop(L, top + 1);
    return 1;
invalid:
    lua_settop(L, top);
    if (error->status == KNURL_OK)
        rl_fail(error, KNURL_INVALID, "invalid ACL configuration");
    return 0;
}
int rl_acl_option(lua_State *L, int options, knurl_acl **out,
                  knurl_error *error)
{
    struct acl_owner *owner;
    int top = lua_gettop(L);
    options = lua_absindex(L, options);
    *out = NULL;
    knurl_error_clear(error);
    rl_field(L, options, "acl");
    if (lua_isnil(L, -1)) {
        lua_settop(L, top);
        return 1;
    }
    if (lua_istable(L, -1)) {
        if (!build(L, -1, error)) {
            lua_settop(L, top);
            return 0;
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, options, "acl");
    }
    owner = luaL_testudata(L, -1, RL_ACL);
    if (owner == NULL || owner->native == NULL) {
        lua_settop(L, top);
        rl_fail(error, KNURL_INVALID,
                "ACL must be an open ACL object or declaration");
        return 0;
    }
    *out = owner->native;
    lua_settop(L, top);
    return 1;
}
static int l_new(lua_State *L)
{
    knurl_error error;
    knurl_error_clear(&error);
    if (!build(L, 1, &error))
        return rl_error(L, &error);
    return 1;
}
static struct acl_owner *active(lua_State *L, knurl_error *error)
{
    struct acl_owner *owner = luaL_testudata(L, 1, RL_ACL);
    if (owner == NULL || owner->native == NULL) {
        rl_fail(error, KNURL_STATE, "ACL is closed");
        return NULL;
    }
    return owner;
}
static int l_close(lua_State *L)
{
    struct acl_owner *owner = luaL_testudata(L, 1, RL_ACL);
    if (owner != NULL && owner->native != NULL) {
        (void)knurl_acl_close(owner->native, NULL);
        owner->native = NULL;
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int l_refresh(lua_State *L)
{
    knurl_error error;
    struct acl_owner *owner = active(L, &error);
    knurl_status status;
    if (owner == NULL)
        return rl_error(L, &error);
    status = knurl_acl_refresh(owner->native, &error);
    if (status != KNURL_OK && status != KNURL_AGAIN)
        return rl_error(L, &error);
    lua_pushboolean(L, status == KNURL_OK);
    return 1;
}
static int l_check(lua_State *L)
{
    knurl_error error;
    struct acl_owner *owner = active(L, &error);
    knurl_mmdb_address peer, effective;
    int allowed;
    const char *p, *e = NULL;
    size_t length;
    if (owner == NULL)
        return rl_error(L, &error);
    if (lua_type(L, 2) != LUA_TSTRING)
        goto invalid;
    p = lua_tolstring(L, 2, &length);
    if (memchr(p, 0, length))
        goto invalid;
    if (!lua_isnoneornil(L, 3)) {
        if (lua_type(L, 3) != LUA_TSTRING)
            goto invalid;
        e = lua_tolstring(L, 3, &length);
        if (memchr(e, 0, length))
            goto invalid;
    }
    if (knurl_mmdb_address_parse(p, &peer, &error) != KNURL_OK ||
        (e && knurl_mmdb_address_parse(e, &effective, &error) != KNURL_OK) ||
        knurl_acl_check(owner->native, &peer, e ? &effective : NULL, &allowed,
                        &error) != KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, allowed);
    return 1;
invalid:
    rl_fail(&error, KNURL_INVALID,
            "numeric peer and optional effective address required");
    return rl_error(L, &error);
}
void rl_acl_open(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"check", l_check}, {"refresh", l_refresh}, {"close", l_close},
        {"__gc", l_close},  {"__close", l_close},   {NULL, NULL}};
    luaL_newmetatable(L, RL_ACL);
    luaL_setfuncs(L, methods, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    lua_pushcfunction(L, l_new);
    lua_setfield(L, -2, "acl");
}
