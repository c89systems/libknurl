#ifndef KNURL_LUA_BINDING_H
#define KNURL_LUA_BINDING_H
#include <knurl/knurl.h>
#include <knurl/lua.h>
#include <lua.h>
#include <lauxlib.h>
#include <pslog_lua.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RL_SERVER "knurl.server"
#define RL_REQUEST "knurl.request"
#define RL_GROUP "knurl.group"
#define RL_RULE "knurl.callback"
#define RL_SOURCE "knurl.source"
#define RL_COUNT "knurl.count"
#define RL_BODY_RESULT "knurl.body_result"
#define RL_REPLY "knurl.reply"
#define RL_LOOP_ITEM "knurl.loop_item"
#define RL_WEBSOCKET "knurl.websocket"
struct rl_reply_view {
    knurl_reply *native;
    knurl_request *request;
};
struct rl_auth_result {
    const char *action, *location, *type, *challenge, *principal;
    knurl_bytes body;
    size_t code;
    int present, snapshot;
};
struct rl_body_result {
    knurl_body_result value;
    unsigned int readers;
    int closed;
};
struct rl_server {
    knurl_server *native;
    lua_State *L;
    int root;
    knurl_error callback_error;
    long process_id;
    unsigned long run_epoch;
    pslog_lua_logger_ref logger, access_logger;
};
struct rl_rule {
    struct rl_server *server;
    knurl_route_group *group;
    int root;
    int is_group;
    int is_upload;
    int native_reply;
    size_t buffer_bytes;
    int codec_kind;
};
struct rl_request {
    knurl_request *native;
    int root;
    int streaming_upload, upload_opened, upload_closed, upload_finishing;
    struct rl_rule *upload;
    int native_reply;
    int typed_records[2];
};
struct rl_source {
    knurl_source *native;
    lua_State *L;
    int root, owned, legacy, resettable;
    knurl_source *delegate;
    struct rl_request *request_body;
    struct rl_body_result *result_body;
    int delegated;
    lua_CFunction retire;
    size_t offset;
    knurl_error error;
    int active;
    struct rl_server *material_server;
};
struct rl_call {
    struct rl_server *server;
    struct rl_rule *rule;
    struct rl_request *request;
    knurl_request *native;
    knurl_reply *reply;
    const char *name;
    const knurl_process_context *process;
    int hook, finish;
    knurl_status status;
    knurl_error *error;
    struct rl_reply_view *reply_view;
};
knurl_status rl_fail(knurl_error *, knurl_status, const char *);
int rl_error(lua_State *, const knurl_error *);
void rl_push_error(lua_State *, const knurl_error *);
knurl_status rl_protect(lua_State *, lua_CFunction, void *, knurl_error *);
knurl_status rl_protect_value(lua_State *, lua_CFunction, void *, int,
                              knurl_error *);
void rl_table_copy(lua_State *, int);
void rl_push_size(lua_State *, size_t);
void rl_push_u64(lua_State *, knurl_u64);
int rl_integer(lua_State *, int, lua_Integer *);
int rl_size(lua_State *, int, size_t *);
int rl_u64(lua_State *, int, knurl_u64 *);
int rl_field_size(lua_State *, int, const char *, size_t *);
int rl_field_string(lua_State *, int, const char *, const char **);
void rl_field(lua_State *, int, const char *);
int rl_fields(lua_State *, int, const char *const *, size_t, knurl_error *);
int rl_mime_types(lua_State *, int, const knurl_mime_type **, size_t *,
                  knurl_error *);
void rl_mime_push(lua_State *, const knurl_mime_type *, size_t);
int rl_config_parse(lua_State *, int, int, knurl_server_config *,
                    knurl_error *);
int rl_material_config(lua_State *, int, int, knurl_tls_material *,
                       knurl_error *);
int rl_acme_config(lua_State *, int, int, knurl_acme_config *, knurl_error *);
void rl_config_push(lua_State *, const knurl_server_effective *);
int rl_auth_prepare(lua_State *, int, knurl_error *);
knurl_status rl_auth_decide(lua_State *, int, knurl_request *, const char *,
                            struct rl_auth_result *, knurl_error *);
knurl_status rl_auth_apply(lua_State *, int, int, knurl_request *,
                           knurl_reply *, int *, knurl_error *);
knurl_status rl_callback_error(lua_State *, int, knurl_error *, const char *);
struct rl_server *rl_server(lua_State *, int);
struct rl_request *rl_request(lua_State *, int);
int rl_request_check(lua_State *, int, struct rl_request **, knurl_error *);
void rl_request_push(lua_State *, struct rl_request *);
knurl_status rl_request_start(knurl_server *, knurl_request *, void *,
                              knurl_error *);
knurl_status rl_request_ensure(struct rl_server *, knurl_request *,
                               knurl_error *);
