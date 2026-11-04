#ifndef ROTMOS_LUA_BINDING_H
#define ROTMOS_LUA_BINDING_H
#include <rotmos/rotmos.h>
#include <rotmos/lua.h>
#include <lua.h>
#include <lauxlib.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RL_SERVER "rotmos.server"
#define RL_REQUEST "rotmos.request"
#define RL_GROUP "rotmos.group"
#define RL_RULE "rotmos.callback"
#define RL_SOURCE "rotmos.source"
#define RL_COUNT "rotmos.count"
#define RL_BODY_RESULT "rotmos.body_result"
#define RL_REPLY "rotmos.reply"
#define RL_LOOP_ITEM "rotmos.loop_item"
#define RL_WEBSOCKET "rotmos.websocket"
struct rl_reply_view {
    rotmos_reply *native;
    rotmos_request *request;
};
struct rl_auth_result {
    const char *action, *location, *type, *challenge, *principal;
    rotmos_bytes body;
    size_t code;
    int present, snapshot;
};
struct rl_body_result {
    rotmos_body_result value;
    unsigned int readers;
    int closed;
};
struct rl_server {
    rotmos_server *native;
    lua_State *L;
    int root;
    rotmos_error callback_error;
    long process_id;
    unsigned long run_epoch;
};
struct rl_rule {
    struct rl_server *server;
    rotmos_route_group *group;
    int root;
    int is_group;
    int is_upload;
    int native_reply;
    size_t buffer_bytes;
};
struct rl_request {
    rotmos_request *native;
    int root;
    int streaming_upload, upload_opened, upload_closed, upload_finishing;
    struct rl_rule *upload;
    int native_reply;
};
struct rl_source {
    rotmos_source *native;
    lua_State *L;
    int root, owned, legacy, resettable;
    int initial_reset;
    rotmos_source *delegate;
    struct rl_request *request_body;
    struct rl_body_result *result_body;
    int delegated;
    lua_CFunction retire;
    size_t offset;
    rotmos_error error;
};
struct rl_call {
    struct rl_server *server;
    struct rl_rule *rule;
    struct rl_request *request;
    rotmos_request *native;
    rotmos_reply *reply;
    const char *name;
    const rotmos_process_context *process;
    int hook, finish;
    rotmos_status status;
    rotmos_error *error;
    struct rl_reply_view *reply_view;
};
rotmos_status rl_fail(rotmos_error *, rotmos_status, const char *);
int rl_error(lua_State *, const rotmos_error *);
void rl_push_error(lua_State *, const rotmos_error *);
rotmos_status rl_protect(lua_State *, lua_CFunction, void *, rotmos_error *);
rotmos_status rl_protect_value(lua_State *, lua_CFunction, void *, int,
                               rotmos_error *);
void rl_table_copy(lua_State *, int);
void rl_push_size(lua_State *, size_t);
void rl_push_u64(lua_State *, rotmos_u64);
int rl_size(lua_State *, int, size_t *);
int rl_u64(lua_State *, int, rotmos_u64 *);
int rl_field_size(lua_State *, int, const char *, size_t *);
int rl_field_string(lua_State *, int, const char *, const char **);
void rl_field(lua_State *, int, const char *);
int rl_fields(lua_State *, int, const char *const *, size_t, rotmos_error *);
int rl_config_parse(lua_State *, int, rotmos_server_config *, rotmos_error *);
void rl_config_push(lua_State *, const rotmos_server_effective *);
int rl_auth_prepare(lua_State *, int, rotmos_error *);
rotmos_status rl_auth_decide(lua_State *, int, rotmos_request *, const char *,
                             struct rl_auth_result *, rotmos_error *);
rotmos_status rl_auth_apply(lua_State *, int, int, rotmos_request *,
                            rotmos_reply *, int *, rotmos_error *);
rotmos_status rl_callback_error(lua_State *, int, rotmos_error *, const char *);
struct rl_server *rl_server(lua_State *, int);
struct rl_request *rl_request(lua_State *, int);
int rl_request_check(lua_State *, int, struct rl_request **, rotmos_error *);
void rl_request_push(lua_State *, struct rl_request *);
rotmos_status rl_request_start(rotmos_server *, rotmos_request *, void *,
                               rotmos_error *);
