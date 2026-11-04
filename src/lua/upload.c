#include "binding.h"
struct upload_call {
    struct rl_rule *rule;
    struct rl_request *request;
    const void *bytes;
    size_t length, consumed;
    int phase;
    rotmos_status status;
    rotmos_error *error;
};
void rl_rule_options(lua_State *L, struct rl_rule *rule)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, rule->server->root);
    lua_getiuservalue(L, -1, 2);
    lua_rawgeti(L, -1, rule->root);
    lua_getiuservalue(L, -1, 1);
}
int rl_upload_prepare(lua_State *L, int index, rotmos_error *error)
{
    static const char *const fields[] = {"open", "on_chunk", "on_complete",
                                         "close"};
    size_t i;
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        rl_field(L, index, fields[i]);
        if (!lua_isfunction(L, -1) && (i == 1 || !lua_isnil(L, -1))) {
            rl_fail(error, ROTMOS_INVALID,
                    "upload requires on_chunk and optional function callbacks");
            return 0;
        }
        lua_pop(L, 1);
    }
    return 1;
}
static int upload_trampoline(lua_State *L)
{
    static const char *const names[] = {"open", "on_chunk", "on_complete",
                                        "close"};
    struct upload_call *c = lua_touserdata(L, 1);
    int view, invoke;
    rl_request_push(L, c->request);
    view = lua_absindex(L, -1);
    rl_rule_options(L, c->rule);
    rl_field(L, -1, names[c->phase]);
    invoke = !lua_isnil(L, -1);
    if (invoke) {
        lua_pushvalue(L, view);
        if (c->phase == 1)
            lua_pushlstring(L, c->bytes, c->length);
        if (c->phase != 0)
            lua_getiuservalue(L, view, 4);
        lua_call(L,
                 c->phase == 0   ? 1
                 : c->phase == 1 ? 3
                                 : 2,
                 c->phase == 3 ? 0 : 2);
        if (c->phase == 3)
            return 0;
        if (c->phase == 1 && lua_isnil(L, -2) &&
            rl_is_sentinel(L, -1, ROTMOS_AGAIN)) {
            c->status = ROTMOS_AGAIN;
            return 0;
        }
        if (!lua_isnil(L, -1)) {
            c->status = rl_callback_error(L, -1, c->error,
                                          "upload callback returned an error");
            return 0;
        }
        lua_pop(L, 1);
    } else if (c->phase == 0)
        lua_newtable(L);
    if (c->phase == 0) {
        lua_setiuservalue(L, view, 4);
        c->request->upload_opened = 1;
    } else if (c->phase == 1) {
        if (lua_isnil(L, -1) || (lua_isboolean(L, -1) && lua_toboolean(L, -1)))
            c->consumed = c->length;
        else if (!rl_size(L, -1, &c->consumed) || c->consumed == 0 ||
                 c->consumed > c->length)
            c->status = rl_fail(c->error, ROTMOS_INVALID,
                                "upload chunk must return nil, true, a "
                                "positive consumed prefix or nil, AGAIN");
    } else if (c->phase == 2) {
        if (!invoke)
            lua_pushnil(L);
        lua_setiuservalue(L, view, 2);
        c->request->upload_finishing = 1;
    }
    return 0;
}
static rotmos_status call_upload(struct upload_call *c)
{
    rotmos_status status =
        rl_protect(c->rule->server->L, upload_trampoline, c, c->error);
    return status == ROTMOS_OK ? c->status : status;
}
static rotmos_status upload_open(rotmos_request *request, void *data,
                                 void **state, rotmos_error *error)
{
    struct upload_call c;
    memset(&c, 0, sizeof(c));
    c.rule = data;
    c.request = rotmos_request_userdata(request);
    c.request->upload = c.rule;
    c.request->streaming_upload = 1;
    c.error = error;
    *state = c.request;
    return call_upload(&c);
}
static rotmos_status upload_write(rotmos_request *request, const void *bytes,
                                  size_t length, size_t *consumed, void *state,
                                  void *data, rotmos_error *error)
{
    struct upload_call c;
    rotmos_status status;
    memset(&c, 0, sizeof(c));
    c.rule = data;
    c.request = state;
    c.bytes = bytes;
    c.length = length > c.rule->buffer_bytes ? c.rule->buffer_bytes : length;
    c.phase = 1;
    c.error = error;
    status = call_upload(&c);
    if (status == ROTMOS_OK && c.consumed < length)
        status = rotmos_request_body_resume(request, error);
    *consumed = status == ROTMOS_OK ? c.consumed : 0;
    return status;
}
static rotmos_status upload_finish(rotmos_request *request, rotmos_reply *reply,
                                   void *state, void *data, rotmos_error *error)
{
    struct upload_call c;
    rotmos_status status;
    (void)request;
    memset(&c, 0, sizeof(c));
    c.rule = data;
    c.request = state;
    c.phase = 2;
    c.error = error;
    status = call_upload(&c);
    /* The public upload executor requires a selected staged reply before
     * after hooks. The last Lua after hook converts the final declaration. */
    return status == ROTMOS_OK ? rotmos_reply_status(reply, 204, error)
                               : status;
}
rotmos_status rl_upload_close(struct rl_request *request, rotmos_error *error)
{
    struct upload_call c;
    if (!request->upload_opened || request->upload_closed)
        return ROTMOS_OK;
    request->upload_closed = 1;
    memset(&c, 0, sizeof(c));
    c.rule = request->upload;
    c.request = request;
    c.phase = 3;
    c.error = error;
    return call_upload(&c);
}
static void upload_close(rotmos_request *request, rotmos_status outcome,
                         void *state, void *data)
{
    rotmos_error error;
    (void)request;
    (void)outcome;
    (void)data;
    /* Terminal cleanup cannot replace an existing transport/callback error. */
    (void)rl_upload_close(state, &error);
}
void rl_upload_config(rotmos_upload_stream_config *out,
                      const rotmos_route_config *route, struct rl_rule *rule)
{
    rotmos_upload_stream_config_init(out);
    out->method = route->method;
    out->methods = route->methods;
    out->path = route->path;
    out->path_kind = route->path_kind;
    out->body = route->body;
    out->hooks = route->hooks;
    out->open = upload_open;
    out->write = upload_write;
    out->finish = upload_finish;
    out->close = upload_close;
    out->userdata = rule;
}
