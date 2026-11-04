#include "webdav_internal.h"
#include <stdlib.h>
#include <string.h>

void rotmos_webdav_auth_response_init(rotmos_webdav_auth_response *response)
{
    if (response != NULL)
        memset(response, 0, sizeof(*response));
}
void rotmos_webdav_config_init(rotmos_webdav_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->path_prefix = "/";
    config->operation_timeout_ms = 30000;
    rotmos_storage_filesystem_config_init(&config->storage);
}
rotmos_bytes rotmos_webdav_request_principal(const rotmos_request *request)
{
    rotmos_bytes bytes = {0};
    if (request != NULL && request->route != NULL &&
        request->route->config.on_headers == rotmos_dav_headers &&
        request->codec_data != NULL) {
        const struct dav_request *s = request->codec_data;
        bytes.data = s->principal;
        bytes.length = strlen(s->principal);
    }
    return bytes;
}
static void release(void *data)
{
    struct dav_route *r = data;
    if (r->own_provider)
        rotmos_storage_provider_close(r->provider, NULL);
    free(r->prefix);
    free((void *)r->config.content_type);
    free((void *)r->config.cache_control);
    free(r);
}
static char *pattern(const char *prefix)
{
    const char *p;
    char *text, *out;
    size_t n = strlen(prefix);
    if (n > ROTMOS_ASSET_PATH_MAX)
        return NULL;
    text = malloc(n * 2 + 10);
    if (text == NULL)
        return NULL;
    out = text;
    *out++ = '^';
    for (p = prefix; *p != 0; ++p) {
        if (strchr(".+*?^$()[]{}|\\", *p) != NULL)
            *out++ = '\\';
        *out++ = *p;
    }
    strcpy(out, strcmp(prefix, "/") == 0 ? ".*$" : "(/.*)?$");
    return text;
}
static rotmos_status add(rotmos_router *router, rotmos_route_group *group,
                         const rotmos_webdav_config *config, rotmos_route **out,
                         rotmos_error *error)
{
    struct dav_route *r;
    rotmos_route_config route;
    rotmos_route *registered;
    rotmos_status status;
    const char *prefix, *group_prefix;
    char *regex;
    size_t n, g;
    if (out != NULL)
        *out = NULL;
    if (group != NULL)
        router = group->router;
    if (router == NULL || config == NULL || config->path_prefix == NULL ||
        (!config->auth_disabled && config->auth == NULL) ||
        config->operation_timeout_ms < 0 ||
        !rotmos_u64_valid(config->max_body_bytes) ||
        (config->content_type != NULL &&
         (!rotmos_header_value_valid(config->content_type) ||
          config->content_type[0] == 0)) ||
        (config->cache_control != NULL &&
         !rotmos_header_value_valid(config->cache_control)))
        return rotmos_fail(error, ROTMOS_INVALID, "webdav",
                           "provide router, valid prefix/policy and auth for a "
                           "protected mount");
    prefix = config->path_prefix;
    n = strlen(prefix);
    if (prefix[0] != '/')
        return rotmos_fail(error, ROTMOS_INVALID, "webdav",
                           "mount prefix must be absolute");
    while (n > 1 && prefix[n - 1] == '/')
        --n;
    group_prefix = group != NULL ? group->config.prefix : "";
    g = strcmp(group_prefix, "/") == 0 ? 0 : strlen(group_prefix);
    if (n == 1 && g != 0)
        n = 0;
    if (n > ROTMOS_ASSET_PATH_MAX || g > ROTMOS_ASSET_PATH_MAX - n)
        return rotmos_fail(error, ROTMOS_LIMIT, "webdav",
                           "mount prefix exceeds path bound");
    r = calloc(1, sizeof(*r));
    if (r == NULL)
        return ROTMOS_NOMEM;
    r->config = *config;
    r->config.content_type = NULL;
    r->config.cache_control = NULL;
    r->prefix = malloc(g + n + 1);
    if (r->prefix == NULL) {
        release(r);
        return ROTMOS_NOMEM;
    }
    memcpy(r->prefix, group_prefix, g);
    memcpy(r->prefix + g, prefix, n);
    r->prefix[g + n] = 0;
    status = rotmos_route_path_validate(r->prefix, ROTMOS_PATH_LITERAL, error);
    if (status != ROTMOS_OK) {
        release(r);
        return status;
    }
    r->config.content_type = rotmos_copy_text(config->content_type);
    r->config.cache_control = rotmos_copy_text(config->cache_control);
    if ((config->content_type != NULL && r->config.content_type == NULL) ||
        (config->cache_control != NULL && r->config.cache_control == NULL)) {
        release(r);
        return ROTMOS_NOMEM;
    }
    if (config->provider != NULL)
        r->provider = config->provider;
    else {
        status = rotmos_storage_filesystem_new(router->server, &config->storage,
                                               &r->provider, error);
        if (status != ROTMOS_OK) {
            release(r);
            return status;
        }
        r->own_provider = 1;
    }
    r->capabilities = rotmos_storage_provider_capabilities(r->provider);
    if (!config->auth_disabled &&
        (r->capabilities & ROTMOS_STORAGE_CAN_WRITE) &&
        !(r->capabilities & ROTMOS_STORAGE_CAN_SCOPE_CHECK)) {
        release(r);
        return rotmos_fail(
            error, ROTMOS_NOT_SUPPORTED, "webdav",
            "protected mutable provider requires atomic mutation scope checks");
    }
    regex = pattern(r->prefix);
    if (regex == NULL) {
        release(r);
        return ROTMOS_NOMEM;
    }
    rotmos_route_config_init(&route);
    route.path = regex;
    route.path_kind = ROTMOS_PATH_REGEX;
    route.methods = ROTMOS_HTTP_METHODS_ALL;
    route.on_headers = rotmos_dav_headers;
    route.userdata = r;
    status = rotmos_router_route(router, &route, &registered, error);
    free(regex);
    if (status != ROTMOS_OK) {
        release(r);
        return status;
    }
    registered->group = group;
    registered->config.hooks = config->hooks;
    registered->directory_mount = 1;
    registered->release_userdata = release;
    if (out != NULL)
        *out = registered;
    return ROTMOS_OK;
}
rotmos_status rotmos_router_webdav(rotmos_router *router,
                                   const rotmos_webdav_config *config,
                                   rotmos_route **out, rotmos_error *error)
{
    return add(router, NULL, config, out, error);
}
rotmos_status rotmos_route_group_webdav(rotmos_route_group *group,
                                        const rotmos_webdav_config *config,
                                        rotmos_route **out, rotmos_error *error)
{
    return add(NULL, group, config, out, error);
}
