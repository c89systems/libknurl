#include "binding.h"
#define N(a) (sizeof(a) / sizeof((a)[0]))
static int static_route(lua_State *L, int kind)
{
    static const char *const fields[] = {"path",
                                         "file_path",
                                         "root_dir",
                                         "content_type",
                                         "cache_control",
                                         "index_file",
                                         "index_enabled",
                                         "not_found_body",
                                         "not_found_content_type",
                                         "method",
                                         "methods",
                                         "auth",
                                         "acl",
                                         "before",
                                         "after",
                                         "provider",
                                         "mime_types",
                                         "compression_disabled",
                                         "precompressed_disabled"};
    struct rl_rule *rule;
    knurl_static_config config;
    knurl_error error;
    knurl_status status;
    int index, root;
    if (!lua_istable(L, 2) || !rl_fields(L, 2, fields, N(fields), &error))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "invalid static declaration fields"),
                            &error));
    knurl_static_config_init(&config);
    config.index_enabled = kind == 2;
    rl_field(L, 2, "provider");
    config.provider = rl_asset_provider(L, -1);
    if ((kind == 2 && config.provider == NULL) ||
        (kind != 2 && !lua_isnil(L, -1)))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "static_assets requires an asset provider"),
                            &error));
    lua_pop(L, 1);
    if (!rl_field_string(L, 2, "path", &config.path) ||
        !rl_field_string(L, 2, "file_path", &config.file_path) ||
        !rl_field_string(L, 2, "root_dir", &config.root_dir) ||
        !rl_field_string(L, 2, "content_type", &config.content_type) ||
        !rl_field_string(L, 2, "cache_control", &config.cache_control) ||
        !rl_field_string(L, 2, "index_file", &config.index_file) ||
        !rl_field_string(L, 2, "not_found_body", &config.not_found_body) ||
        !rl_field_string(L, 2, "not_found_content_type",
                         &config.not_found_content_type) ||
        !rl_field_bool(L, 2, "index_enabled", &config.index_enabled) ||
        !rl_field_bool(L, 2, "compression_disabled",
                       &config.compression_disabled) ||
        !rl_field_bool(L, 2, "precompressed_disabled",
                       &config.precompressed_disabled) ||
        !rl_methods(L, 2, config.methods, &config.methods))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "invalid static declaration value"),
                            &error));
    rl_field(L, 2, "mime_types");
    if (!rl_mime_types(L, -1, &config.mime_types, &config.mime_type_count,
                       &error))
        return rl_error(L, &error);
    lua_remove(L, -2);
    rule = rl_rule_new(L, &error);
    if (rule == NULL)
        return rl_error(L, &error);
    rule->native_reply = 1;
    index = lua_absindex(L, -1);
    rl_rule_hooks(&config.hooks, rule);
    config.acl = rule->acl;
    root = rl_rule_root(L, index, rule);
    if (rule->group != NULL)
        status = kind == 2 ? knurl_route_group_static_assets(
                                 rule->group, &config, NULL, &error)
                 : kind == 1
                     ? knurl_route_group_static_directory(rule->group, &config,
                                                          NULL, &error)
                     : knurl_route_group_static_file(rule->group, &config, NULL,
                                                     &error);
    else
        status = kind == 2   ? knurl_router_static_assets(
                                   knurl_server_router(rule->server->native),
                                   &config, NULL, &error)
                 : kind == 1 ? knurl_router_static_directory(
                                   knurl_server_router(rule->server->native),
                                   &config, NULL, &error)
                             : knurl_router_static_file(
                                   knurl_server_router(rule->server->native),
                                   &config, NULL, &error);
    return rl_rule_finish(L, index, root, rule, status, &error);
}
int rl_static_file(lua_State *L) { return static_route(L, 0); }
int rl_static_directory(lua_State *L) { return static_route(L, 1); }
int rl_static_assets(lua_State *L) { return static_route(L, 2); }
int rl_upload_file(lua_State *L)
{
    static const char *const fields[] = {
        "path", "path_kind", "file_path", "method", "methods", "max_body_bytes",
        "body", "auth",      "acl",       "before", "after"};
    struct rl_rule *rule;
    knurl_route_config route;
    knurl_upload_file_config config;
    knurl_error error;
    knurl_status status;
    int index, root;
    if (!lua_istable(L, 2) || !rl_fields(L, 2, fields, N(fields), &error))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "invalid upload-file declaration fields"),
                            &error));
    knurl_route_config_init(&route);
    knurl_upload_file_config_init(&config);
    if (!rl_route_path(L, 2, &route) ||
        !rl_field_string(L, 2, "file_path", &config.file_path) ||
        !rl_methods(L, 2, KNURL_HTTP_METHOD_MASK(KNURL_HTTP_POST),
                    &config.methods))
        goto invalid;
    config.path = route.path;
    config.path_kind = route.path_kind;
    rl_field(L, 2, "max_body_bytes");
    if (!lua_isnil(L, -1) && !rl_u64(L, -1, &config.body.max_bytes))
        goto invalid;
    lua_pop(L, 1);
    rl_field(L, 2, "body");
    if (!lua_isnil(L, -1) && !rl_body_policy(L, -1, &config.body, 1, &error))
        return rl_error(L, &error);
    lua_pop(L, 1);
    rule = rl_rule_new(L, &error);
    if (rule == NULL)
        return rl_error(L, &error);
    rule->is_upload = rule->native_reply = 1;
    index = lua_absindex(L, -1);
    rl_rule_hooks(&config.hooks, rule);
    config.acl = rule->acl;
    root = rl_rule_root(L, index, rule);
    status =
        rule->group != NULL
            ? knurl_route_group_upload_file(rule->group, &config, NULL, &error)
            : knurl_router_upload_file(
                  knurl_server_router(rule->server->native), &config, NULL,
                  &error);
    return rl_rule_finish(L, index, root, rule, status, &error);
