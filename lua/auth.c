#include "binding.h"

#define RL_AUTH "knurl.auth"

static int policy(lua_State *L, int index, knurl_auth **out, knurl_error *error)
{
    knurl_auth **owner = luaL_testudata(L, index, RL_AUTH);
    *out = NULL;
    if (lua_isnoneornil(L, index))
        return 1;
    if (owner == NULL || *owner == NULL) {
        rl_fail(error, KNURL_INVALID, "expected an open knurl auth policy");
        return 0;
    }
    *out = *owner;
    return 1;
}

int rl_auth_option(lua_State *L, int index, knurl_auth **out,
                   knurl_error *error)
{
    int ok;
    rl_field(L, index, "auth");
    ok = policy(L, -1, out, error);
    lua_pop(L, 1);
    if (!ok)
        snprintf(error->detail, sizeof(error->detail), "%s", "auth");
    return ok;
}

static int close_policy(lua_State *L)
{
    knurl_auth **owner = luaL_checkudata(L, 1, RL_AUTH);
    knurl_auth_close(*owner);
    *owner = NULL;
    return 0;
}

static int smtp(lua_State *L, knurl_auth_smtp *config, knurl_error *error)
{
    static const char *const fields[] = {"url",      "from",    "username",
                                         "password", "ca_file", "timeout_ms"};
    size_t timeout = 0;
    int ok;
    rl_field(L, 1, "smtp");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    if (!rl_fields(L, -1, fields, RL_COUNT(fields), error))
        return 0;
    ok = rl_field_string(L, -1, "url", &config->url) &&
         rl_field_string(L, -1, "from", &config->from) &&
         rl_field_string(L, -1, "username", &config->username) &&
         rl_field_string(L, -1, "password", &config->password) &&
         rl_field_string(L, -1, "ca_file", &config->ca_file) &&
         rl_field_size(L, -1, "timeout_ms", &timeout) && timeout <= LONG_MAX;
    lua_pop(L, 1);
    if (!ok)
        rl_fail(error, KNURL_INVALID, "invalid SMTP configuration");
    config->timeout_ms = (long)timeout;
    return ok;
}

static int oidc(lua_State *L, knurl_auth_oidc *config, knurl_error *error)
{
    static const char *const fields[] = {"issuer",    "audience", "jwks_uri",
                                         "jwks_json", "scope",    "token_type"};
    int ok;
    rl_field(L, 1, "oidc");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    if (!rl_fields(L, -1, fields, RL_COUNT(fields), error))
        return 0;
    ok = rl_field_string(L, -1, "issuer", &config->issuer) &&
         rl_field_string(L, -1, "audience", &config->audience) &&
         rl_field_string(L, -1, "jwks_uri", &config->jwks_uri) &&
         rl_field_string(L, -1, "jwks_json", &config->jwks_json) &&
         rl_field_string(L, -1, "scope", &config->scope) &&
         rl_field_string(L, -1, "token_type", &config->token_type);
    lua_pop(L, 1);
    if (!ok)
        rl_fail(error, KNURL_INVALID, "invalid OIDC configuration");
    return ok;
}

static int new_policy(lua_State *L)
{
    static const char *const fields[] = {
        "name", "store",     "method",        "login", "session_seconds",
        "smtp", "client_id", "client_secret", "oidc"};
    knurl_auth_config config;
    knurl_auth **owner;
    knurl_error error;
    knurl_status status = KNURL_OK;
    const char *store = NULL, *method = "session", *login = "password";
    size_t lifetime = 0;
    knurl_auth_config_init(&config);
    if (!rl_fields(L, 1, fields, RL_COUNT(fields), &error))
        return rl_error(L, &error);
    if (!rl_field_string(L, 1, "name", &config.name) ||
        !rl_field_string(L, 1, "store", &store) ||
        !rl_field_string(L, 1, "client_id", &config.client_id) ||
        !rl_field_string(L, 1, "client_secret", &config.client_secret) ||
        !rl_field_string(L, 1, "method", &method) ||
        !rl_field_string(L, 1, "login", &login) ||
        !rl_field_size(L, 1, "session_seconds", &lifetime) ||
        lifetime > 2592000UL ||
        (strcmp(method, "session") && strcmp(method, "machine")) ||
        (strcmp(login, "password") && strcmp(login, "totp") &&
         strcmp(login, "email")))
        return rl_result(
            L, rl_fail(&error, KNURL_INVALID, "invalid auth configuration"),
            &error);
    config.session_seconds = (unsigned long)lifetime;
    if (!strcmp(method, "machine"))
        config.method = KNURL_AUTH_MACHINE;
    if (!strcmp(login, "totp"))
        config.login = KNURL_AUTH_LOGIN_TOTP;
    else if (!strcmp(login, "email"))
        config.login = KNURL_AUTH_LOGIN_EMAIL;
    if (!smtp(L, &config.smtp, &error) || !oidc(L, &config.oidc, &error))
        return rl_error(L, &error);
    owner = lua_newuserdatauv(L, sizeof(*owner), 0);
    *owner = NULL;
    luaL_setmetatable(L, RL_AUTH);
    if (store != NULL)
        status = knurl_auth_json_open(store, &config.json, &error);
    if (status == KNURL_OK)
        status = knurl_auth_new(&config, owner, &error);
    knurl_auth_json_close(config.json);
    return status == KNURL_OK ? 1 : rl_error(L, &error);
}

