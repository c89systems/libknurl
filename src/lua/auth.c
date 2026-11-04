#include "binding.h"
#define N(a) (sizeof(a) / sizeof((a)[0]))
/* Credential/session implementations remain host-owned. This is the existing
 * callback-provider declaration and decision-to-staged-reply conversion. */
static int modes(lua_State *L, int index, unsigned int *out)
{
    size_t n, i, value;
    const char *name;
    unsigned int flags = 0;
    index = lua_absindex(L, index);
    if (lua_isnil(L, index)) {
        *out = 0;
        return 1;
    }
    if (lua_isinteger(L, index)) {
        if (!rl_size(L, index, &value) || value > UINT_MAX)
            return 0;
        *out = (unsigned int)value;
        return 1;
    }
    n = lua_istable(L, index) ? lua_rawlen(L, index) : 1;
    for (i = 0; i < n; ++i) {
        if (lua_istable(L, index))
            lua_rawgeti(L, index, (lua_Integer)i + 1);
        else
            lua_pushvalue(L, index);
        if (lua_isinteger(L, -1)) {
            if (!rl_size(L, -1, &value) || value > UINT_MAX)
                return 0;
            flags |= (unsigned int)value;
            lua_pop(L, 1);
            continue;
        }
        name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
        if (name == NULL)
            return 0;
        if (!strcmp(name, "basic"))
            flags |= 1U;
        else if (!strcmp(name, "bearer"))
            flags |= 2U;
        else if (strcmp(name, "default"))
            return 0;
        lua_pop(L, 1);
    }
    *out = flags;
    return 1;
}
int rl_auth_prepare(lua_State *L, int options, rotmos_error *error)
{
    static const char *const allowed[] = {"provider", "kind",  "callback",
                                          "purpose",  "realm", "allowed_modes",
                                          "modes"};
    static const char *const inherit[] = {"kind", "purpose", "realm"};
    int base = lua_gettop(L), auth, provider, copy;
    size_t i;
    unsigned int flags = 0;
    const char *kind = NULL, *purpose = NULL, *realm = NULL;
    options = lua_absindex(L, options);
    rotmos_error_clear(error);
    rl_field(L, options, "auth");
    if (lua_isnil(L, -1)) {
        lua_settop(L, base);
        return 1;
    }
    auth = lua_absindex(L, -1);
    provider = auth;
    if (!lua_istable(L, auth) ||
        !rl_fields(L, auth, allowed, N(allowed), error))
        goto invalid;
    rl_field(L, auth, "provider");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1) ||
            !rl_fields(L, -1, allowed + 1, N(allowed) - 1, error))
            goto invalid;
        provider = lua_absindex(L, -1);
    }
    rl_table_copy(L, provider);
    copy = lua_absindex(L, -1);
    for (i = 0; i < N(inherit); ++i) {
        rl_field(L, auth, inherit[i]);
        if (!lua_isnil(L, -1))
            lua_setfield(L, copy, inherit[i]);
        else
            lua_pop(L, 1);
    }
    if (!rl_field_string(L, copy, "kind", &kind) ||
        !rl_field_string(L, copy, "purpose", &purpose) ||
        !rl_field_string(L, copy, "realm", &realm))
        goto invalid;
    if (kind == NULL || strcmp(kind, "callback")) {
        rl_fail(error, ROTMOS_NOT_SUPPORTED,
                "credential providers remain host-owned; supply a callback "
                "provider");
        goto invalid;
    }
    rl_field(L, copy, "callback");
    if (!lua_isfunction(L, -1))
        goto invalid;
    lua_pop(L, 1);
    for (i = 0; i < 4 && flags == 0; ++i) {
        rl_field(L, i % 2 ? provider : auth, i < 2 ? "allowed_modes" : "modes");
        if (!modes(L, -1, &flags))
            goto invalid;
        lua_pop(L, 1);
    }
    if (flags == 0)
        flags = 1;
    lua_pushinteger(L, flags);
    lua_setfield(L, copy, "allowed_modes");
    if (purpose == NULL) {
        lua_pushstring(L, "webdav");
        lua_setfield(L, copy, "purpose");
    }
    lua_pushvalue(L, copy);
    lua_setfield(L, options, "auth");
    lua_settop(L, base);
    return 1;
