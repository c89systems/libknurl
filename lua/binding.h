#ifndef KNURL_LUA_BINDING_H
#define KNURL_LUA_BINDING_H

#include <knurl/knurl.h>
#include <knurl/lonejson.h>
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
struct rl_server;
struct rl_proxy_rewrite;

struct rl_cleanup {
    struct rl_cleanup *next;
    void (*dispose)(lua_State *, struct rl_cleanup *);
};

struct rl_work {
    struct rl_server *server;
    knurl_status (*run)(knurl_task *, struct rl_work *, knurl_error *);
    void (*complete)(struct rl_work *, knurl_status, const knurl_error *);
    knurl_task *task;
    knurl_timer *retry;
    knurl_error retry_error;
    knurl_status retry_status;
    knurl_u64 deadline;
};

knurl_status rl_work_start(struct rl_work *, knurl_error *);
void rl_work_cancel(struct rl_work *);
void rl_cleanup_later(struct rl_server *, struct rl_cleanup *,
                      void (*)(lua_State *, struct rl_cleanup *));
void rl_cleanup_drain(struct rl_server *, int);
int rl_hooks_prepare(lua_State *, struct rl_server *, knurl_async_route_hooks *,
                     knurl_error *);
void rl_hooks_finish(lua_State *, const knurl_async_route_hooks *,
                     knurl_status);
int rl_proxy_hooks_prepare(lua_State *, struct rl_server *,
                           knurl_proxy_config *, knurl_async_route_hooks *,
                           knurl_error *);
knurl_status rl_proxy_rewrite_read(lua_State *, int, struct rl_proxy_rewrite **,
                                   knurl_error *);
knurl_status rl_proxy_rewrite_apply(const knurl_proxy_inbound *,
                                    knurl_proxy_outbound *, void *,
                                    knurl_error *);
void rl_proxy_rewrite_free(void *);

struct rl_server {
    knurl_server *native;
    struct rl_fixed *replies;
    lua_State *worker;
    knurl_blocking_pool_config pool;
    knurl_u64 hard_body_bytes;
    size_t websocket_max_frame_bytes;
    pslog_lua_logger_ref logger, access_logger;
    int running, has_lua;
    knurl_task *websocket_task;
    struct rl_cleanup *cleanup;
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
void rl_push_error(lua_State *, const knurl_error *);
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
int rl_oauth2_option(lua_State *, int, knurl_oauth2_client **, knurl_error *);
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
void rl_websocket_open(lua_State *, int);
void rl_openapi_open(lua_State *);
int rl_schema(lua_State *, int, lonejson_schema_view *, knurl_error *);
int rl_typed_options(lua_State *, int, int, knurl_xml_config *, knurl_error *);
knurl_status rl_typed_input(lua_State *, int, int, const knurl_xml_config *,
                            knurl_source *, knurl_error *);
int rl_json_record_reply(lua_State *, int, knurl_reply *, int, const char *,
                         knurl_status *, knurl_error *);
knurl_status rl_xml_reply(lua_State *, int, knurl_reply *, int, const char *,
                          knurl_error *);
knurl_status rl_lua_failure(lua_State *, int, const char *, knurl_error *);
void rl_push_request(lua_State *, const knurl_request_snapshot *, int);
knurl_status rl_stream_reply(lua_State *, int, struct rl_server *,
                             knurl_reply *, int, const char *, int,
                             knurl_error *);
knurl_status rl_server_run(lua_State *, struct rl_server *, knurl_error *);
knurl_status rl_json_reply(lua_State *, int, knurl_reply *, int, const char *,
                           struct rl_json **, knurl_error *);
void rl_json_free(struct rl_json *);
int rl_curl_core(lua_State *);
KNURL_API int luaopen_knurl(lua_State *);

#endif
