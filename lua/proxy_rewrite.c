#include "binding.h"

#define REWRITE_TEXT_LIMIT 65536

struct rl_proxy_rewrite {
    knurl_http_method method;
    size_t target;
    int has_target, has_host, has_query;
    char *path, *query, *host;
    knurl_reply *headers;
    knurl_reply *remove;
};

void rl_proxy_rewrite_free(void *data)
{
    struct rl_proxy_rewrite *edit = data;
    if (edit != NULL) {
        free(edit->path);
        free(edit->query);
        free(edit->host);
        (void)knurl_reply_destroy(edit->headers, NULL);
        (void)knurl_reply_destroy(edit->remove, NULL);
        free(edit);
    }
}

static knurl_status copy(lua_State *L, int index, const char *field, char **out,
                         int *present, knurl_error *error)
{
    const char *value;
    size_t length;
    rl_field(L, index, field);
    *present = !lua_isnil(L, -1);
    if (*present && !(lua_isboolean(L, -1) && !lua_toboolean(L, -1))) {
        if (lua_type(L, -1) != LUA_TSTRING)
            return rl_fail(error, KNURL_INVALID,
                           "proxy path, query and host must be strings");
        value = lua_tolstring(L, -1, &length);
        if (length > REWRITE_TEXT_LIMIT || memchr(value, 0, length))
            return rl_fail(
                error, KNURL_INVALID,
                "proxy rewrite string exceeds 64 KiB or contains NUL");
        *out = malloc(length + 1);
        if (*out == NULL)
            return rl_fail(error, KNURL_NOMEM, "copying proxy rewrite");
        memcpy(*out, value, length + 1);
    }
    lua_pop(L, 1);
    return KNURL_OK;
}

knurl_status rl_proxy_rewrite_read(lua_State *L, int index,
                                   struct rl_proxy_rewrite **out,
                                   knurl_error *error)
{
    static const char *const fields[] = {"path",   "query",  "host",
                                         "method", "target", "headers"};
    struct rl_proxy_rewrite *edit;
    knurl_http_methods methods;
    knurl_status status;
    const char *name, *value;
    size_t n, v, budget = 16384;
    int path_present;
    index = lua_absindex(L, index);
    if (lua_isnil(L, index))
        return KNURL_OK;
    if (!rl_fields(L, index, fields, RL_COUNT(fields), error))
        return error->status;
    edit = calloc(1, sizeof(*edit));
    if (edit == NULL)
        return rl_fail(error, KNURL_NOMEM, "allocating proxy rewrite");
    *out = edit;
    if (!rl_methods(L, index, 0, &methods))
        return rl_fail(error, KNURL_INVALID, "invalid proxy rewrite method");
    if (methods != 0) {
        for (edit->method = KNURL_HTTP_GET; edit->method <= KNURL_HTTP_MOVE;
             ++edit->method)
            if (methods == KNURL_HTTP_METHOD_MASK(edit->method))
                break;
        if (edit->method > KNURL_HTTP_MOVE)
            return rl_fail(error, KNURL_INVALID,
                           "proxy rewrite requires one method");
    }
    rl_field(L, index, "target");
    edit->has_target = !lua_isnil(L, -1);
    if (edit->has_target &&
        (!rl_size(L, -1, &edit->target) || edit->target == 0))
        return rl_fail(error, KNURL_INVALID,
                       "proxy target must be a one-based index");
    lua_pop(L, 1);

    status = copy(L, index, "path", &edit->path, &path_present, error);
    if (status == KNURL_OK && path_present && edit->path == NULL)
        status = rl_fail(error, KNURL_INVALID, "proxy path requires a string");
    if (status == KNURL_OK)
        status = copy(L, index, "query", &edit->query, &edit->has_query, error);
    if (status == KNURL_OK)
        status = copy(L, index, "host", &edit->host, &edit->has_host, error);
    if (status != KNURL_OK)
        return status;

    rl_field(L, index, "headers");
    if (lua_isnil(L, -1))
        return KNURL_OK;
    if (!lua_istable(L, -1))
        return rl_fail(error, KNURL_INVALID,
                       "proxy rewrite headers must be a table");
    status = knurl_reply_new(&edit->headers, error);
    if (status == KNURL_OK)
        status = knurl_reply_new(&edit->remove, error);
    if (status != KNURL_OK)
        return status;
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            return rl_fail(error, KNURL_INVALID,
                           "proxy rewrite header names must be strings");
        name = lua_tolstring(L, -2, &n);
        if (n > budget || memchr(name, 0, n))
            return rl_fail(error, KNURL_INVALID,
                           "invalid or oversized proxy rewrite header name");
        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
            status = knurl_reply_header_add(edit->remove, name, "", error);
            v = 0;
        } else {
            if (lua_type(L, -1) != LUA_TSTRING)
                return rl_fail(
                    error, KNURL_INVALID,
                    "proxy rewrite header values must be strings or false");
            value = lua_tolstring(L, -1, &v);
            if (v > budget - n || memchr(value, 0, v))
                return rl_fail(
                    error, KNURL_INVALID,
                    "invalid or oversized proxy rewrite header value");
            status = knurl_reply_header_set(edit->headers, name, value, error);
        }
        if (status != KNURL_OK)
            return status;
        if (budget - n - v < 4)
            return rl_fail(error, KNURL_LIMIT,
                           "proxy rewrite header budget exceeded");
        budget -= n + v + 4;
        lua_pop(L, 1);
    }
    return KNURL_OK;
}

knurl_status rl_proxy_rewrite_apply(const knurl_proxy_inbound *inbound,
                                    knurl_proxy_outbound *outbound, void *data,
                                    knurl_error *error)
{
    struct rl_proxy_rewrite *edit = data;
    knurl_status status = KNURL_OK;
    knurl_header header;
    size_t i;
    (void)inbound;
    if (edit->has_target)
        status = knurl_proxy_outbound_select_target(outbound, edit->target - 1,
                                                    error);
    if (status == KNURL_OK && edit->method != KNURL_HTTP_ANY)
        status = knurl_proxy_outbound_set_method(outbound, edit->method, error);
    if (status == KNURL_OK && edit->path != NULL)
        status = knurl_proxy_outbound_set_path(outbound, edit->path, error);
    if (status == KNURL_OK && edit->has_query)
        status = knurl_proxy_outbound_set_query(outbound, edit->query, error);
    if (status == KNURL_OK && edit->has_host)
        status = knurl_proxy_outbound_set_host(outbound, edit->host, error);
    for (i = 0;
         status == KNURL_OK && i < knurl_reply_header_count(edit->remove);
         ++i) {
        (void)knurl_reply_header_at(edit->remove, i, &header, NULL);
        status = knurl_proxy_outbound_remove_header(outbound, header.name.data,
                                                    error);
    }
    for (i = 0;
         status == KNURL_OK && i < knurl_reply_header_count(edit->headers);
         ++i) {
        (void)knurl_reply_header_at(edit->headers, i, &header, NULL);
        status = knurl_proxy_outbound_set_header(outbound, header.name.data,
                                                 header.value.data, error);
    }
    return status;
}
