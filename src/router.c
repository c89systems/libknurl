#include "router_internal.h"

#include <stdlib.h>
#include <string.h>

void rotmos_body_policy_init(rotmos_body_policy *policy)
{
    if (policy == NULL)
        return;
    policy->mode = ROTMOS_BODY_NONE;
    policy->max_bytes.high = policy->max_bytes.low = 0;
    policy->memory_buffer_limit_bytes = 262144UL;
    policy->disk_spool_disabled = 0;
}

void rotmos_body_policy_init_json(rotmos_body_policy *policy)
{
    rotmos_body_policy_init(policy);
    if (policy != NULL)
        policy->mode = ROTMOS_BODY_JSON;
}

void rotmos_body_policy_init_buffered(rotmos_body_policy *policy)
{
    rotmos_body_policy_init(policy);
    if (policy != NULL)
        policy->mode = ROTMOS_BODY_BUFFERED;
}

void rotmos_body_policy_init_upload(rotmos_body_policy *policy)
{
    rotmos_body_policy_init(policy);
    if (policy != NULL) {
        policy->mode = ROTMOS_BODY_STREAMING_UPLOAD;
        policy->max_bytes.low = 3221225472UL;
    }
}

static void hooks_init(rotmos_route_hooks *hooks)
{
    hooks->auth = hooks->before = hooks->after = NULL;
    hooks->userdata = NULL;
}

void rotmos_route_config_init(rotmos_route_config *config)
{
    if (config == NULL)
        return;
    config->method = ROTMOS_HTTP_ANY;
    config->methods = 0;
    config->path = NULL;
    config->path_kind = ROTMOS_PATH_LITERAL;
    rotmos_body_policy_init(&config->body);
    config->handler = NULL;
    config->userdata = NULL;
    hooks_init(&config->hooks);
}

void rotmos_route_group_config_init(rotmos_route_group_config *config)
{
    if (config != NULL) {
        config->prefix = "";
        hooks_init(&config->hooks);
    }
}

