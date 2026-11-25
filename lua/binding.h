#ifndef KNURL_LUA_BINDING_H
#define KNURL_LUA_BINDING_H

#include <knurl/knurl.h>
#include <lua.h>
#include <lauxlib.h>
#include <pslog_lua.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RL_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define RL_SERVER "knurl.server"
#define RL_GROUP "knurl.group"
#define RL_ROUTE "knurl.route"

struct rl_fixed;
struct rl_json;

struct rl_server {
    knurl_server *native;
    struct rl_fixed *replies;
    lua_State *worker;
    knurl_blocking_pool_config pool;
    knurl_u64 hard_body_bytes;
    pslog_lua_logger_ref logger, access_logger;
    int running, has_lua;
};

struct rl_group {
    struct rl_server *server;
    knurl_route_group *native;
};

struct rl_route {
    struct rl_server *server;
    knurl_route *native;
};

knurl_status rl_fail(knurl_error *, knurl_status, const char *);
int rl_error(lua_State *, const knurl_error *);
int rl_result(lua_State *, knurl_status, const knurl_error *);
void rl_field(lua_State *, int, const char *);
int rl_fields(lua_State *, int, const char *const *, size_t, knurl_error *);
int rl_size(lua_State *, int, size_t *);
int rl_field_size(lua_State *, int, const char *, size_t *);
int rl_field_string(lua_State *, int, const char *, const char **);
int rl_field_bool(lua_State *, int, const char *, int *);
int rl_string_array(lua_State *, int, const char *, const char *const **,
                    size_t *, knurl_error *);
int rl_methods(lua_State *, int, knurl_http_methods, knurl_http_methods *);
int rl_route_path(lua_State *, int, knurl_route_config *);
int rl_mime_types(lua_State *, int, const knurl_mime_type **, size_t *,
                  knurl_error *);
int rl_body_limit(lua_State *, int, const struct rl_server *, knurl_u64 *,
                  knurl_error *);

int rl_acl_option(lua_State *, int, knurl_acl **, knurl_error *);
int rl_set_acl(lua_State *);
void rl_acl_open(lua_State *);
void rl_geoip_open(lua_State *);
int rl_auth_option(lua_State *, int, knurl_auth **, knurl_error *);
int rl_set_auth(lua_State *);
void rl_auth_open(lua_State *);
void rl_auth_endpoints(lua_State *);
void rl_server_open(lua_State *);
void rl_logging_open(lua_State *);
void rl_logging_init(struct rl_server *);
void rl_logging_cleanup(struct rl_server *);
int rl_logging_config(lua_State *, int, struct rl_server *, knurl_error *);
int rl_set_pslog(lua_State *);
void rl_routes_open(lua_State *, int);
struct rl_route *rl_route_new(lua_State *, struct rl_server *);
void rl_fixed_free(struct rl_fixed *);
void rl_application_open(lua_State *, int);
knurl_status rl_server_run(lua_State *, struct rl_server *, knurl_error *);
knurl_status rl_json_reply(lua_State *, int, knurl_reply *, int, const char *,
                           struct rl_json **, knurl_error *);
void rl_json_free(struct rl_json *);
int rl_curl_core(lua_State *);
KNURL_API int luaopen_knurl(lua_State *);

#endif
