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
struct rl_server {
    rotmos_server *native;
    lua_State *L;
    int root;
    rotmos_listener_config *pending_listeners;
    rotmos_error callback_error;
};
struct rl_rule {
    struct rl_server *server;
    rotmos_route_group *group;
    int root;
    int is_group;
};
struct rl_request {
    rotmos_request *native;
    int root;
};
struct rl_source {
    rotmos_source *native;
    lua_State *L;
    int root, owned, legacy, started;
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
rotmos_status rl_callback_error(lua_State *, int, rotmos_error *, const char *);
struct rl_server *rl_server(lua_State *, int);
struct rl_request *rl_request(lua_State *, int);
void rl_request_push(lua_State *, struct rl_request *);
rotmos_status rl_request_start(rotmos_server *, rotmos_request *, void *,
                               rotmos_error *);
void rl_request_close(rotmos_server *, rotmos_request *,
                      const rotmos_request_outcome *, void *);
rotmos_status rl_reply(lua_State *, int, rotmos_reply *, rotmos_error *);
rotmos_status rl_handler(rotmos_request *, rotmos_reply *, void *,
                         rotmos_error *);
rotmos_status rl_before(rotmos_request *, rotmos_reply *, int *, void *,
                        rotmos_error *);
rotmos_status rl_after(rotmos_request *, rotmos_reply *, int *, void *,
                       rotmos_error *);
void rl_values_open(lua_State *);
void rl_servers_open(lua_State *);
void rl_requests_open(lua_State *);
void rl_sources_open(lua_State *);
rotmos_status rl_source_take(lua_State *, int, rotmos_reply *, int,
                             const char *, rotmos_error *);
rotmos_status rl_source_create(lua_State *, int, int, rotmos_error *);
void rl_sentinel(lua_State *, int);
int rl_is_sentinel(lua_State *, int, int);
#endif
