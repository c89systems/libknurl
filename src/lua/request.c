#include "binding.h"
struct rl_request *rl_request(lua_State *L, int index)
{
    return luaL_testudata(L, index, RL_REQUEST);
}
void rl_request_push(lua_State *L, struct rl_request *r)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, r->root);
}
static int start_trampoline(lua_State *L)
{
    struct rl_call *c = lua_touserdata(L, 1);
    struct rl_request *r;
    r = lua_newuserdatauv(L, sizeof(*r), 7);
    memset(r, 0, sizeof(*r));
    r->native = c->native;
    r->root = LUA_NOREF;
    luaL_setmetatable(L, RL_REQUEST);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->server->root);
    lua_setiuservalue(L, -2, 1);
    lua_pushvalue(L, -1);
    r->root = luaL_ref(L, LUA_REGISTRYINDEX);
    rotmos_request_set_userdata(c->native, r);
    return 0;
}
rotmos_status rl_request_ensure(struct rl_server *s, rotmos_request *request,
                                rotmos_error *error)
{
    struct rl_call c;
    if (rotmos_request_userdata(request) != NULL)
        return ROTMOS_OK;
    memset(&c, 0, sizeof(c));
    c.server = s;
    c.native = request;
    return rl_protect(s->L, start_trampoline, &c, error);
}
rotmos_status rl_request_start(rotmos_server *server, rotmos_request *request,
                               void *data, rotmos_error *error)
{
    struct rl_server *s = data;
    rotmos_status status = rl_request_ensure(s, request, error);
    (void)server;
    return status == ROTMOS_OK ? rl_policy_start(s, request, error) : status;
}
void rl_request_close(rotmos_server *server, rotmos_request *request,
                      const rotmos_request_outcome *outcome, void *data)
{
    struct rl_request *r = rotmos_request_userdata(request);
    struct rl_server *s = data;
    size_t i, n;
    int top = lua_gettop(s->L);
    (void)server;
    rl_policy_close(s, request, outcome);
    r = rotmos_request_userdata(request);
    if (r == NULL)
        return;
    r->native = NULL;
    rl_request_push(s->L, r);
    lua_getiuservalue(s->L, -1, 5);
    if (lua_istable(s->L, -1)) {
        n = lua_rawlen(s->L, -1);
        for (i = 0; i < n; ++i) {
            lua_rawgeti(s->L, -1, (lua_Integer)i + 1);
            rl_source_expire(luaL_testudata(s->L, -1, RL_SOURCE));
            lua_pop(s->L, 1);
        }
    }
    lua_pop(s->L, 1);
    lua_pushnil(s->L);
    lua_setiuservalue(s->L, -2, 5);
    lua_settop(s->L, top);
    luaL_unref(s->L, LUA_REGISTRYINDEX, r->root);
    r->root = LUA_NOREF;
}
static void bytes_field(lua_State *L, const char *name, rotmos_bytes value)
{
    if (value.data != NULL)
        lua_pushlstring(L, value.data, value.length);
    else
        lua_pushnil(L);
    lua_setfield(L, -2, name);
}
static void endpoint(lua_State *L, rotmos_endpoint value)
{
    lua_createtable(L, 0, 2);
    bytes_field(L, "address", value.address);
    lua_pushinteger(L, value.port);
    lua_setfield(L, -2, "port");
}
int rl_request_check(lua_State *L, int index, struct rl_request **r,
                     rotmos_error *e)
{
    *r = rl_request(L, index);
    if (*r == NULL || (*r)->native == NULL) {
        rl_fail(e, ROTMOS_STATE, "request scope has closed");
        return 0;
    }
    return 1;
}
static int valid(lua_State *L, struct rl_request **r, rotmos_error *e)
{
    return rl_request_check(L, 1, r, e);
}
static void push_principal(lua_State *L, int index, rotmos_request *request)
{
    rotmos_bytes principal = rotmos_webdav_request_principal(request);
    if (principal.data != NULL)
        lua_pushlstring(L, principal.data, principal.length);
    else
        lua_getiuservalue(L, index, 3);
}
static int request_principal(lua_State *L)
{
    struct rl_request *request;
    rotmos_error error;
    if (!valid(L, &request, &error))
        return rl_error(L, &error);
    push_principal(L, 1, request->native);
    return 1;
}
static int snapshot(lua_State *L)
{
    struct rl_request *r;
    rotmos_error e;
    rotmos_request *n;
    rotmos_bytes body;
    if (!valid(L, &r, &e))
        return rl_error(L, &e);
    n = r->native;
    lua_createtable(L, 0, 15);
    push_principal(L, 1, n);
    lua_setfield(L, -2, "principal");
    bytes_field(L, "method", rotmos_request_method_token(n));
    bytes_field(L, "version", rotmos_request_version(n));
    bytes_field(L, "target", rotmos_request_target(n));
    bytes_field(L, "raw_path", rotmos_request_raw_path(n));
    bytes_field(L, "path", rotmos_request_path(n));
    bytes_field(L, "query_string", rotmos_request_query(n));
    bytes_field(L, "authority", rotmos_request_authority(n));
    bytes_field(L, "ip", rotmos_request_effective_address(n));
    bytes_field(L, "correlation_id", rotmos_request_correlation_id(n));
    endpoint(L, rotmos_request_peer(n));
    lua_setfield(L, -2, "peer");
    endpoint(L, rotmos_request_local(n));
    lua_setfield(L, -2, "local_endpoint");
    lua_pushboolean(L, rotmos_request_is_tls(n));
    lua_setfield(L, -2, "tls");
    lua_pushboolean(L, rotmos_body_kind(rotmos_request_body(n)) ==
                           ROTMOS_BODY_FILE);
    lua_setfield(L, -2, "body_spooled");
    lua_pushboolean(L, r->streaming_upload);
    lua_setfield(L, -2, "body_streaming_upload");
    {
        const char *path;
        int fd;
        if (rotmos_body_file(rotmos_request_body(n), &fd, &path, NULL) ==
            ROTMOS_OK) {
            lua_pushstring(L, path);
            lua_setfield(L, -2, "body_path");
        }
    }
    rl_push_size(L, rotmos_request_listener_index(n) + 1);
    lua_setfield(L, -2, "listener_index");
    if (rotmos_request_body_bytes(n, &body, NULL) == ROTMOS_OK) {
        lua_pushlstring(L, body.data != NULL ? body.data : "", body.length);
        lua_setfield(L, -2, "body");
        rl_push_size(L, body.length);
        lua_setfield(L, -2, "body_size");
    } else {
        rotmos_u64 length;
        if (rotmos_body_length(rotmos_request_body(n), &length, NULL) ==
            ROTMOS_OK)
            rl_push_u64(L, length);
        else
            lua_pushinteger(L, 0);
        lua_setfield(L, -2, "body_size");
    }
    return 1;
}
static int lookup(lua_State *L, int kind)
{
    struct rl_request *r;
    rotmos_error e;
    const char *name;
    size_t length;
    rotmos_bytes value;
    rotmos_status status;
    if (!valid(L, &r, &e))
        return rl_error(L, &e);
    if (lua_type(L, 2) != LUA_TSTRING)
        return rl_error(
            L,
            (rl_fail(&e, ROTMOS_INVALID, "lookup name must be a string"), &e));
    name = lua_tolstring(L, 2, &length);
    if (memchr(name, 0, length))
        return rl_error(
            L, (rl_fail(&e, ROTMOS_INVALID, "lookup name contains NUL"), &e));
    status =
        kind == 0   ? rotmos_request_header_first(r->native, name, &value, &e)
        : kind == 1 ? rotmos_request_query_first(r->native, name, &value, &e)
                    : rotmos_request_path_param(r->native, name, &value, &e);
    if (status == ROTMOS_EOF) {
        lua_pushnil(L);
        return 1;
    }
    if (status != ROTMOS_OK)
        return rl_error(L, &e);
    lua_pushlstring(L, value.data, value.length);
    return 1;
}
static int header(lua_State *L) { return lookup(L, 0); }
static int query(lua_State *L) { return lookup(L, 1); }
static int param(lua_State *L) { return lookup(L, 2); }
static int pairs_list(lua_State *L, int queries)
{
    struct rl_request *r;
    rotmos_error e;
    size_t i, n;
    rotmos_header value;
    if (!valid(L, &r, &e))
        return rl_error(L, &e);
    n = queries ? rotmos_request_query_count(r->native)
                : rotmos_request_header_count(r->native);
    lua_createtable(L, (int)n, 0);
    for (i = 0; i < n; ++i) {
        if (queries)
            rotmos_request_query_at(r->native, i, &value, NULL);
        else
            rotmos_request_header_at(r->native, i, &value, NULL);
        lua_createtable(L, 0, 2);
        bytes_field(L, "name", value.name);
        bytes_field(L, "value", value.value);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}
static int headers(lua_State *L) { return pairs_list(L, 0); }
static int queries(lua_State *L) { return pairs_list(L, 1); }
static int request_body(lua_State *L)
{
    struct rl_request *r;
    rotmos_error e;
    rotmos_bytes body;
    if (!valid(L, &r, &e))
        return rl_error(L, &e);
    if (rotmos_request_body_bytes(r->native, &body, &e) != ROTMOS_OK)
        return rl_error(L, &e);
    lua_pushlstring(L, body.data != NULL ? body.data : "", body.length);
    return 1;
}
static int request_body_resume(lua_State *L)
{
    struct rl_request *r;
    rotmos_error error;
    if (!valid(L, &r, &error) ||
        rotmos_request_body_resume(r->native, &error) != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int producer_resume(lua_State *L)
{
    struct rl_request *request;
    rotmos_error error;
    if (!valid(L, &request, &error) ||
        rotmos_response_producer_resume(
            rotmos_request_response(request->native), &error) != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int request_event(lua_State *L)
{
    struct rl_request *r;
    rotmos_error error;
    const char *name;
    size_t length;
    if (!valid(L, &r, &error))
        return rl_error(L, &error);
    if (lua_type(L, 2) != LUA_TSTRING)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_INVALID, "event name must be a string"),
                &error));
    name = lua_tolstring(L, 2, &length);
    if (memchr(name, 0, length))
        return rl_error(
            L, (rl_fail(&error, ROTMOS_INVALID, "event name contains NUL"),
                &error));
    if (rotmos_request_event(r->native, name, &error) != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
rotmos_status rl_reply_headers(lua_State *L, int table, rotmos_reply *reply,
                               rotmos_error *error)
{
    const char *key, *value;
    size_t n, m, i, count;
    int item;
    table = lua_absindex(L, table);
    count = lua_rawlen(L, table);
    if (count != 0) {
        for (i = 0; i < count; ++i) {
            lua_rawgeti(L, table, (lua_Integer)i + 1);
            item = lua_absindex(L, -1);
            if (!lua_istable(L, item))
                return rl_fail(error, ROTMOS_INVALID,
                               "ordered headers require name/value tables");
            key = value = NULL;
            if (!rl_field_string(L, item, "name", &key) ||
                !rl_field_string(L, item, "value", &value) || key == NULL ||
                value == NULL)
                return rl_fail(error, ROTMOS_INVALID,
                               "header name/value strings required");
            if (rotmos_reply_header_add(reply, key, value, error) != ROTMOS_OK)
                return error->status;
            lua_pop(L, 1);
        }
        return ROTMOS_OK;
    }
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        if (!lua_isstring(L, -2) || !lua_isstring(L, -1))
            return rl_fail(error, ROTMOS_INVALID,
                           "header map values must be strings");
        lua_pushvalue(L, -2);
        key = lua_tolstring(L, -1, &n);
        value = lua_tolstring(L, -2, &m);
        if (memchr(key, 0, n) || memchr(value, 0, m))
            return rl_fail(error, ROTMOS_INVALID, "header contains NUL");
        if (rotmos_reply_header_add(reply, key, value, error) != ROTMOS_OK)
            return error->status;
        lua_pop(L, 2);
    }
    return ROTMOS_OK;
}
/* Snapshot getters while the outer callback trampoline owns protection.
 * Keep the original value untouched through all before/after callbacks. */
static int reply_snapshot(lua_State *L, int index)
{
    static const char *fields[] = {
        "status",    "status_code", "headers",           "content_type",
        "file_path", "file",        "stream_source",     "spooled_source",
        "json",      "xml",         "json_materialized", "xml_materialized",
        "body"};
    int snapshot, i;
    lua_Integer status;
    index = lua_absindex(L, index);
    lua_newtable(L);
    snapshot = lua_absindex(L, -1);
    lua_getfield(L, index, "status_code");
    if (lua_isnil(L, -1) || (rl_integer(L, -1, &status) && status == 0)) {
        lua_pop(L, 1);
        lua_getfield(L, index, "status");
    }
    lua_setfield(L, snapshot, "status_code");
    for (i = 2; i < (int)(sizeof(fields) / sizeof(fields[0])); ++i) {
        lua_getfield(L, index, fields[i]);
        if ((i == 3 || i == 4 || i == 5 || i == 12) &&
            lua_type(L, -1) == LUA_TNUMBER)
            (void)lua_tolstring(L, -1, NULL);
        lua_setfield(L, snapshot, fields[i]);
        if (i >= 4 && i <= 11) {
            rl_field(L, snapshot, fields[i]);
            if (!lua_isnil(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            lua_pop(L, 1);
        }
    }
    return snapshot;
}
rotmos_status rl_reply(lua_State *L, int index, rotmos_reply *reply,
                       rotmos_request *request, rotmos_error *error)
{
    rotmos_status status;
    rotmos_bytes body;
    size_t code = 200;
    const char *type, *declared_type = NULL, *file = NULL;
    int source;
    index = lua_absindex(L, index);
    if (lua_isnil(L, index))
        return rotmos_reply_status(reply, 204, error);
    if (lua_isstring(L, index)) {
        body.data = lua_tolstring(L, index, &body.length);
        return rotmos_reply_bytes(reply, 200, "text/plain; charset=utf-8", body,
                                  error);
    }
    if (!lua_istable(L, index))
        return rl_fail(
            error, ROTMOS_INVALID,
            "route callback must return nil, string or response table");
    index = reply_snapshot(L, index);
    if (!rl_field_size(L, index, "status_code", &code))
        return rl_fail(error, ROTMOS_INVALID, "invalid response status");
    if (code > 599 || code < 100 ||
        !rl_field_string(L, index, "content_type", &declared_type) ||
        !rl_field_string(L, index, "file", &file) ||
        !rl_field_string(L, index, "file_path", &file))
        return rl_fail(error, ROTMOS_INVALID, "invalid response declaration");
    type = declared_type != NULL ? declared_type : "text/plain; charset=utf-8";
    rl_field(L, index, "headers");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            return rl_fail(error, ROTMOS_INVALID,
                           "response headers must be a table");
        status = rl_reply_headers(L, -1, reply, error);
        if (status != ROTMOS_OK)
            return status;
    }
    lua_pop(L, 1);
    if (file != NULL)
        return rotmos_reply_file(reply, (int)code, type, file, error);
    rl_field(L, index, "stream_source");
    source = lua_absindex(L, -1);
    if (lua_istable(L, source)) {
        status = rl_source_create(L, source, 1, error);
        if (status != ROTMOS_OK)
            return status;
        source = lua_absindex(L, -1);
    }
    if (!lua_isnil(L, source))
        return rl_source_take(L, source, reply, (int)code, type, error);
    lua_pop(L, 1);
    rl_field(L, index, "spooled_source");
    if (!lua_isnil(L, -1)) {
        rotmos_server_effective config;
        source = lua_absindex(L, -1);
        if (lua_istable(L, source)) {
            status = rl_source_create(L, source, 1, error);
            if (status != ROTMOS_OK)
                return status;
            source = lua_absindex(L, -1);
        }
        status = rotmos_server_effective_config(rotmos_request_server(request),
                                                &config, error);
        if (status != ROTMOS_OK)
            return status;
        return rl_source_spool(L, source, reply, (int)code, type,
                               config.config.limits.body_spool_directory,
                               error);
    }
    lua_pop(L, 1);
    rl_field(L, index, "json");
    if (!lua_isnil(L, -1))
        return rl_codec_reply(L, -1, 0, 0, reply, (int)code, declared_type,
                              error);
    lua_pop(L, 1);
    rl_field(L, index, "xml");
    if (!lua_isnil(L, -1))
        return rl_codec_reply(L, -1, 1, 0, reply, (int)code, declared_type,
                              error);
    lua_pop(L, 1);
    rl_field(L, index, "json_materialized");
    if (!lua_isnil(L, -1))
        return rl_codec_reply(L, -1, 0, 1, reply, (int)code, declared_type,
                              error);
    lua_pop(L, 1);
    rl_field(L, index, "xml_materialized");
    if (!lua_isnil(L, -1))
        return rl_codec_reply(L, -1, 1, 1, reply, (int)code, declared_type,
                              error);
    lua_pop(L, 1);
    rl_field(L, index, "body");
    if (lua_isnil(L, -1))
        return rotmos_reply_status(reply, (int)code, error);
    if (lua_type(L, -1) != LUA_TSTRING)
        return rl_fail(error, ROTMOS_INVALID,
                       "response body must be a binary string");
    body.data = lua_tolstring(L, -1, &body.length);
    return rotmos_reply_bytes(reply, (int)code, type, body, error);
}
static int route_trampoline(lua_State *L)
{
    struct rl_call *c = lua_touserdata(L, 1);
    int view, options, value, invoke = 1;
    rl_request_push(L, c->request);
    view = lua_absindex(L, -1);
    rl_rule_options(L, c->rule);
    options = lua_absindex(L, -1);
    if (c->hook == 3) {
        c->status = rl_auth_apply(L, options, view, c->native, c->reply,
                                  &c->finish, c->error);
        return 0;
    }
    rl_field(L, options, c->name);
    if (lua_isnil(L, -1))
        invoke = 0;
    if (invoke) {
        if (!lua_isfunction(L, -1)) {
            c->status = rl_fail(c->error, ROTMOS_INVALID,
                                "route hook must be a function");
            return 0;
        }
        lua_pushvalue(L, view);
        if (c->hook == 2) {
            if (c->request->native_reply)
                rl_reply_view_push(L, c);
            else
                lua_getiuservalue(L, view, 2);
            lua_call(L, 2, 2);
        } else
            lua_call(L, 1, 2);
        if (!lua_isnil(L, -1)) {
            c->status = rl_callback_error(L, -1, c->error,
                                          "route callback returned an error");
            return 0;
        }
        lua_pop(L, 1);
        if (c->hook != 0 &&
            (lua_isnil(L, -1) ||
             (lua_isboolean(L, -1) && lua_toboolean(L, -1)) ||
             (c->reply_view != NULL && lua_touserdata(L, -1) == c->reply_view)))
            invoke = 0;
    }
    if (invoke && c->hook != 0 && !lua_istable(L, -1)) {
        c->status = rl_fail(c->error, ROTMOS_INVALID,
                            c->hook == 1 ? "Lua route before hook must return "
                                           "nil, true, or a response table"
                                         : "Lua route after hook must return "
                                           "nil, true, or a response table");
        return 0;
    }
    if (c->hook == 1) {
        if (invoke) {
            c->status = rl_reply(L, -1, c->reply, c->native, c->error);
            c->finish = c->status == ROTMOS_OK;
        }
        return 0;
    }
    if (invoke) {
        lua_pushvalue(L, -1);
        lua_setiuservalue(L, view, 2);
        c->request->native_reply = 0;
    }
    if (c->hook == 4) {
        c->finish = invoke;
        return 0;
    }
    if (c->hook == 0)
        return 0;
    if (!c->request->native_reply &&
        (c->rule->is_group || c->rule->group == NULL)) {
        lua_getiuservalue(L, view, 2);
        value = lua_absindex(L, -1);
        c->status = rl_reply(L, value, c->reply, c->native, c->error);
        if (c->status == ROTMOS_OK && c->request->upload_finishing)
            c->status = rl_upload_close(c->request, c->error);
    }
    return 0;
}
static rotmos_status route_call(rotmos_request *n, rotmos_reply *reply,
                                int *finish, void *data, rotmos_error *error,
                                int hook, const char *name)
{
    struct rl_rule *r = data;
    struct rl_call c;
    rotmos_status status;
    memset(&c, 0, sizeof(c));
    c.server = r->server;
    c.rule = r;
    c.request = rotmos_request_userdata(n);
    c.native = n;
    c.reply = reply;
    c.hook = hook;
    c.name = name;
    c.error = error;
    if (c.request == NULL)
        return rl_fail(error, ROTMOS_STATE, "Lua request association missing");
    if (r->is_upload)
        c.request->streaming_upload = 1;
    if (!r->is_group && hook == 3)
        c.request->native_reply = r->native_reply;
    if (!r->is_group && r->native_reply && hook == 1)
        c.request->native_reply = 1;
    status = rl_protect(c.server->L, route_trampoline, &c, error);
    if (c.reply_view != NULL)
        c.reply_view->native = NULL;
    if (finish != NULL)
        *finish = c.finish;
    return status == ROTMOS_OK ? c.status : status;
}
rotmos_status rl_handler(rotmos_request *n, rotmos_reply *r, void *d,
                         rotmos_error *e)
{
    return route_call(n, r, NULL, d, e, 0, "handler");
}
rotmos_status rl_before(rotmos_request *n, rotmos_reply *r, int *f, void *d,
                        rotmos_error *e)
{
    return route_call(n, r, f, d, e, 1, "before");
}
rotmos_status rl_auth(rotmos_request *n, rotmos_reply *r, int *f, void *d,
                      rotmos_error *e)
{
    return route_call(n, r, f, d, e, 3, "auth");
}
rotmos_status rl_after(rotmos_request *n, rotmos_reply *r, int *f, void *d,
                       rotmos_error *e)
{
    return route_call(n, r, f, d, e, 2, "after");
}
rotmos_status rl_rest_validate(rotmos_request *n, const void *decoded,
                               rotmos_reply *r, int *f, void *d,
                               rotmos_error *e)
{
    (void)decoded;
    return route_call(n, r, f, d, e, 4, "validate");
}
void rl_requests_open(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"snapshot", snapshot},
        {"principal", request_principal},
        {"header", header},
        {"query", query},
        {"param", param},
        {"headers", headers},
        {"queries", queries},
        {"body_bytes", request_body},
        {"json_record", rl_request_json_record},
        {"json_array_each", rl_request_json_array_each},
        {"xml_record", rl_request_xml_record},
        {"body_reader", rl_request_body_reader},
        {"body_materialize", rl_request_body_materialize},
        {"body_copy", rl_request_body_copy},
        {"body_spill", rl_request_body_spill},
        {"body_resume", request_body_resume},
        {"producer_resume", producer_resume},
        {"event", request_event},
        {NULL, NULL}};
    luaL_newmetatable(L, RL_REQUEST);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pop(L, 1);
}