rotmos_status rotmos_router_new(rotmos_router **out, rotmos_error *error)
{
    rotmos_router *router;
    if (out != NULL)
        *out = NULL;
    if (out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "router_new",
                           "router output is required");
    router = malloc(sizeof(*router));
    if (router == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "router_new",
                           "cannot allocate router");
    router->routes = NULL;
    router->count = 0;
    router->groups = NULL;
    router->server = NULL;
    router->frozen = 0;
    *out = router;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_router_release(rotmos_router *router)
{
    rotmos_route_group *group;
    rotmos_route_group *next;
    size_t i;
    for (i = 0; i < router->count; ++i) {
        if (router->routes[i]->has_pattern)
            regfree(&router->routes[i]->pattern);
        free((void *)router->routes[i]->config.path);
        free(router->routes[i]);
    }
    free(router->routes);
    for (group = router->groups; group != NULL; group = next) {
        next = group->next;
        free((void *)group->config.prefix);
        free(group);
    }
    free(router);
}

rotmos_status rotmos_router_destroy(rotmos_router *router, rotmos_error *error)
{
    if (router != NULL) {
        if (router->server != NULL || router->frozen)
            return rotmos_fail(error, ROTMOS_STATE, "router_destroy",
                               "router is attached or active");
        rotmos_router_release(router);
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static rotmos_status route_add(rotmos_router *router,
                               const rotmos_route_config *input,
                               rotmos_route_group *group, rotmos_route **out,
                               rotmos_error *error)
{
    rotmos_route *route;
    rotmos_route **grown;
    rotmos_http_methods methods;
    rotmos_status status;
    size_t i;
    int code;
    if (out != NULL)
        *out = NULL;
    if (router == NULL || input == NULL || input->handler == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                           "router, declaration and handler are required");
    if (router->frozen)
        return rotmos_fail(error, ROTMOS_STATE, "router_route",
                           "route registration is unavailable during a run");
    if (input->method < ROTMOS_HTTP_ANY || input->method > ROTMOS_HTTP_MOVE)
        return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                           "unknown HTTP method");
    methods = input->methods;
    if (methods == 0)
        methods = input->method == ROTMOS_HTTP_ANY
                      ? ROTMOS_HTTP_METHODS_ALL
                      : ROTMOS_HTTP_METHOD_MASK(input->method);
    if ((methods & ~ROTMOS_HTTP_METHODS_ALL) != 0)
        return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                           "HTTP method mask contains unsupported bits");
    if (input->body.mode < ROTMOS_BODY_NONE ||
        input->body.mode > ROTMOS_BODY_STREAMING_UPLOAD ||
        !rotmos_u64_valid(input->body.max_bytes))
        return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                           "invalid body policy mode or wide limit");
    status = rotmos_route_path_validate(input->path, input->path_kind, error);
    if (status != ROTMOS_OK)
        return status;
    for (i = 0; i < router->count; ++i) {
        if ((router->routes[i]->config.methods & methods) != 0 &&
            router->routes[i]->config.path_kind == input->path_kind &&
            strcmp(router->routes[i]->config.path, input->path) == 0)
            return rotmos_fail(
                error, ROTMOS_CONFLICT, "router_route",
                "identical route kind/path overlaps an existing method mask");
    }
    route = malloc(sizeof(*route));
    if (route == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "router_route",
                           "cannot allocate route");
    route->config = *input;
    route->config.methods = methods;
    route->config.path = rotmos_copy_text(input->path);
    route->has_pattern = 0;
    route->group = group;
    if (route->config.path == NULL) {
        free(route);
        return rotmos_fail(error, ROTMOS_NOMEM, "router_route",
                           "cannot copy route path");
    }
    if (input->path_kind == ROTMOS_PATH_REGEX) {
        code = regcomp(&route->pattern, input->path, REG_EXTENDED | REG_NOSUB);
        if (code != 0) {
            free((void *)route->config.path);
            free(route);
            status = rotmos_fail(
                error, code == REG_ESPACE ? ROTMOS_NOMEM : ROTMOS_INVALID,
                "router_route", "cannot compile POSIX extended route regex");
            if (error != NULL)
                error->native_code = code;
            return status;
        }
        route->has_pattern = 1;
    }
    if (router->count >= (size_t)-1 / sizeof(*grown)) {
        status = rotmos_fail(error, ROTMOS_OVERFLOW, "router_route",
                             "route registry size overflow");
        goto failed;
    }
    grown = realloc(router->routes, (router->count + 1) * sizeof(*grown));
    if (grown == NULL) {
        status = rotmos_fail(error, ROTMOS_NOMEM, "router_route",
                             "cannot grow route registry");
        goto failed;
    }
    router->routes = grown;
    router->routes[router->count++] = route;
    if (out != NULL)
        *out = route;
    rotmos_error_clear(error);
    return ROTMOS_OK;
failed:
    if (route->has_pattern)
        regfree(&route->pattern);
    free((void *)route->config.path);
    free(route);
    return status;
}

rotmos_status rotmos_router_route(rotmos_router *router,
                                  const rotmos_route_config *config,
                                  rotmos_route **out, rotmos_error *error)
{
    return route_add(router, config, NULL, out, error);
}

