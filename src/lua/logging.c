#include "binding.h"
#include <rotmos/pslog.h>
#include <errno.h>

#define RL_LOG_REFS "rotmos.logger_refs"
struct log_refs {
    pslog_lua_logger_ref root, access;
};
static void ref_init(pslog_lua_logger_ref *ref)
{
    memset(ref, 0, sizeof(*ref));
    ref->size = sizeof(*ref);
    ref->abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
    ref->registry_ref = LUA_NOREF;
}
void rl_logging_init(struct rl_server *server)
{
    ref_init(&server->logger);
    ref_init(&server->access_logger);
}
void rl_logging_cleanup(struct rl_server *server)
{
    pslog_lua_unref_logger(&server->access_logger);
    pslog_lua_unref_logger(&server->logger);
    rl_logging_init(server);
}
static int refs_gc(lua_State *L)
{
    struct log_refs *refs = luaL_checkudata(L, 1, RL_LOG_REFS);
    pslog_lua_unref_logger(&refs->access);
    pslog_lua_unref_logger(&refs->root);
    return 0;
}
void rl_logging_open(lua_State *L)
{
    luaL_newmetatable(L, RL_LOG_REFS);
    lua_pushcfunction(L, refs_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}
static int retain(lua_State *L, int index, pslog_lua_logger_ref *ref,
                  rotmos_error *error)
{
    pslog_lua_logger_view view;
    int code;
    if (lua_isnoneornil(L, index))
        return 1;
    memset(&view, 0, sizeof(view));
    view.size = sizeof(view);
    view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
    code = pslog_lua_check_logger(L, index, &view);
    if (code != 0 || !(view.flags & PSLOG_LUA_LOGGER_NATIVE_SINK)) {
        rl_fail(error, ROTMOS_INVALID,
                "server logging needs a live pslog logger with a native sink");
        return 0;
    }
    code = pslog_lua_ref_logger(L, index, NULL, 0, ref);
    if (code != 0) {
        rl_fail(error, code == ENOMEM ? ROTMOS_NOMEM : ROTMOS_INVALID,
                "retaining pslog logger failed");
        return 0;
    }
    return 1;
}
static int level(lua_State *L, int index, pslog_level *out)
{
    size_t length;
    const char *name;
    if (lua_type(L, index) != LUA_TSTRING)
        return 0;
    name = lua_tolstring(L, index, &length);
    return memchr(name, 0, length) == NULL && pslog_parse_level(name, out);
}
static int configure(lua_State *L, struct rl_server *server, int logger,
                     int access, rotmos_error *error)
{
    static const char *const fields[] = {"enabled",
                                         "logger",
                                         "success_level",
                                         "client_error_level",
                                         "server_error_level",
                                         "status_levels"};
    static const char *const names[] = {"success_level", "client_error_level",
                                        "server_error_level"};
    rotmos_access_log_config config;
    rotmos_access_log_status_level_rule
        rules[ROTMOS_ACCESS_LOG_MAX_STATUS_LEVEL_RULES];
    pslog_level *levels[3];
    struct log_refs *refs;
    rotmos_status status;
    size_t i, count = 0;
    int enabled = 1, has_code;
    lua_Integer code;
    logger = lua_absindex(L, logger);
    access = lua_absindex(L, access);
    refs = lua_newuserdatauv(L, sizeof(*refs), 0);
    ref_init(&refs->root);
    ref_init(&refs->access);
    luaL_setmetatable(L, RL_LOG_REFS);
    rotmos_access_log_config_init(&config);
    levels[0] = &config.success_level;
    levels[1] = &config.client_error_level;
    levels[2] = &config.server_error_level;
    if (lua_isboolean(L, access))
        config.logger_disabled = !lua_toboolean(L, access);
    else if (!lua_isnoneornil(L, access)) {
        if (!lua_istable(L, access) ||
            !rl_fields(L, access, fields, sizeof(fields) / sizeof(fields[0]),
                       error) ||
            !rl_field_bool(L, access, "enabled", &enabled))
            goto invalid;
        config.logger_disabled = !enabled;
        for (i = 0; i < 3; ++i) {
            rl_field(L, access, names[i]);
            if (!lua_isnil(L, -1) && !level(L, -1, levels[i]))
                goto invalid;
            lua_pop(L, 1);
        }
        rl_field(L, access, "status_levels");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1))
                goto invalid;
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                if (count == ROTMOS_ACCESS_LOG_MAX_STATUS_LEVEL_RULES)
                    goto invalid;
                code = lua_tointegerx(L, -2, &has_code);
                if (!has_code || code < 100 || code > 599 ||
                    !level(L, -1, &rules[count].level))
                    goto invalid;
                for (i = 0; i < count; ++i)
                    if (rules[i].status == (unsigned int)code)
                        goto invalid;
                rules[count++].status = (unsigned int)code;
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    config.status_level_rules = rules;
    config.status_level_rule_count = count;
    if (!retain(L, logger, &refs->root, error))
        goto failed;
    if (lua_istable(L, access)) {
        rl_field(L, access, "logger");
        if (!retain(L, -1, &refs->access, error))
            goto failed;
        lua_pop(L, 1);
    }
    config.logger = refs->access.borrowed;
    status = rotmos_server_set_pslog(server->native, refs->root.borrowed,
                                     &config, error);
    if (status != ROTMOS_OK)
        goto failed;
    rl_logging_cleanup(server);
    server->logger = refs->root;
    server->access_logger = refs->access;
    ref_init(&refs->root);
    ref_init(&refs->access);
    return 1;
invalid:
    rl_fail(error, ROTMOS_INVALID, "invalid access_log declaration");
failed:
    pslog_lua_unref_logger(&refs->access);
    pslog_lua_unref_logger(&refs->root);
    ref_init(&refs->access);
    ref_init(&refs->root);
    return 0;
}
int rl_logging_config(lua_State *L, int options, struct rl_server *server,
                      rotmos_error *error)
{
    int top = lua_gettop(L), logger, access, result;
    rl_field(L, options, "logger");
    logger = lua_absindex(L, -1);
    rl_field(L, options, "access_log");
    access = lua_absindex(L, -1);
    result = configure(L, server, logger, access, error);
    lua_settop(L, top);
    return result;
}
int rl_set_pslog(lua_State *L)
{
    struct rl_server *server = rl_server(L, 1);
    rotmos_error error;
    if (server == NULL || server->native == NULL || server->root != LUA_NOREF)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_STATE, "inactive server required"),
                &error));
    lua_settop(L, 3);
    if (!configure(L, server, 2, 3, &error))
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
