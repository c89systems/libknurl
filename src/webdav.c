#include "webdav_internal.h"
#include <stdlib.h>
#include <string.h>

void knurl_webdav_auth_response_init(knurl_webdav_auth_response *response)
{
    if (response != NULL)
        memset(response, 0, sizeof(*response));
}
void knurl_webdav_config_init(knurl_webdav_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->path_prefix = "/";
    config->operation_timeout_ms = 30000;
    knurl_storage_filesystem_config_init(&config->storage);
}
knurl_bytes knurl_webdav_request_principal(const knurl_request *request)
{
    knurl_bytes bytes = {0};
    if (request != NULL && request->route != NULL &&
        request->route->config.on_headers == knurl_dav_headers &&
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
        knurl_storage_provider_close(r->provider, NULL);
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
    if (n > KNURL_ASSET_PATH_MAX)
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
static knurl_status add(knurl_router *router, knurl_route_group *group,
                        const knurl_webdav_config *config, knurl_route **out,
                        knurl_error *error)
{
    struct dav_route *r;
    knurl_route_config route;
    knurl_route *registered;
    knurl_status status;
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
        !knurl_u64_valid(config->max_body_bytes) ||
        (config->content_type != NULL &&
         !knurl_content_type_valid(config->content_type)) ||
        (config->cache_control != NULL &&
         !knurl_header_value_valid(config->cache_control)))
        return knurl_fail(error, KNURL_INVALID, "webdav",
                          "provide router, valid prefix/policy and auth for a "
                          "protected mount");
    prefix = config->path_prefix;
    n = strlen(prefix);
    if (prefix[0] != '/')
        return knurl_fail(error, KNURL_INVALID, "webdav",
                          "mount prefix must be absolute");
    while (n > 1 && prefix[n - 1] == '/')
        --n;
    group_prefix = group != NULL ? group->config.prefix : "";
    g = strcmp(group_prefix, "/") == 0 ? 0 : strlen(group_prefix);
    if (n == 1 && g != 0)
        n = 0;
    if (n > KNURL_ASSET_PATH_MAX || g > KNURL_ASSET_PATH_MAX - n)
        return knurl_fail(error, KNURL_LIMIT, "webdav",
                          "mount prefix exceeds path bound");
    r = calloc(1, sizeof(*r));
    if (r == NULL)
        return KNURL_NOMEM;
    r->config = *config;
    r->config.content_type = NULL;
    r->config.cache_control = NULL;
    r->prefix = malloc(g + n + 1);
    if (r->prefix == NULL) {
        release(r);
        return KNURL_NOMEM;
    }
    memcpy(r->prefix, group_prefix, g);
    memcpy(r->prefix + g, prefix, n);
    r->prefix[g + n] = 0;
    status = knurl_route_path_validate(r->prefix, KNURL_PATH_LITERAL, error);
    if (status != KNURL_OK) {
        release(r);
        return status;
    }
    r->config.content_type = knurl_copy_text(config->content_type);
    r->config.cache_control = knurl_copy_text(config->cache_control);
    if ((config->content_type != NULL && r->config.content_type == NULL) ||
        (config->cache_control != NULL && r->config.cache_control == NULL)) {
        release(r);
        return KNURL_NOMEM;
    }
    if (config->provider != NULL)
        r->provider = config->provider;
    else {
        status = knurl_storage_filesystem_new(router->server, &config->storage,
                                              &r->provider, error);
        if (status != KNURL_OK) {
            release(r);
            return status;
        }
        r->own_provider = 1;
    }
    r->capabilities = knurl_storage_provider_capabilities(r->provider);
    if (!config->auth_disabled && (r->capabilities & KNURL_STORAGE_CAN_WRITE) &&
        !(r->capabilities & KNURL_STORAGE_CAN_SCOPE_CHECK)) {
        release(r);
        return knurl_fail(
            error, KNURL_NOT_SUPPORTED, "webdav",
            "protected mutable provider requires atomic mutation scope checks");
    }
    regex = pattern(r->prefix);
    if (regex == NULL) {
        release(r);
        return KNURL_NOMEM;
    }
    knurl_route_config_init(&route);
    route.path = regex;
    route.path_kind = KNURL_PATH_REGEX;
    route.methods = KNURL_HTTP_METHODS_ALL;
    route.on_headers = knurl_dav_headers;
    route.userdata = r;
    status = knurl_router_route(router, &route, &registered, error);
    free(regex);
    if (status != KNURL_OK) {
        release(r);
        return status;
    }
    registered->group = group;
    registered->config.hooks = config->hooks;
    registered->directory_mount = 1;
    registered->release_userdata = release;
    if (out != NULL)
        *out = registered;
    return KNURL_OK;
}
knurl_status knurl_router_webdav(knurl_router *router,
                                 const knurl_webdav_config *config,
                                 knurl_route **out, knurl_error *error)
{
    return add(router, NULL, config, out, error);
}
knurl_status knurl_route_group_webdav(knurl_route_group *group,
                                      const knurl_webdav_config *config,
                                      knurl_route **out, knurl_error *error)
{
    return add(NULL, group, config, out, error);
}