rotmos_status rotmos_router_group(rotmos_router *router,
                                  const rotmos_route_group_config *config,
                                  rotmos_route_group **out, rotmos_error *error)
{
    rotmos_route_group_config defaults;
    rotmos_route_group *group;
    rotmos_status status;
    const char *prefix;
    char *copy;
    size_t length;
    if (out != NULL)
        *out = NULL;
    if (router == NULL || out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "router_group",
                           "router and group output are required");
    if (router->frozen)
        return rotmos_fail(error, ROTMOS_STATE, "router_group",
                           "group registration is unavailable during a run");
    rotmos_route_group_config_init(&defaults);
    if (config == NULL)
        config = &defaults;
    prefix = config->prefix != NULL ? config->prefix : "";
    copy = rotmos_copy_text(prefix);
    if (copy == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "router_group",
                           "cannot copy group prefix");
    length = strlen(copy);
    if (length > 1 && copy[length - 1] == '/')
        copy[length - 1] = '\0';
    if (*copy != '\0') {
        status = rotmos_route_path_validate(copy, ROTMOS_PATH_PARAMS, error);
        if (status != ROTMOS_OK) {
            free(copy);
            return status;
        }
    }
    group = malloc(sizeof(*group));
    if (group == NULL) {
        free(copy);
        return rotmos_fail(error, ROTMOS_NOMEM, "router_group",
                           "cannot allocate group");
    }
    group->config = *config;
    group->config.prefix = copy;
    group->router = router;
    group->next = router->groups;
    router->groups = group;
    *out = group;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_route_group_route(rotmos_route_group *group,
                                       const rotmos_route_config *config,
                                       rotmos_route **out, rotmos_error *error)
{
    rotmos_route_config joined;
    char *path;
    char *target;
    const char *p;
    const char *prefix;
    size_t prefix_length;
    size_t path_length;
    size_t escaped;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (group == NULL || config == NULL || config->path == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "group_route",
                           "group and route declaration are required");
    if (group->router->frozen)
        return rotmos_fail(error, ROTMOS_STATE, "group_route",
                           "route registration is unavailable during a run");
    status = rotmos_route_path_validate(config->path, config->path_kind, error);
    if (status != ROTMOS_OK)
        return status;
    prefix = group->config.prefix;
    if (*prefix == '\0' || strcmp(prefix, "/") == 0)
        return route_add(group->router, config, group, out, error);
    prefix_length = strlen(prefix);
    path_length = strlen(config->path);
    escaped = prefix_length;
    if (config->path_kind == ROTMOS_PATH_REGEX) {
        for (p = prefix; *p != '\0'; ++p) {
            if (strchr(".+*?^$()[]{}|\\", *p) != NULL) {
                if (escaped == (size_t)-1)
                    return rotmos_fail(error, ROTMOS_OVERFLOW, "group_route",
                                       "escaped prefix size overflow");
                ++escaped;
            }
        }
    }
    if (path_length == (size_t)-1 || escaped > (size_t)-1 - path_length - 1)
        return rotmos_fail(error, ROTMOS_OVERFLOW, "group_route",
                           "prefixed path size overflow");
    path = malloc(escaped + path_length + 1);
    if (path == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "group_route",
                           "cannot allocate prefixed path");
    target = path;
    if (config->path_kind == ROTMOS_PATH_REGEX)
        *target++ = '^';
    for (p = prefix; *p != '\0'; ++p) {
        if (config->path_kind == ROTMOS_PATH_REGEX &&
            strchr(".+*?^$()[]{}|\\", *p) != NULL)
            *target++ = '\\';
        *target++ = *p;
    }
    memcpy(target,
           config->path + (config->path_kind == ROTMOS_PATH_REGEX ? 1 : 0),
           path_length + 1 - (config->path_kind == ROTMOS_PATH_REGEX ? 1 : 0));
    joined = *config;
    joined.path = path;
    if (joined.path_kind != ROTMOS_PATH_REGEX && strchr(path, ':') != NULL)
        joined.path_kind = ROTMOS_PATH_PARAMS;
    status = route_add(group->router, &joined, group, out, error);
    free(path);
    return status;
}

size_t rotmos_router_route_count(const rotmos_router *router)
{
    return router != NULL ? router->count : 0;
}

rotmos_status rotmos_router_route_at(const rotmos_router *router, size_t index,
                                     const rotmos_route **out,
                                     rotmos_error *error)
{
    if (out != NULL)
        *out = NULL;
    if (router == NULL || out == NULL || index >= router->count)
        return rotmos_fail(error, ROTMOS_INVALID, "router_route_at",
                           "route index is outside the registry");
    *out = router->routes[index];
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

const rotmos_route_config *rotmos_route_declaration(const rotmos_route *route)
{
    return route != NULL ? &route->config : NULL;
}