int rl_set_auth(lua_State *L)
{
    const struct rl_route *route = luaL_testudata(L, 1, RL_ROUTE);
    const struct rl_group *group = luaL_testudata(L, 1, RL_GROUP);
    const struct rl_server *server = route != NULL   ? route->server
                                     : group != NULL ? group->server
                                                     : NULL;
    knurl_auth *auth;
    knurl_error error;
    knurl_status status;
    if (server == NULL || server->native == NULL || server->running)
        return rl_result(L,
                         rl_fail(&error, KNURL_STATE,
                                 "setting auth requires an inactive server"),
                         &error);
    if (!policy(L, 2, &auth, &error))
        return rl_error(L, &error);
    status = route != NULL
                 ? knurl_route_set_auth(route->native, auth, &error)
                 : knurl_route_group_set_auth(group->native, auth, &error);
    return rl_result(L, status, &error);
}

static int endpoint(lua_State *L)
{
    static const char *const fields[] = {"path", "auth", "acl"};
    struct rl_server *server = luaL_testudata(L, 1, RL_SERVER);
    const char *path = NULL;
    knurl_auth *auth;
    knurl_acl *acl;
    struct rl_route *route;
    knurl_error error;
    knurl_status status;
    knurl_auth_endpoint kind =
        (knurl_auth_endpoint)lua_tointeger(L, lua_upvalueindex(1));
    if (server == NULL || server->native == NULL || server->running)
        return rl_result(L,
                         rl_fail(&error, KNURL_STATE,
                                 "declaring auth requires an inactive server"),
                         &error);
    if (!rl_fields(L, 2, fields, RL_COUNT(fields), &error) ||
        !rl_auth_option(L, 2, &auth, &error) ||
        !rl_acl_option(L, 2, &acl, &error))
        return rl_error(L, &error);
    if (!rl_field_string(L, 2, "path", &path) || path == NULL || auth == NULL)
        return rl_result(
            L, rl_fail(&error, KNURL_INVALID, "auth and path are required"),
            &error);
    route = rl_route_new(L, server);
    status = knurl_auth_route(knurl_server_router(server->native), path, auth,
                              kind, &route->native, &error);
    if (status == KNURL_OK && acl != NULL)
        status = knurl_route_set_acl(route->native, acl, &error);
    return status == KNURL_OK ? 1 : rl_error(L, &error);
}

void rl_auth_endpoints(lua_State *L)
{
    static const char *const names[] = {"login", "email_code", "logout"};
    size_t i;
    for (i = 0; i < RL_COUNT(names); ++i) {
        lua_pushinteger(L, (lua_Integer)i);
        lua_pushcclosure(L, endpoint, 1);
        lua_setfield(L, -2, names[i]);
    }
}

void rl_auth_open(lua_State *L)
{
    static const luaL_Reg methods[] = {{"close", close_policy},
                                       {"__gc", close_policy},
                                       {"__close", close_policy},
                                       {NULL, NULL}};
    luaL_newmetatable(L, RL_AUTH);
    luaL_setfuncs(L, methods, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    lua_pushcfunction(L, new_policy);
    lua_setfield(L, -2, "auth");
}