invalid:
    if (error->status == ROTMOS_OK)
        rl_fail(error, ROTMOS_INVALID,
                "invalid callback auth provider declaration");
    lua_settop(L, base);
    return 0;
}
rotmos_status rl_auth_apply(lua_State *L, int options, int view,
                            rotmos_request *request, rotmos_reply *reply,
                            int *finish, rotmos_error *error)
{
    static const char *fields[] = {
        "action", "status_code",      "location", "content_type",
        "body",   "www_authenticate", "principal"};
    int auth, result, snapshot;
    size_t i, code = 0, length;
    const char *action = "deny", *location = NULL,
               *type = "text/plain; charset=utf-8", *challenge = NULL,
               *principal = NULL;
    rotmos_bytes body;
    rotmos_status status;
    options = lua_absindex(L, options);
    view = lua_absindex(L, view);
    rl_field(L, options, "auth");
    if (lua_isnil(L, -1))
        return ROTMOS_OK;
    auth = lua_absindex(L, -1);
    rl_field(L, auth, "callback");
    lua_newtable(L);
    if (rotmos_request_header_first(request, "authorization", &body, NULL) ==
        ROTMOS_OK) {
        lua_pushlstring(L, body.data, body.length);
        lua_setfield(L, -2, "authorization");
    }
    rl_field(L, auth, "purpose");
    lua_setfield(L, -2, "purpose");
    rl_field(L, auth, "allowed_modes");
    lua_setfield(L, -2, "allowed_modes");
    body = rotmos_request_path(request);
    lua_pushlstring(L, body.data, body.length);
    lua_setfield(L, -2, "resource");
    lua_call(L, 1, 2);
    if (!lua_isnil(L, -1))
        return rl_callback_error(L, -1, error,
                                 "auth callback returned an error");
    lua_pop(L, 1);
    result = lua_absindex(L, -1);
    if (!lua_istable(L, result))
        return rl_fail(error, ROTMOS_INVALID,
                       "auth callback must return a decision table");
    lua_newtable(L);
    snapshot = lua_absindex(L, -1);
    for (i = 0; i < N(fields); ++i) {
        lua_getfield(L, result, fields[i]);
        if (i != 1 && lua_type(L, -1) == LUA_TNUMBER)
            (void)lua_tolstring(L, -1, NULL);
        lua_setfield(L, snapshot, fields[i]);
    }
    if (!rl_field_string(L, snapshot, "action", &action) ||
        !rl_field_size(L, snapshot, "status_code", &code) || code > 599 ||
        !rl_field_string(L, snapshot, "location", &location) ||
        !rl_field_string(L, snapshot, "content_type", &type) ||
        !rl_field_string(L, snapshot, "www_authenticate", &challenge) ||
        !rl_field_string(L, snapshot, "principal", &principal))
        return rl_fail(error, ROTMOS_INVALID, "invalid auth decision field");
    rl_field(L, snapshot, "body");
    if (lua_isnil(L, -1)) {
        body.data = !strcmp(action, "deny") ? "forbidden\n"
                                            : "authentication required\n";
        body.length = strlen(body.data);
    } else {
        if (lua_type(L, -1) != LUA_TSTRING)
            return rl_fail(error, ROTMOS_INVALID,
                           "auth response body must be bytes");
        body.data = lua_tolstring(L, -1, &body.length);
    }
    lua_pop(L, 1);
    if (!strcmp(action, "allow")) {
        if (principal != NULL && principal[0] != 0) {
            length = strlen(principal);
            /* Vectis's ordinary route request uses a 128-byte principal. */
            if (length > 127)
                length = 127;
            lua_pushlstring(L, principal, length);
            lua_setiuservalue(L, view, 3);
        }
        return rotmos_reply_header_add(reply, "cache-control", "no-store",
                                       error);
    }
    if (!strcmp(action, "required")) {
        if (code == 0)
            code = 401;
        if (challenge != NULL && challenge[0] != 0) {
            status = rotmos_reply_header_add(reply, "www-authenticate",
                                             challenge, error);
            if (status != ROTMOS_OK)
                return status;
        }
    } else if (!strcmp(action, "redirect")) {
        if (code == 0)
            code = 302;
        if (location != NULL && location[0] != 0) {
            status =
                rotmos_reply_header_add(reply, "location", location, error);
            if (status != ROTMOS_OK)
                return status;
        }
    } else if (!strcmp(action, "deny")) {
        if (code == 0)
            code = 403;
    } else
        return rl_fail(error, ROTMOS_INVALID,
                       "auth action must be allow, deny, required or redirect");
    status = rotmos_reply_bytes(reply, (int)code, type, body, error);
    *finish = status == ROTMOS_OK;
    return status;
}