invalid:
    return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                "invalid upload-file declaration value"),
                        &error));
}
struct dav_call {
    struct rl_rule *rule;
    const knurl_webdav_auth_request *input;
    knurl_webdav_auth_response *output;
    knurl_status status;
    knurl_error *error;
};
static int dav_auth_trampoline(lua_State *L)
{
    struct dav_call *call = lua_touserdata(L, 1);
    struct rl_request *request = knurl_request_userdata(call->input->request);
    struct rl_auth_result result;
    int options;
    rl_rule_options(L, call->rule);
    options = lua_absindex(L, -1);
    call->status =
        rl_auth_decide(L, options, call->input->request,
                       call->input->resource_path, &result, call->error);
    if (call->status != KNURL_OK)
        return 0;
    if (!result.present) {
        call->status = rl_fail(call->error, KNURL_STATE,
                               "WebDAV callback auth is missing");
        return 0;
    }
    call->output->action =
        !strcmp(result.action, "allow")      ? KNURL_WEBDAV_ALLOW
        : !strcmp(result.action, "required") ? KNURL_WEBDAV_CHALLENGE
        : !strcmp(result.action, "redirect") ? KNURL_WEBDAV_REDIRECT
                                             : KNURL_WEBDAV_DENY;
    call->output->status = (int)result.code;
    call->output->principal = result.principal;
    call->output->location = result.location;
    call->output->www_authenticate = result.challenge;
    call->output->content_type = result.type;
    call->output->body = result.body;
    rl_request_push(L, request);
    lua_pushvalue(L, result.snapshot);
    lua_setiuservalue(L, -2, 7);
    return 0;
}
static knurl_status dav_auth(const knurl_webdav_auth_request *input,
                             knurl_webdav_auth_response *output, void *data,
                             knurl_error *error)
{
    struct dav_call call;
    knurl_status status;
    memset(&call, 0, sizeof(call));
    call.rule = data;
    call.input = input;
    call.output = output;
    call.error = error;
    status =
        rl_protect(call.rule->server->L, dav_auth_trampoline, &call, error);
    return status == KNURL_OK ? call.status : status;
}
int rl_webdav(lua_State *L)
{
    static const char *const fields[] = {"path_prefix",
                                         "cache_dir",
                                         "site_id",
                                         "root_dir",
                                         "provider",
                                         "max_file_bytes",
                                         "max_total_bytes",
                                         "max_resources",
                                         "max_body_bytes",
                                         "content_type",
                                         "cache_control",
                                         "operation_timeout_ms",
                                         "operation_timeout_disabled",
                                         "auth_required",
                                         "conceal_unauthorized",
                                         "auth",
                                         "acl",
                                         "before",
                                         "after"};
    struct rl_rule *rule;
    knurl_webdav_config config;
    knurl_error error;
    knurl_status status;
    size_t timeout = 0;
    int index, root, required = 1, conceal = 1;
    if (!lua_istable(L, 2) || !rl_fields(L, 2, fields, N(fields), &error))
        return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                    "invalid WebDAV declaration fields"),
                            &error));
    knurl_webdav_config_init(&config);
    rl_field(L, 2, "operation_timeout_ms");
    config.operation_timeout_disabled = lua_isnil(L, -1);
    lua_pop(L, 1);
    rl_field(L, 2, "provider");
    if (!lua_isnil(L, -1)) {
        config.provider = rl_storage_provider(L, -1);
        if (config.provider == NULL)
            goto invalid;
    }
    lua_pop(L, 1);
    if (!rl_field_string(L, 2, "path_prefix", &config.path_prefix) ||
        !rl_field_string(L, 2, "cache_dir", &config.storage.cache_dir) ||
        !rl_field_string(L, 2, "site_id", &config.storage.site_id) ||
        !rl_field_string(L, 2, "root_dir", &config.storage.root_dir) ||
        !rl_field_string(L, 2, "content_type", &config.content_type) ||
        !rl_field_string(L, 2, "cache_control", &config.cache_control) ||
        !rl_field_size(L, 2, "max_file_bytes",
                       &config.storage.max_file_bytes) ||
        !rl_field_size(L, 2, "max_total_bytes",
                       &config.storage.max_total_bytes) ||
        !rl_field_size(L, 2, "max_resources", &config.storage.max_resources) ||
        !rl_field_size(L, 2, "operation_timeout_ms", &timeout) ||
        timeout > LONG_MAX ||
        !rl_field_bool(L, 2, "operation_timeout_disabled",
                       &config.operation_timeout_disabled) ||
        !rl_field_bool(L, 2, "auth_required", &required) ||
        !rl_field_bool(L, 2, "conceal_unauthorized", &conceal))
        goto invalid;
    config.operation_timeout_ms = (long)timeout;
    config.auth_disabled = !required;
    config.conceal_disabled = !conceal;
    (void)knurl_u64_from_size(config.storage.max_file_bytes,
                              &config.max_body_bytes, NULL);
    rl_field(L, 2, "max_body_bytes");
    if (!lua_isnil(L, -1) && !rl_u64(L, -1, &config.max_body_bytes))
        goto invalid;
    lua_pop(L, 1);
    rl_field(L, 2, "auth");
    if (required && lua_isnil(L, -1))
        goto invalid;
    lua_pop(L, 1);
    rule = rl_rule_new(L, &error);
    if (rule == NULL)
        return rl_error(L, &error);
    rule->native_reply = 1;
    index = lua_absindex(L, -1);
    rl_rule_hooks(&config.hooks, rule);
    config.acl = rule->acl;
    /* Root/child auth belongs to WebDAV, not a second generic route gate. */
    config.hooks.auth = NULL;
    if (required) {
        config.auth = dav_auth;
        config.auth_userdata = rule;
    }
    root = rl_rule_root(L, index, rule);
    status =
        rule->group != NULL
            ? knurl_route_group_webdav(rule->group, &config, NULL, &error)
            : knurl_router_webdav(knurl_server_router(rule->server->native),
                                  &config, NULL, &error);
    return rl_rule_finish(L, index, root, rule, status, &error);
invalid:
    return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                "invalid WebDAV declaration value; protected "
                                "mounts require callback auth"),
                        &error));
}
