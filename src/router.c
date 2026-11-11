#include "router_internal.h"
#include <knurl/proxy.h>
#include <knurl/websocket.h>
#include <knurl/xml.h>
#include <knurl/upload.h>
#include <knurl/lonejson.h>
#include <knurl/router.h>
#include <knurl/openapi.h>
#include <knurl/static.h>
#include <knurl/webdav.h>
#include "openapi_internal.h"
#include "body_internal.h"
#include "server_internal.h"

#include <stdlib.h>
#include <string.h>

void knurl_body_policy_init(knurl_body_policy *policy)
{
    if (policy == NULL)
        return;
    policy->mode = KNURL_BODY_NONE;
    policy->max_bytes.high = policy->max_bytes.low = 0;
    policy->memory_buffer_limit_bytes = 0;
    policy->disk_spool_disabled = 0;
}

void knurl_body_policy_init_json(knurl_body_policy *policy)
{
    knurl_body_policy_init(policy);
    if (policy != NULL)
        policy->mode = KNURL_BODY_JSON;
}

void knurl_body_policy_init_buffered(knurl_body_policy *policy)
{
    knurl_body_policy_init(policy);
    if (policy != NULL)
        policy->mode = KNURL_BODY_BUFFERED;
}

void knurl_body_policy_init_upload(knurl_body_policy *policy)
{
    knurl_body_policy_init(policy);
    if (policy != NULL) {
        policy->mode = KNURL_BODY_STREAMING_UPLOAD;
        policy->max_bytes.low = 3221225472UL;
    }
}

static void hooks_init(knurl_route_hooks *hooks)
{
    hooks->auth = hooks->before = hooks->after = NULL;
    hooks->userdata = NULL;
}

void knurl_route_config_init(knurl_route_config *config)
{
    if (config == NULL)
        return;
    config->method = KNURL_HTTP_ANY;
    config->methods = 0;
    config->path = NULL;
    config->path_kind = KNURL_PATH_LITERAL;
    knurl_body_policy_init(&config->body);
    config->handler = NULL;
    config->on_headers = NULL;
    config->userdata = NULL;
    hooks_init(&config->hooks);
}

void knurl_route_group_config_init(knurl_route_group_config *config)
{
    if (config != NULL) {
        config->prefix = "";
        hooks_init(&config->hooks);
    }
}