void rl_request_close(rotmos_server *, rotmos_request *,
                      const rotmos_request_outcome *, void *);
rotmos_status rl_reply(lua_State *, int, rotmos_reply *, rotmos_request *,
                       rotmos_error *);
rotmos_status rl_reply_headers(lua_State *, int, rotmos_reply *,
                               rotmos_error *);
rotmos_status rl_handler(rotmos_request *, rotmos_reply *, void *,
                         rotmos_error *);
rotmos_status rl_before(rotmos_request *, rotmos_reply *, int *, void *,
                        rotmos_error *);
rotmos_status rl_auth(rotmos_request *, rotmos_reply *, int *, void *,
                      rotmos_error *);
rotmos_status rl_after(rotmos_request *, rotmos_reply *, int *, void *,
                       rotmos_error *);
void rl_values_open(lua_State *);
void rl_servers_open(lua_State *);
void rl_requests_open(lua_State *);
void rl_sources_open(lua_State *);
void rl_codecs_open(lua_State *);
int rl_request_json_record(lua_State *);
int rl_request_xml_record(lua_State *);
int rl_codec_route_check(lua_State *, int, int, rotmos_error *);
rotmos_status rl_codec_reply(lua_State *, int, int, int, rotmos_reply *, int,
                             const char *, rotmos_error *);
struct rl_source *rl_source_native_new(lua_State *, int, lua_CFunction,
                                       rotmos_error *);
void rl_bodies_open(lua_State *);
void rl_replies_open(lua_State *);
void rl_reply_view_push(lua_State *, struct rl_call *);
struct rl_rule *rl_rule_new(lua_State *, rotmos_error *);
int rl_rule_root(lua_State *, int, struct rl_rule *);
int rl_rule_finish(lua_State *, int, int, struct rl_rule *, rotmos_status,
                   const rotmos_error *);
void rl_rule_hooks(rotmos_route_hooks *, struct rl_rule *);
int rl_methods(lua_State *, int, rotmos_http_methods, rotmos_http_methods *);
int rl_method_value(lua_State *, int, rotmos_http_method *);
int rl_route_path(lua_State *, int, rotmos_route_config *);
int rl_body_policy(lua_State *, int, rotmos_body_policy *, int, rotmos_error *);
int rl_field_bool(lua_State *, int, const char *, int *);
int rl_static_file(lua_State *);
int rl_static_directory(lua_State *);
int rl_upload_file(lua_State *);
int rl_webdav(lua_State *);
int rl_proxy(lua_State *);
int rl_defer(lua_State *);
int rl_timer(lua_State *);
int rl_watch(lua_State *);
int rl_waker(lua_State *);
int rl_now(lua_State *);
int rl_websocket(lua_State *);
void rl_loops_open(lua_State *);
void rl_loops_process_start(lua_State *, struct rl_server *);
void rl_websockets_open(lua_State *);
int rl_sse_encode(lua_State *);
void rl_body_result_retire(struct rl_body_result *);
int rl_request_body_reader(lua_State *);
int rl_request_body_materialize(lua_State *);
int rl_request_body_copy(lua_State *);
int rl_request_body_spill(lua_State *);
int rl_source_wrap_reader(lua_State *, int, struct rl_request *,
                          struct rl_body_result *, const rotmos_body_result *,
                          rotmos_error *);
void rl_source_expire(struct rl_source *);
void rl_rule_options(lua_State *, struct rl_rule *);
int rl_upload_prepare(lua_State *, int, rotmos_error *);
void rl_upload_config(rotmos_upload_stream_config *,
                      const rotmos_route_config *, struct rl_rule *);
rotmos_status rl_upload_close(struct rl_request *, rotmos_error *);
rotmos_status rl_source_take(lua_State *, int, rotmos_reply *, int,
                             const char *, rotmos_error *);
rotmos_status rl_source_spool(lua_State *, int, rotmos_reply *, int,
                              const char *, const char *, rotmos_error *);
rotmos_status rl_source_create(lua_State *, int, int, rotmos_error *);
void rl_sentinel(lua_State *, int);
int rl_is_sentinel(lua_State *, int, int);
#endif