void rl_request_close(knurl_server *, knurl_request *,
                      const knurl_request_outcome *, void *);
knurl_status rl_reply(lua_State *, int, knurl_reply *, knurl_request *,
                      knurl_error *);
knurl_status rl_reply_headers(lua_State *, int, knurl_reply *, knurl_error *);
knurl_status rl_handler(knurl_request *, knurl_reply *, void *, knurl_error *);
knurl_status rl_before(knurl_request *, knurl_reply *, int *, void *,
                       knurl_error *);
knurl_status rl_auth(knurl_request *, knurl_reply *, int *, void *,
                     knurl_error *);
knurl_status rl_after(knurl_request *, knurl_reply *, int *, void *,
                      knurl_error *);
void rl_values_open(lua_State *);
void rl_servers_open(lua_State *);
void rl_logging_init(struct rl_server *);
void rl_logging_cleanup(struct rl_server *);
void rl_logging_open(lua_State *);
int rl_logging_config(lua_State *, int, struct rl_server *, knurl_error *);
int rl_set_pslog(lua_State *);
void rl_requests_open(lua_State *);
void rl_sources_open(lua_State *);
void rl_codecs_open(lua_State *);
void rl_xml_objects_open(lua_State *);
void rl_rest_open(lua_State *);
int rl_rest_route(lua_State *);
knurl_status rl_rest_validate(knurl_request *, const void *, knurl_reply *,
                              int *, void *, knurl_error *);
int rl_policy_config(lua_State *, int, knurl_server_config *,
                     struct rl_server *, knurl_error *);
knurl_status rl_policy_start(struct rl_server *, knurl_request *,
                             knurl_error *);
void rl_policy_close(struct rl_server *, knurl_request *,
                     const knurl_request_outcome *);
void rl_process_push(lua_State *, const knurl_process_context *);
void rl_worker_event_push(lua_State *, const knurl_worker_event *);
int rl_control_interest(lua_State *);
int rl_supervision_take(lua_State *);
int rl_tls_identity_state(lua_State *);
void rl_policy_open(lua_State *);
void rl_openapi_open(lua_State *);
int rl_openapi_doc(lua_State *);
int rl_openapi(lua_State *);
int rl_request_json_record(lua_State *);
int rl_request_json_array_each(lua_State *);
int rl_request_xml_record(lua_State *);
knurl_status rl_codec_route_register(lua_State *, struct rl_rule *,
                                     struct rl_rule *,
                                     const knurl_route_config *, int,
                                     knurl_error *);
knurl_status rl_codec_reply(lua_State *, int, int, int, knurl_reply *, int,
                            const char *, knurl_error *);
struct rl_source *rl_source_native_new(lua_State *, int, lua_CFunction,
                                       knurl_error *);
void rl_bodies_open(lua_State *);
void rl_replies_open(lua_State *);
void rl_reply_view_push(lua_State *, struct rl_call *);
struct rl_rule *rl_rule_new(lua_State *, knurl_error *);
int rl_rule_root(lua_State *, int, struct rl_rule *);
int rl_rule_finish(lua_State *, int, int, struct rl_rule *, knurl_status,
                   const knurl_error *);
void rl_rule_hooks(knurl_route_hooks *, struct rl_rule *);
int rl_methods(lua_State *, int, knurl_http_methods, knurl_http_methods *);
int rl_method_value(lua_State *, int, knurl_http_method *);
int rl_route_path(lua_State *, int, knurl_route_config *);
int rl_body_policy(lua_State *, int, knurl_body_policy *, int, knurl_error *);
int rl_field_bool(lua_State *, int, const char *, int *);
int rl_static_file(lua_State *);
int rl_static_directory(lua_State *);
int rl_static_assets(lua_State *);
knurl_asset_provider *rl_asset_provider(lua_State *, int);
knurl_storage_provider *rl_storage_provider(lua_State *, int);
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
                          struct rl_body_result *, const knurl_body_result *,
                          knurl_error *);
void rl_source_expire(struct rl_source *);
void rl_rule_options(lua_State *, struct rl_rule *);
int rl_upload_prepare(lua_State *, int, knurl_error *);
void rl_upload_config(knurl_upload_stream_config *, const knurl_route_config *,
                      struct rl_rule *);
knurl_status rl_upload_close(struct rl_request *, knurl_error *);
knurl_status rl_source_take(lua_State *, int, knurl_reply *, int, const char *,
                            knurl_error *);
knurl_status rl_source_spool(lua_State *, int, knurl_reply *, int, const char *,
                             const char *, knurl_error *);
knurl_status rl_source_create(lua_State *, int, int, knurl_error *);
void rl_sentinel(lua_State *, int);
int rl_is_sentinel(lua_State *, int, int);
#endif