knurl_status knurl_router_new(knurl_router **out, knurl_error *error)
{
    knurl_router *router;
    if (out != NULL)
        *out = NULL;
    if (out == NULL)
        return knurl_fail(error, KNURL_INVALID, "router_new",
                          "router output is required");
    router = malloc(sizeof(struct knurl_router_impl));
    if (router == NULL)
        return knurl_fail(error, KNURL_NOMEM, "router_new",
                          "cannot allocate router");
    knurl_router_receiver_init(router);
    router->impl->openapi = NULL;
    router->impl->routes = NULL;
    router->impl->count = 0;
    router->impl->groups = NULL;
    router->impl->server = NULL;
    router->impl->frozen = 0;
    *out = router;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_router_release(knurl_router *router)
{
    knurl_route_group *group;
    knurl_route_group *next;
    size_t i;
    for (i = 0; i < router->impl->count; ++i) {
        if (router->impl->routes[i]->impl->has_pattern)
            regfree(&router->impl->routes[i]->impl->pattern);
        free((void *)router->impl->routes[i]->impl->config.path);
        if (router->impl->routes[i]->impl->release_userdata != NULL)
            router->impl->routes[i]->impl->release_userdata(
                router->impl->routes[i]->impl->config.userdata);
        free(router->impl->routes[i]);
    }
    knurl_openapi_clear(router);
    free(router->impl->routes);
    for (group = router->impl->groups; group != NULL; group = next) {
        next = group->impl->next;
        free((void *)group->impl->config.prefix);
        free(group);
    }
    free(router);
}

knurl_status knurl_router_destroy(knurl_router *router, knurl_error *error)
{
    if (router != NULL) {
        if (router->impl->server != NULL || router->impl->frozen)
            return knurl_fail(error, KNURL_STATE, "router_destroy",
                              "router is attached or active");
        knurl_router_release(router);
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status body_check(knurl_router *router,
                               const knurl_body_policy *input,
                               knurl_error *error)
{
    knurl_body_policy policy = *input;
    knurl_body *body;
    knurl_u64 fallback, hard;
    int order;
    knurl_status status;
    if (input->mode == KNURL_BODY_NONE || input->mode == KNURL_BODY_STREAM)
        return KNURL_OK;
    fallback.high = 0;
    fallback.low = 10485760UL;
    if (router->impl->server != NULL)
        fallback = router->impl->server->impl->config.limits.default_body_bytes;
    policy.max_bytes = knurl_body_policy_limit(input, fallback);
    if (router->impl->server != NULL) {
        hard = router->impl->server->impl->config.limits.hard_body_bytes;
        if (hard.high || hard.low) {
            (void)knurl_u64_compare(policy.max_bytes, hard, &order, NULL);
            if (order > 0)
                return knurl_fail(
                    error, KNURL_INVALID, "router_route",
                    "route body limit exceeds the server body limit");
        }
    }
    status = knurl_body_create(&policy, NULL, &body, error);
    if (status == KNURL_OK)
        knurl_body_release(body);
    return status;
}

static knurl_status route_add(knurl_router *router,
                              const knurl_route_config *input,
                              knurl_route_group *group, knurl_route **out,
                              knurl_error *error, int facade)
{
    knurl_route *route;
    knurl_route **grown;
    knurl_http_methods methods;
    knurl_status status;
    size_t i;
    int code;
    if (out != NULL)
        *out = NULL;
    if (router == NULL || input == NULL ||
        ((input->handler == NULL) == (input->on_headers == NULL)))
        return knurl_fail(error, KNURL_INVALID, "router_route",
                          "provide a router, declaration and exactly one "
                          "handler or on_headers callback");
    if (!facade && input->on_headers != NULL &&
        (input->hooks.auth != NULL || input->hooks.before != NULL ||
         input->hooks.after != NULL ||
         (group != NULL && (group->impl->config.hooks.auth != NULL ||
                            group->impl->config.hooks.before != NULL ||
                            group->impl->config.hooks.after != NULL))))
        return knurl_fail(error, KNURL_INVALID, "router_route",
                          "raw on_headers routes own admission and cannot use "
                          "staged facade hooks");
    if (router->impl->frozen)
        return knurl_fail(error, KNURL_STATE, "router_route",
                          "route registration is unavailable during a run");
    if (input->method < KNURL_HTTP_ANY || input->method > KNURL_HTTP_MOVE)
        return knurl_fail(error, KNURL_INVALID, "router_route",
                          "unknown HTTP method");
    methods = input->methods;
    if (methods == 0)
        methods = input->method == KNURL_HTTP_ANY
                      ? KNURL_HTTP_METHODS_ALL
                      : KNURL_HTTP_METHOD_MASK(input->method);
    if ((methods & ~KNURL_HTTP_METHODS_ALL) != 0)
        return knurl_fail(error, KNURL_INVALID, "router_route",
                          "HTTP method mask contains unsupported bits");
    if (input->body.mode < KNURL_BODY_NONE ||
        input->body.mode > KNURL_BODY_STREAM ||
        !knurl_u64_valid(input->body.max_bytes))
        return knurl_fail(error, KNURL_INVALID, "router_route",
                          "invalid body policy mode or wide limit");
    status = input->on_headers == NULL ? body_check(router, &input->body, error)
                                       : KNURL_OK;
    if (status != KNURL_OK)
        return status;
    status = knurl_route_path_validate(input->path, input->path_kind, error);
    if (status != KNURL_OK)
        return status;
    for (i = 0; i < router->impl->count; ++i) {
        if ((router->impl->routes[i]->impl->config.methods & methods) != 0 &&
            router->impl->routes[i]->impl->config.path_kind ==
                input->path_kind &&
            strcmp(router->impl->routes[i]->impl->config.path, input->path) ==
                0)
            return knurl_fail(
                error, KNURL_CONFLICT, "router_route",
                "identical route kind/path overlaps an existing method mask");
    }
    route = malloc(sizeof(struct knurl_route_impl));
    if (route == NULL)
        return knurl_fail(error, KNURL_NOMEM, "router_route",
                          "cannot allocate route");
    knurl_route_receiver_init(route);
    route->impl->config = *input;
    route->impl->config.methods = methods;
    route->impl->config.path = knurl_copy_text(input->path);
    route->impl->has_pattern = 0;
    route->impl->raw_path = 0;
    route->impl->directory_mount = 0;
    route->impl->preflight = NULL;
    route->impl->group = group;
    route->impl->release_userdata = NULL;
    if (route->impl->config.path == NULL) {
        free(route);
        return knurl_fail(error, KNURL_NOMEM, "router_route",
                          "cannot copy route path");
    }
    if (input->path_kind == KNURL_PATH_REGEX) {
        code = regcomp(&route->impl->pattern, input->path,
                       REG_EXTENDED | REG_NOSUB);
        if (code != 0) {
            free((void *)route->impl->config.path);
            free(route);
            status = knurl_fail(
                error, code == REG_ESPACE ? KNURL_NOMEM : KNURL_INVALID,
                "router_route", "cannot compile POSIX extended route regex");
            if (error != NULL)
                error->native_code = code;
            return status;
        }
        route->impl->has_pattern = 1;
    }
    if (router->impl->count >= (size_t)-1 / sizeof(*grown)) {
        status = knurl_fail(error, KNURL_OVERFLOW, "router_route",
                            "route registry size overflow");
        goto failed;
    }
    grown = realloc(router->impl->routes,
                    (router->impl->count + 1) * sizeof(*grown));
    if (grown == NULL) {
        status = knurl_fail(error, KNURL_NOMEM, "router_route",
                            "cannot grow route registry");
        goto failed;
    }
    router->impl->routes = grown;
    router->impl->routes[router->impl->count++] = route;
    if (out != NULL)
        *out = route;
    knurl_error_clear(error);
    return KNURL_OK;
failed:
    if (route->impl->has_pattern)
        regfree(&route->impl->pattern);
    free((void *)route->impl->config.path);
    free(route);
    return status;
}

knurl_status knurl_router_route(knurl_router *router,
                                const knurl_route_config *config,
                                knurl_route **out, knurl_error *error)
{
    return route_add(router, config, NULL, out, error, 0);
}

knurl_status knurl_router_group(knurl_router *router,
                                const knurl_route_group_config *config,
                                knurl_route_group **out, knurl_error *error)
{
    knurl_route_group_config defaults;
    knurl_route_group *group;
    knurl_status status;
    const char *prefix;
    char *copy;
    size_t length;
    if (out != NULL)
        *out = NULL;
    if (router == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "router_group",
                          "router and group output are required");
    if (router->impl->frozen)
        return knurl_fail(error, KNURL_STATE, "router_group",
                          "group registration is unavailable during a run");
    knurl_route_group_config_init(&defaults);
    if (config == NULL)
        config = &defaults;
    prefix = config->prefix != NULL ? config->prefix : "";
    copy = knurl_copy_text(prefix);
    if (copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "router_group",
                          "cannot copy group prefix");
    length = strlen(copy);
    if (length > 1 && copy[length - 1] == '/')
        copy[length - 1] = '\0';
    if (*copy != '\0') {
        status = knurl_route_path_validate(copy, KNURL_PATH_PARAMS, error);
        if (status != KNURL_OK) {
            free(copy);
            return status;
        }
    }
    group = malloc(sizeof(struct knurl_route_group_impl));
    if (group == NULL) {
        free(copy);
        return knurl_fail(error, KNURL_NOMEM, "router_group",
                          "cannot allocate group");
    }
    knurl_route_group_receiver_init(group);
    group->impl->config = *config;
    group->impl->config.prefix = copy;
    group->impl->router = router;
    group->impl->next = router->impl->groups;
    router->impl->groups = group;
    *out = group;
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status group_route(knurl_route_group *group,
                                const knurl_route_config *config,
                                knurl_route **out, knurl_error *error,
                                int facade)
{
    knurl_route_config joined;
    char *path;
    char *target;
    const char *p;
    const char *prefix;
    size_t prefix_length;
    size_t path_length;
    size_t escaped;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (group == NULL || config == NULL || config->path == NULL)
        return knurl_fail(error, KNURL_INVALID, "group_route",
                          "group and route declaration are required");
    if (group->impl->router->impl->frozen)
        return knurl_fail(error, KNURL_STATE, "group_route",
                          "route registration is unavailable during a run");
    status = knurl_route_path_validate(config->path, config->path_kind, error);
    if (status != KNURL_OK)
        return status;
    prefix = group->impl->config.prefix;
    if (*prefix == '\0' || strcmp(prefix, "/") == 0)
        return route_add(group->impl->router, config, group, out, error,
                         facade);
    prefix_length = strlen(prefix);
    path_length = strlen(config->path);
    escaped = prefix_length;
    if (config->path_kind == KNURL_PATH_REGEX) {
        for (p = prefix; *p != '\0'; ++p) {
            if (strchr(".+*?^$()[]{}|\\", *p) != NULL) {
                if (escaped == (size_t)-1)
                    return knurl_fail(error, KNURL_OVERFLOW, "group_route",
                                      "escaped prefix size overflow");
                ++escaped;
            }
        }
    }
    if (path_length == (size_t)-1 || escaped > (size_t)-1 - path_length - 1)
        return knurl_fail(error, KNURL_OVERFLOW, "group_route",
                          "prefixed path size overflow");
    path = malloc(escaped + path_length + 1);
    if (path == NULL)
        return knurl_fail(error, KNURL_NOMEM, "group_route",
                          "cannot allocate prefixed path");
    target = path;
    if (config->path_kind == KNURL_PATH_REGEX)
        *target++ = '^';
    for (p = prefix; *p != '\0'; ++p) {
        if (config->path_kind == KNURL_PATH_REGEX &&
            strchr(".+*?^$()[]{}|\\", *p) != NULL)
            *target++ = '\\';
        *target++ = *p;
    }
    memcpy(target,
           config->path + (config->path_kind == KNURL_PATH_REGEX ? 1 : 0),
           path_length + 1 - (config->path_kind == KNURL_PATH_REGEX ? 1 : 0));
    joined = *config;
    joined.path = path;
    if (joined.path_kind != KNURL_PATH_REGEX && strchr(path, ':') != NULL)
        joined.path_kind = KNURL_PATH_PARAMS;
    status = route_add(group->impl->router, &joined, group, out, error, facade);
    free(path);
    return status;
}

size_t knurl_router_route_count(const knurl_router *router)
{
    return router != NULL ? router->impl->count : 0;
}

knurl_status knurl_router_route_at(const knurl_router *router, size_t index,
                                   const knurl_route **out, knurl_error *error)
{
    if (out != NULL)
        *out = NULL;
    if (router == NULL || out == NULL || index >= router->impl->count)
        return knurl_fail(error, KNURL_INVALID, "router_route_at",
                          "route index is outside the registry");
    *out = router->impl->routes[index];
    knurl_error_clear(error);
    return KNURL_OK;
}

const knurl_route_config *knurl_route_declaration(const knurl_route *route)
{
    return route != NULL ? &route->impl->config : NULL;
}

knurl_status knurl_route_group_route(knurl_route_group *group,
                                     const knurl_route_config *config,
                                     knurl_route **out, knurl_error *error)
{
    return group_route(group, config, out, error, 0);
}
knurl_status knurl_router_facade_route(knurl_router *router,
                                       knurl_route_group *group,
                                       const knurl_route_config *config,
                                       knurl_route **out, knurl_error *error)
{
    return group != NULL ? group_route(group, config, out, error, 1)
                         : route_add(router, config, NULL, out, error, 1);
}

void knurl_router_receiver_init(knurl_router *receiver)
{
    static const knurl_router methods = {knurl_router_proxy,
                                         knurl_router_websocket,
                                         knurl_router_xml_route,
                                         knurl_router_upload_stream,
                                         knurl_router_upload_reader,
                                         knurl_router_upload_file,
                                         knurl_router_json_route,
                                         knurl_router_json_typed_route,
                                         knurl_router_rest_route,
                                         knurl_router_destroy,
                                         knurl_router_route,
                                         knurl_router_group,
                                         knurl_router_route_count,
                                         knurl_router_route_at,
                                         knurl_router_dispatch_headers,
                                         knurl_router_openapi_doc,
                                         knurl_router_openapi,
                                         knurl_router_static_file,
                                         knurl_router_static_directory,
                                         knurl_router_static_assets,
                                         knurl_router_webdav,
                                         NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_router_impl *)receiver;
}

void knurl_route_group_receiver_init(knurl_route_group *receiver)
{
    static const knurl_route_group methods = {
        knurl_route_group_xml_route,
        knurl_route_group_upload_stream,
        knurl_route_group_upload_reader,
        knurl_route_group_upload_file,
        knurl_route_group_json_route,
        knurl_route_group_json_typed_route,
        knurl_route_group_rest_route,
        knurl_route_group_route,
        knurl_route_group_static_file,
        knurl_route_group_static_directory,
        knurl_route_group_static_assets,
        knurl_route_group_webdav,
        NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_route_group_impl *)receiver;
}

void knurl_route_receiver_init(knurl_route *receiver)
{
    static const knurl_route methods = {knurl_route_declaration, NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_route_impl *)receiver;
}
