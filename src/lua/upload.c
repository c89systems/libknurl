#include "binding.h"
struct upload_call {
    struct rl_rule *rule;
    struct rl_request *request;
    const void *bytes;
    size_t length, consumed;
    int phase;
    knurl_status status;
    knurl_error *error;
};
int rl_upload_prepare(lua_State *L, int index, knurl_error *error)
{
    static const char *const fields[] = {"open", "on_chunk", "on_complete",
                                         "close"};
    size_t i;
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        rl_field(L, index, fields[i]);
        if (!lua_isfunction(L, -1) && (i == 1 || !lua_isnil(L, -1))) {
            rl_fail(error, KNURL_INVALID,
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
            rl_is_sentinel(L, -1, KNURL_AGAIN)) {
            c->status = KNURL_AGAIN;
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
            c->status = rl_fail(c->error, KNURL_INVALID,
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
static knurl_status call_upload(struct upload_call *c)
{
    knurl_status status =
        rl_protect(c->rule->server->L, upload_trampoline, c, c->error);
    return status == KNURL_OK ? c->status : status;
}
static knurl_status upload_open(knurl_request *request, void *data,
                                void **state, knurl_error *error)
{
    struct upload_call c;
    memset(&c, 0, sizeof(c));
    c.rule = data;
    c.request = knurl_request_userdata(request);
    c.request->upload = c.rule;
    c.request->streaming_upload = 1;
    c.error = error;
    *state = c.request;
    return call_upload(&c);
}
static knurl_status upload_write(knurl_request *request, const void *bytes,
                                 size_t length, size_t *consumed, void *state,
                                 void *data, knurl_error *error)
{
    struct upload_call c;
    knurl_status status;
    memset(&c, 0, sizeof(c));
    c.rule = data;
    c.request = state;
    c.bytes = bytes;
    c.length = length > c.rule->buffer_bytes ? c.rule->buffer_bytes : length;
    c.phase = 1;
    c.error = error;
    status = call_upload(&c);
    if (status == KNURL_OK && c.consumed < length)
        status = knurl_request_body_resume(request, error);
    *consumed = status == KNURL_OK ? c.consumed : 0;
    return status;
}
static knurl_status upload_finish(knurl_request *request, knurl_reply *reply,
                                  void *state, void *data, knurl_error *error)
{
    struct upload_call c;
    knurl_status status;
    (void)request;
    memset(&c, 0, sizeof(c));
    c.rule = data;
    c.request = state;
    c.phase = 2;
    c.error = error;
    status = call_upload(&c);
    /* The public upload executor requires a selected staged reply before
     * after hooks. The last Lua after hook converts the final declaration. */
    return status == KNURL_OK ? knurl_reply_status(reply, 204, error) : status;
}
knurl_status rl_upload_close(struct rl_request *request, knurl_error *error)
{
    struct upload_call c;
    if (!request->upload_opened || request->upload_closed)
        return KNURL_OK;
    request->upload_closed = 1;
    memset(&c, 0, sizeof(c));
    c.rule = request->upload;
    c.request = request;
    c.phase = 3;
    c.error = error;
    return call_upload(&c);
}
static void upload_close(knurl_request *request, knurl_status outcome,
                         void *state, void *data)
{
    knurl_error error;
    (void)request;
    (void)outcome;
    (void)data;
    /* Terminal cleanup cannot replace an existing transport/callback error. */
    (void)rl_upload_close(state, &error);
}
void rl_upload_config(knurl_upload_stream_config *out,
                      const knurl_route_config *route, struct rl_rule *rule)
{
    knurl_upload_stream_config_init(out);
    out->method = route->method;
    out->methods = route->methods;
    out->path = route->path;
    out->path_kind = route->path_kind;
    out->body = route->body;
    out->hooks = route->hooks;
    out->acl = route->acl;
    out->open = upload_open;
    out->write = upload_write;
    out->finish = upload_finish;
    out->close = upload_close;
    out->userdata = rule;
}
