/* Public adapters for the policy and supervisor hooks extracted from Vectis. */
#include "binding.h"

enum operation {
    ADDRESS,
    CORRELATION,
    CONNECTION,
    ADMISSION,
    START,
    CLOSE,
    OBSERVE,
    WORKER,
    IDENTITY,
    CONTROL
};
struct policy_call {
    struct rl_server *server;
    enum operation operation;
    knurl_request *request;
    knurl_reply *reply;
    const void *event;
    knurl_admission_action *action;
    char *buffer;
    size_t capacity, *length;
    unsigned int events;
    knurl_error *error;
    knurl_status status;
};
static void integer(lua_State *L, const char *key, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
}
static void boolean(lua_State *L, const char *key, int value)
{
    lua_pushboolean(L, value);
    lua_setfield(L, -2, key);
}
static void bytes(lua_State *L, const char *key, knurl_bytes value)
{
    if (value.data == NULL)
        lua_pushnil(L);
    else
        lua_pushlstring(L, value.data, value.length);
    lua_setfield(L, -2, key);
}
static void wide(lua_State *L, const char *key, knurl_u64 value)
{
    rl_push_u64(L, value);
    lua_setfield(L, -2, key);
}
static void endpoint(lua_State *L, knurl_endpoint value)
{
    lua_createtable(L, 0, 2);
    bytes(L, "address", value.address);
    integer(L, "port", value.port);
}
static void connection(lua_State *L, const knurl_connection_info *value)
{
    lua_createtable(L, 0, 5);
    endpoint(L, value->peer);
    lua_setfield(L, -2, "peer");
    endpoint(L, value->local);
    lua_setfield(L, -2, "local_endpoint");
    rl_push_size(L, value->listener_index + 1);
    lua_setfield(L, -2, "listener_index");
    boolean(L, "tls", value->tls);
    rl_process_push(L, &value->process);
    lua_setfield(L, -2, "process");
}
static void outcome(lua_State *L, const knurl_request_outcome *value)
{
    lua_createtable(L, 0, 7);
    integer(L, "cause", value->cause);
    integer(L, "body_cause", value->body_cause);
    boolean(L, "body_complete", value->body_complete);
    boolean(L, "upgraded", value->upgraded);
    wide(L, "received_body_bytes", value->received_body_bytes);
    wide(L, "delivered_body_bytes", value->delivered_body_bytes);
    lua_createtable(L, 0, 7);
    integer(L, "cause", value->response.cause);
    integer(L, "status", value->response.status);
    boolean(L, "committed", value->response.committed);
    wide(L, "wire_bytes", value->response.wire_bytes);
    wide(L, "interim_wire_bytes", value->response.interim_wire_bytes);
    wide(L, "body_bytes", value->response.body_bytes);
    wide(L, "duration_ms", value->response.duration_ms);
    lua_setfield(L, -2, "response");
}
void rl_worker_event_push(lua_State *L, const knurl_worker_event *event)
{
    static const char *const names[] = {"started", "exited", "restarted",
                                        "escalated"};
    lua_createtable(L, 0, 7);
    lua_pushstring(L, names[event->kind]);
    lua_setfield(L, -2, "kind");
    rl_process_push(L, &event->process);
    lua_setfield(L, -2, "process");
    integer(L, "exit_status", event->exit_status);
    integer(L, "signal_number", event->signal_number);
    boolean(L, "abandoned", event->abandoned);
    boolean(L, "expected", event->expected);
    rl_push_error(L, &event->error);
    lua_setfield(L, -2, "error");
}
static int callback(lua_State *L, struct rl_server *s, const char *section,
                    const char *name)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->root);
    lua_getiuservalue(L, -1, 1);
    rl_field(L, -1, section);
    if (!lua_istable(L, -1))
        return 0;
    rl_field(L, -1, name);
    return lua_isfunction(L, -1);
}
static int push_request(lua_State *L, struct policy_call *call)
{
    call->status = rl_request_ensure(call->server, call->request, call->error);
    if (call->status != KNURL_OK)
        return 0;
    rl_request_push(L, knurl_request_userdata(call->request));
    return 1;
}
static void observation(lua_State *L, struct policy_call *call)
{
    static const char *const names[] = {
        "connection_accepted", "tcp_stall",        "tls_failure",
        "http_status",         "request_started",  "request_rejected",
        "response_committed",  "request_complete", "application"};
    static const char *const sides[] = {"unknown", "downstream", "upstream",
                                        "host"};
    const knurl_observation *event = call->event;
    lua_createtable(L, 0, 13);
    lua_pushstring(L, names[event->kind]);
    lua_setfield(L, -2, "kind");
    connection(L, &event->connection);
    lua_setfield(L, -2, "connection");
    bytes(L, "effective_address", event->effective_address);
    bytes(L, "correlation_id", event->correlation_id);
    bytes(L, "name", event->name);
    integer(L, "status", event->status);
    integer(L, "cause", event->cause);
    boolean(L, "committed", event->committed);
    boolean(L, "has_outcome", event->has_outcome);
    boolean(L, "input_counts_known", event->input_counts_known);
    lua_pushstring(L, sides[event->disconnect_side]);
    lua_setfield(L, -2, "disconnect_side");
    if (event->has_outcome) {
        outcome(L, &event->outcome);
        lua_setfield(L, -2, "outcome");
    }
    if (event->request != NULL) {
        call->request = event->request;
        if (push_request(L, call))
            lua_setfield(L, -2, "request");
    }
}
static int decide(lua_State *L, struct policy_call *call, int value)
{
    const char *name;
    int top = lua_gettop(L);
    value = lua_absindex(L, value);
    if (lua_istable(L, value))
        lua_getfield(L, value, "action");
    else
        lua_pushvalue(L, value);
    if (lua_isnil(L, -1) || (lua_isboolean(L, -1) && lua_toboolean(L, -1)))
        *call->action = KNURL_ADMISSION_ALLOW;
    else if (lua_isboolean(L, -1))
        *call->action = KNURL_ADMISSION_REJECT;
    else {
        name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        if (!strcmp(name, "allow"))
            *call->action = KNURL_ADMISSION_ALLOW;
        else if (!strcmp(name, "reject"))
            *call->action = KNURL_ADMISSION_REJECT;
        else if (!strcmp(name, "close"))
            *call->action = KNURL_ADMISSION_CLOSE;
        else {
            rl_fail(call->error, KNURL_INVALID, "unknown admission action");
            lua_settop(L, top);
            return 0;
        }
    }
    lua_settop(L, top);
    if (call->operation == ADMISSION &&
        *call->action == KNURL_ADMISSION_REJECT && lua_istable(L, value)) {
        static const char *const fields[] = {"action",       "status",
                                             "status_code",  "headers",
                                             "content_type", "body"};
        if (!rl_fields(L, value, fields, sizeof(fields) / sizeof(fields[0]),
                       call->error))
            return 0;
        /* An action alone keeps the public admission helper's default 403. */
        lua_pushnil(L);
        while (lua_next(L, value)) {
            name = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
            lua_pop(L, 1);
            if (strcmp(name, "action")) {
                lua_pop(L, 1);
                return rl_reply(L, value, call->reply, call->request,
                                call->error) == KNURL_OK;
            }
        }
    }
    return 1;
}
static int invoke(lua_State *L)
{
    struct policy_call *call = lua_touserdata(L, 1);
    const char *section, *name;
    int count = 1;
    size_t length;
    const char *text;
    switch (call->operation) {
    case ADDRESS:
        section = "client_ip";
        name = "resolve";
        break;
    case CORRELATION:
        section = "correlation_id";
        name = "resolve";
        break;
    case CONNECTION:
        section = "admission";
        name = "on_connection";
        break;
    case ADMISSION:
        section = "admission";
        name = "on_request";
        break;
    case CONTROL:
        section = "supervision";
        name = "on_event";
        break;
    default:
        section = "callbacks";
        name = call->operation == START     ? "on_request_start"
               : call->operation == CLOSE   ? "on_request_close"
               : call->operation == OBSERVE ? "on_observe"
               : call->operation == WORKER  ? "on_worker_event"
                                            : "on_tls_identity";
    }
    if (!callback(L, call->server, section, name))
        return 0;
    if (call->operation >= START) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, call->server->root);
        count++;
    }
    switch (call->operation) {
    case CONNECTION:
        connection(L, call->event);
        break;
    case OBSERVE:
        observation(L, call);
        break;
    case WORKER:
        rl_worker_event_push(L, call->event);
        break;
    case IDENTITY: {
        const knurl_tls_identity_event *event = call->event;
        lua_createtable(L, 0, 4);
        rl_push_size(L, event->identity_index + 1);
        lua_setfield(L, -2, "identity_index");
        lua_pushstring(L,
                       event->state == KNURL_TLS_USABLE ? "usable" : "pending");
        lua_setfield(L, -2, "state");
        bytes(L, "domain", event->domain);
        rl_push_error(L, &event->error);
        lua_setfield(L, -2, "error");
        break;
    }
    case CONTROL:
        lua_pushinteger(L, call->events);
        break;
    default:
        if (!push_request(L, call))
            return 0;
        if (call->operation == CLOSE) {
            outcome(L, call->event);
            count++;
        }
    }
    if (call->status != KNURL_OK)
        return 0;
    if (call->operation == CLOSE || call->operation == OBSERVE ||
        call->operation == WORKER || call->operation == IDENTITY) {
        lua_call(L, count, 0);
        return 0;
    }
    lua_call(L, count, 2);
    if (!lua_isnil(L, -1)) {
        call->status = rl_callback_error(L, -1, call->error,
                                         "policy callback returned an error");
        return 0;
    }
    if (call->operation == ADDRESS || call->operation == CORRELATION) {
        if (lua_isnil(L, -2)) {
            knurl_bytes current =
                knurl_request_effective_address(call->request);
            text = call->operation == ADDRESS ? current.data : "";
            length = call->operation == ADDRESS ? current.length : 0;
        } else if (lua_type(L, -2) == LUA_TSTRING)
            text = lua_tolstring(L, -2, &length);
        else {
            call->status =
                rl_fail(call->error, KNURL_INVALID,
                        "resolver must return a string or nil/error");
            return 0;
        }
        if (length > call->capacity)
            call->status = rl_fail(call->error, KNURL_LIMIT,
                                   "resolver result exceeds its public bound");
        else {
            if (length)
                memcpy(call->buffer, text, length);
            *call->length = length;
        }
    } else if (call->operation == CONNECTION || call->operation == ADMISSION) {
        if (!decide(L, call, -2))
            call->status = call->error->status;
    } else if (lua_isboolean(L, -2) && !lua_toboolean(L, -2))
        call->status = rl_fail(call->error, KNURL_STATE,
                               "policy callback rejected operation");
    return 0;
}
static knurl_status call_policy(struct policy_call *call)
{
    knurl_status status =
        rl_protect(call->server->L, invoke, call, call->error);
    return status == KNURL_OK ? call->status : status;
}
static void notify(struct rl_server *s, enum operation operation,
                   knurl_request *request, const void *event)
{
    struct policy_call call;
    knurl_error error;
    memset(&call, 0, sizeof(call));
    call.server = s;
    call.operation = operation;
    call.request = request;
    call.event = event;
    call.error = &error;
    if (call_policy(&call) != KNURL_OK) {
        if (s->callback_error.status == KNURL_OK)
            s->callback_error = error;
        (void)knurl_server_request_stop(s->native, NULL);
    }
}
knurl_status rl_policy_start(struct rl_server *s, knurl_request *request,
                             knurl_error *error)
{
    struct policy_call call;
    memset(&call, 0, sizeof(call));
    call.server = s;
    call.operation = START;
    call.request = request;
    call.error = error;
    return call_policy(&call);
}
void rl_policy_close(struct rl_server *s, knurl_request *request,
                     const knurl_request_outcome *value)
{
    notify(s, CLOSE, request, value);
}
static void observe(knurl_server *server, const knurl_observation *value,
                    void *data)
{
    (void)server;
    notify(data, OBSERVE, NULL, value);
}
static void worker(knurl_server *server, const knurl_worker_event *value,
                   void *data)
{
    (void)server;
    notify(data, WORKER, NULL, value);
}
static void identity(knurl_server *server,
                     const knurl_tls_identity_event *value, void *data)
{
    (void)server;
    notify(data, IDENTITY, NULL, value);
}
static knurl_status resolve(knurl_request *request, char *buffer,
                            size_t capacity, size_t *length, void *data,
                            knurl_error *error, enum operation operation)
{
    struct policy_call call;
    memset(&call, 0, sizeof(call));
    call.server = data;
    call.operation = operation;
    call.request = request;
    call.buffer = buffer;
    call.capacity = capacity;
    call.length = length;
    call.error = error;
    return call_policy(&call);
}
static knurl_status address(knurl_request *r, char *b, size_t n, size_t *out,
                            void *data, knurl_error *e)
{
    return resolve(r, b, n, out, data, e, ADDRESS);
}
static knurl_status correlation(knurl_request *r, char *b, size_t n,
                                size_t *out, void *data, knurl_error *e)
{
    return resolve(r, b, n, out, data, e, CORRELATION);
}
static knurl_status admit(knurl_request *request, knurl_reply *reply,
                          const knurl_connection_info *info,
                          knurl_admission_action *action, void *data,
                          knurl_error *error)
{
    struct policy_call call;
    memset(&call, 0, sizeof(call));
    call.server = data;
    call.request = request;
    call.reply = reply;
    call.event = info;
    call.operation = request == NULL ? CONNECTION : ADMISSION;
    call.action = action;
    call.error = error;
    return call_policy(&call);
}
static knurl_status connection_admit(knurl_server *s,
                                     const knurl_connection_info *c,
                                     knurl_admission_action *a, void *data,
                                     knurl_error *e)
{
    (void)s;
    return admit(NULL, NULL, c, a, data, e);
}
static knurl_status request_admit(knurl_request *r, knurl_reply *reply,
                                  knurl_admission_action *a, void *data,
                                  knurl_error *e)
{
    return admit(r, reply, NULL, a, data, e);
}
static knurl_status control(knurl_server *s, unsigned int events, void *data,
                            knurl_error *error)
{
    struct policy_call call;
    (void)s;
    memset(&call, 0, sizeof(call));
    call.server = data;
    call.operation = CONTROL;
    call.events = events;
    call.error = error;
    return call_policy(&call);
}
static int function_field(lua_State *L, int index, const char *name,
                          int *present)
{
    int valid;
    rl_field(L, index, name);
    *present = lua_isfunction(L, -1);
    valid = *present || lua_isnil(L, -1);
    lua_pop(L, 1);
    return valid;
}
static int supervision(lua_State *L, int index,
                       knurl_supervision_config *config, struct rl_server *s,
                       knurl_error *error)
{
    static const char *const fields[] = {"enabled", "control_fd", "interest",
                                         "on_event"};
    size_t interest = config->interest;
    lua_Integer fd;
    int present;
    if (!rl_fields(L, index, fields, sizeof(fields) / sizeof(fields[0]), error))
        return 0;
    if (!rl_field_bool(L, index, "enabled", &config->enabled) ||
        !rl_field_size(L, index, "interest", &interest) ||
        interest > UINT_MAX || !function_field(L, index, "on_event", &present))
        goto invalid;
    config->interest = (unsigned int)interest;
    config->on_event = present ? control : NULL;
    config->userdata = s;
    rl_field(L, index, "control_fd");
    if (!lua_isnil(L, -1)) {
        if (!rl_integer(L, -1, &fd))
            goto invalid;
        if (fd < -1 || fd > INT_MAX)
            goto invalid;
        config->control_fd = (int)fd;
    }
    lua_pop(L, 1);
    return 1;
invalid:
    rl_fail(error, KNURL_INVALID,
            "invalid supervision descriptor, interest or callback");
    return 0;
}
int rl_policy_config(lua_State *L, int owner, knurl_server_config *config,
                     struct rl_server *s, knurl_error *error)
{
    static const char *const groups[] = {"client_ip", "correlation_id",
                                         "admission", "supervision"};
    static const char *const correlation_fields[] = {"resolve"};
    static const char *const admission_fields[] = {"on_connection",
                                                   "on_request"};
    size_t i;
    int options, group, present;
    lua_getiuservalue(L, owner, 1);
    options = lua_absindex(L, -1);
    for (i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i) {
        rl_field(L, options, groups[i]);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        if (!lua_istable(L, -1))
            goto invalid;
        rl_table_copy(L, -1);
        group = lua_absindex(L, -1);
        lua_pushvalue(L, group);
        lua_setfield(L, options, groups[i]);
        if (i < 2) {
            if (i == 1 && !rl_fields(L, group, correlation_fields, 1, error))
                return 0;
            if (!function_field(L, group, "resolve", &present))
                goto invalid;
            if (i == 0) {
                config->client_ip.resolve = present ? address : NULL;
                config->client_ip.userdata = s;
            } else {
                config->correlation_id.resolve = present ? correlation : NULL;
                config->correlation_id.userdata = s;
            }
        } else if (i == 2) {
            if (!rl_fields(L, group, admission_fields, 2, error))
                return 0;
            if (!function_field(L, group, "on_connection", &present))
                goto invalid;
            config->admission.on_connection = present ? connection_admit : NULL;
            if (!function_field(L, group, "on_request", &present))
                goto invalid;
            config->admission.on_request = present ? request_admit : NULL;
            config->admission.userdata = s;
        } else if (!supervision(L, group, &config->supervision, s, error))
            return 0;
        lua_pop(L, 2);
    }
    rl_field(L, options, "callbacks");
    if (lua_istable(L, -1)) {
        if (!function_field(L, -1, "on_observe", &present))
            goto invalid;
        config->callbacks.on_observe = present ? observe : NULL;
        if (!function_field(L, -1, "on_worker_event", &present))
            goto invalid;
        config->callbacks.on_worker_event = present ? worker : NULL;
        if (!function_field(L, -1, "on_tls_identity", &present))
            goto invalid;
        config->callbacks.on_tls_identity = present ? identity : NULL;
    }
    lua_pop(L, 2);
    return 1;
invalid:
    rl_fail(error, KNURL_INVALID, "invalid policy group or callback");
    return 0;
}
int rl_control_interest(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    size_t interest;
    knurl_error error;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "server is closed"), &error));
    if (!rl_size(L, 2, &interest) || interest > UINT_MAX)
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "server and control interest required"),
                            &error));
    if (knurl_server_control_interest(s->native, (unsigned int)interest,
                                      &error) != KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
int rl_supervision_take(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    knurl_server_config defaults;
    knurl_error error;
    int options, old, next;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "server is closed"), &error));
    if (!lua_istable(L, 2))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "server and supervision options required"),
                            &error));
    knurl_server_config_init(&defaults);
    rl_table_copy(L, 2);
    next = lua_absindex(L, -1);
    if (!supervision(L, next, &defaults.supervision, s, &error))
        return rl_error(L, &error);
    lua_getiuservalue(L, 1, 1);
    options = lua_absindex(L, -1);
    rl_field(L, options, "supervision");
    old = lua_absindex(L, -1);
    lua_pushvalue(L, next);
    lua_setfield(L, options, "supervision");
    if (knurl_server_supervision_take(s->native, &defaults.supervision,
                                      &error) != KNURL_OK) {
        lua_pushvalue(L, old);
        lua_setfield(L, options, "supervision");
        return rl_error(L, &error);
    }
    lua_pushboolean(L, 1);
    return 1;
}
int rl_tls_identity_state(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    knurl_tls_identity_state state;
    knurl_error error;
    size_t index = 1;
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, KNURL_STATE, "server is closed"), &error));
    if (!lua_isnoneornil(L, 2) && (!rl_size(L, 2, &index) || !index))
        return rl_error(L,
                        (rl_fail(&error, KNURL_INVALID,
                                 "server and positive identity index required"),
                         &error));
    if (knurl_server_tls_identity_state(s->native, index - 1, &state, &error) !=
        KNURL_OK)
        return rl_error(L, &error);
    lua_pushstring(L, state == KNURL_TLS_USABLE ? "usable" : "pending");
    return 1;
}
void rl_policy_open(lua_State *L)
{
    lua_newtable(L);
    integer(L, "READ", KNURL_CONTROL_READ);
    integer(L, "WRITE", KNURL_CONTROL_WRITE);
    integer(L, "EOF", KNURL_CONTROL_EOF);
    integer(L, "ERROR", KNURL_CONTROL_ERROR);
    lua_setfield(L, -2, "control");
}
