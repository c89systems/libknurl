#include <knurl/static.h>
#include <knurl/request.h>
#include "static_internal.h"
#include "router_internal.h"
#include "request_internal.h"
#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct static_route {
    knurl_static_config config;
    knurl_asset_provider *provider;
    char *prefix, *file;
    int own_provider, custom, directory;
};
void knurl_static_config_init(knurl_static_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}
static knurl_status not_found(struct static_route *r, knurl_reply *reply,
                              knurl_error *error)
{
    const char *body = r->config.not_found_body;
    knurl_status status;
    if (body == NULL && !r->custom)
        return knurl_reply_status(reply, 404, error);
    status = knurl_reply_header_set(reply, "cache-control", "no-store", error);
    if (status != KNURL_OK)
        return status;
    return knurl_reply_text(reply, 404,
                            r->config.not_found_content_type != NULL
                                ? r->config.not_found_content_type
                                : "text/plain; charset=utf-8",
                            body != NULL ? body : "not found\n", error);
}
/* Canonical redirects preserve the raw query, encoding the decoded path once.
 */
static knurl_status redirect(knurl_request *request, knurl_reply *reply,
                             knurl_error *error)
{
    static const char hex[] = "0123456789ABCDEF";
    knurl_bytes path = knurl_request_path(request),
                query = knurl_request_query(request);
    const unsigned char *p = path.data;
    char *location, *out;
    size_t n, capacity;
    knurl_status status;
    if (path.length > ((size_t)-1 - query.length - 3) / 3)
        return knurl_fail(error, KNURL_OVERFLOW, "static_redirect",
                          "redirect path is too large");
    capacity = path.length * 3 + query.length + 3;
    location = malloc(capacity);
    if (location == NULL)
        return knurl_fail(error, KNURL_NOMEM, "static_redirect",
                          "allocating redirect");
    out = location;
    for (n = 0; n < path.length; ++n) {
        unsigned char c = p[n];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || strchr("-._~/", c) != NULL)
            *out++ = (char)c;
        else {
            *out++ = '%';
            *out++ = hex[c >> 4];
            *out++ = hex[c & 15];
        }
    }
    *out++ = '/';
    if (query.length != 0) {
        *out++ = '?';
        memcpy(out, query.data, query.length);
        out += query.length;
    }
    *out = 0;
    status = knurl_reply_header_set(reply, "location", location, error);
    free(location);
    return status == KNURL_OK ? knurl_reply_status(reply, 308, error) : status;
}
static knurl_status serve(knurl_request *request, knurl_reply *reply,
                          void *data, knurl_error *error)
{
    struct static_route *r = data;
    const char *path = knurl_request_path(request).data, *relative;
    char *copy, *index;
    knurl_asset *asset = NULL, *selected = NULL;
    const knurl_asset_info *info;
    size_t n, prefix, index_length;
    knurl_status status;
    int trailing;
    if (!r->directory) {
        status = knurl_asset_provider_open(r->provider, r->file, &asset, error);
        if (status == KNURL_EOF)
            return not_found(r, reply, error);
        if (status != KNURL_OK)
            return status;
        if (knurl_asset_stat(asset)->kind != KNURL_ASSET_FILE) {
            knurl_asset_close(asset, NULL);
            return not_found(r, reply, error);
        }
        return knurl_static_reply_asset(&r->config, r->custom, request, reply,
                                        asset, r->file, error);
    }
    prefix = strlen(r->prefix);
    relative = strcmp(r->prefix, "/") == 0 ? path : path + prefix;
    if (*relative == 0)
        relative = "/";
    n = strlen(relative);
    trailing = n > 1 && relative[n - 1] == '/';
    copy = knurl_copy_text(relative);
    if (copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "static",
                          "copying asset lookup path");
    if (trailing)
        copy[--n] = 0;
    status = knurl_asset_provider_open(r->provider, copy, &asset, error);
    if (status == KNURL_INVALID || status == KNURL_EOF) {
        free(copy);
        return not_found(r, reply, error);
    }
    if (status != KNURL_OK) {
        free(copy);
        return status;
    }
    info = knurl_asset_stat(asset);
    if (info->kind == KNURL_ASSET_FILE) {
        if (trailing) {
            free(copy);
            knurl_asset_close(asset, NULL);
            return not_found(r, reply, error);
        }
        if (strcmp(copy, "/") == 0 && strcmp(r->prefix, "/") != 0 &&
            path[strlen(path) - 1] != '/') {
            free(copy);
            knurl_asset_close(asset, NULL);
            return redirect(request, reply, error);
        }
        status = knurl_static_reply_asset(&r->config, r->custom, request, reply,
                                          asset, copy, error);
        free(copy);
        return status;
    }
    if (!r->config.index_enabled) {
        free(copy);
        knurl_asset_close(asset, NULL);
        return not_found(r, reply, error);
    }
    index_length = strlen(r->config.index_file);
    index = malloc(n + index_length + 2);
    if (index == NULL) {
        free(copy);
        knurl_asset_close(asset, NULL);
        return knurl_fail(error, KNURL_NOMEM, "static",
                          "allocating index lookup path");
    }
    memcpy(index, copy, n);
    if (n == 1) {
        memcpy(index + 1, r->config.index_file, index_length + 1);
    } else {
        index[n] = '/';
        memcpy(index + n + 1, r->config.index_file, index_length + 1);
    }
    free(copy);
    status = knurl_asset_provider_open(r->provider, index, &selected, error);
    knurl_asset_close(asset, NULL);
    if (status != KNURL_OK) {
        free(index);
        return status == KNURL_EOF || status == KNURL_INVALID
                   ? not_found(r, reply, error)
                   : status;
    }
    if (knurl_asset_stat(selected)->kind != KNURL_ASSET_FILE) {
        free(index);
        knurl_asset_close(selected, NULL);
        return not_found(r, reply, error);
    }
    if (strlen(path) > 1 && path[strlen(path) - 1] != '/') {
        knurl_asset_close(selected, NULL);
        free(index);
        return redirect(request, reply, error);
    }
    status = knurl_static_reply_asset(&r->config, r->custom, request, reply,
                                      selected, index, error);
    free(index);
    return status;
}
static knurl_status preflight(knurl_request *request, knurl_request_plan *plan,
                              void *data, knurl_error *error)
{
    struct static_route *r = data;
    const char *allow;
    knurl_response *response;
    knurl_status status;
    knurl_u64 zero;
    if ((r->config.methods &
         KNURL_HTTP_METHOD_MASK(knurl_request_method(request))) != 0)
        return KNURL_OK;
    allow =
        r->config.methods == (KNURL_HTTP_METHODS_GET | KNURL_HTTP_METHODS_HEAD)
            ? "GET, HEAD"
        : r->config.methods == KNURL_HTTP_METHODS_GET ? "GET"
                                                      : "HEAD";
    response = knurl_request_response(request);
    zero.high = zero.low = 0;
    status = knurl_response_status(response, 405, error);
    if (status == KNURL_OK)
        status = knurl_response_header_set(response, "allow", allow, error);
    if (status == KNURL_OK)
        status = knurl_response_known_length(response, zero, error);
    if (status == KNURL_OK)
        status = knurl_response_begin(response, error);
    if (status == KNURL_OK)
        status = knurl_response_end(response, NULL, 0, error);
    if (status == KNURL_OK)
        plan->disposition = KNURL_REQUEST_REPLY;
    return status;
}
static void release_route(void *data)
{
    struct static_route *r = data;
    if (r->own_provider)
        knurl_asset_provider_close(r->provider, NULL);
    free(r->prefix);
    free(r->file);
    free((void *)r->config.content_type);
    knurl_mime_free(r->config.mime_types, r->config.mime_type_count);
    free((void *)r->config.cache_control);
    free((void *)r->config.index_file);
    free((void *)r->config.not_found_body);
    free((void *)r->config.not_found_content_type);
    free(r);
}
static char *mount_regex(const char *prefix)
{
    const char *p;
    char *regex, *out;
    size_t n = strlen(prefix), extra = 0;
    for (p = prefix; *p != 0; ++p)
        if (strchr(".+*?^$()[]{}|\\", *p) != NULL)
            ++extra;
    regex = malloc(n + extra + 10);
    if (regex == NULL)
        return NULL;
    out = regex;
    *out++ = '^';
    for (p = prefix; *p != 0; ++p) {
        if (strchr(".+*?^$()[]{}|\\", *p) != NULL)
            *out++ = '\\';
        *out++ = *p;
    }
    strcpy(out, strcmp(prefix, "/") == 0 ? ".*$" : "(/.*)?$");
    return regex;
}
static knurl_status add_route(knurl_router *router, knurl_route_group *group,
                              const knurl_static_config *config, int kind,
                              knurl_route **out, knurl_error *error)
{
    struct static_route *r;
    knurl_route_config route;
    knurl_route *registered;
    knurl_status status;
    char *path, *pattern, *root = NULL;
    const char *slash, *file;
    size_t n, group_length = 0;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || (router == NULL && group == NULL) ||
        config->path == NULL)
        return knurl_fail(error, KNURL_INVALID, "static_route",
                          "provide router, static declaration and path");
    if ((config->methods &
         ~(KNURL_HTTP_METHODS_GET | KNURL_HTTP_METHODS_HEAD)) != 0 ||
        (config->content_type != NULL &&
         !knurl_content_type_valid(config->content_type)) ||
        (config->cache_control != NULL &&
         !knurl_header_value_valid(config->cache_control)) ||
        (config->not_found_content_type != NULL &&
         !knurl_content_type_valid(config->not_found_content_type)))
        return knurl_fail(
            error, KNURL_INVALID, "static_route",
            "static routes accept GET/HEAD and valid content/cache headers");
    path = knurl_copy_text(config->path);
    if (path == NULL)
        return knurl_fail(error, KNURL_NOMEM, "static_route",
                          "copying static path");
    n = strlen(path);
    if (kind != 0)
        while (n > 1 && path[n - 1] == '/')
            path[--n] = 0;
    status = knurl_route_path_validate(path, KNURL_PATH_LITERAL, error);
    if (status != KNURL_OK) {
        free(path);
        return status;
    }
    if (group != NULL) {
        if (group->config.prefix[0] != 0) {
            status = knurl_route_path_validate(group->config.prefix,
                                               KNURL_PATH_LITERAL, error);
            if (status != KNURL_OK) {
                free(path);
                return status;
            }
        }
        group_length = strcmp(group->config.prefix, "/") == 0
                           ? 0
                           : strlen(group->config.prefix);
    }
    if (n > KNURL_ASSET_PATH_MAX || group_length > KNURL_ASSET_PATH_MAX - n) {
        free(path);
        return knurl_fail(error, KNURL_LIMIT, "static_route",
                          "static prefix exceeds asset path bound");
    }
    r = calloc(1, sizeof(*r));
    if (r == NULL) {
        free(path);
        return knurl_fail(error, KNURL_NOMEM, "static_route",
                          "allocating static route");
    }
    r->directory = kind != 0;
    r->custom = kind == 2;
    r->config.methods = config->methods
                            ? config->methods
                            : KNURL_HTTP_METHODS_GET | KNURL_HTTP_METHODS_HEAD;
    r->config.index_enabled = config->index_enabled;
    status = knurl_mime_copy(config->mime_types, config->mime_type_count,
                             &r->config.mime_types, error);
    if (status != KNURL_OK) {
        free(path);
        release_route(r);
        return status;
    }
    r->config.mime_type_count = config->mime_type_count;
    r->prefix = malloc(group_length + n + 1);
    if (r->prefix != NULL) {
        if (group_length)
            memcpy(r->prefix, group->config.prefix, group_length);
        memcpy(r->prefix + group_length, path, n + 1);
        if (group_length && n == 1)
            r->prefix[group_length] = 0;
    }
    r->config.content_type = knurl_copy_text(config->content_type);
    r->config.cache_control = knurl_copy_text(config->cache_control);
    r->config.index_file = knurl_copy_text(
        config->index_file != NULL ? config->index_file : "index.html");
    r->config.not_found_body = knurl_copy_text(config->not_found_body);
    r->config.not_found_content_type =
        knurl_copy_text(config->not_found_content_type);
    if (r->prefix == NULL ||
        (config->content_type != NULL && r->config.content_type == NULL) ||
        (config->cache_control != NULL && r->config.cache_control == NULL) ||
        r->config.index_file == NULL ||
        (config->not_found_body != NULL && r->config.not_found_body == NULL) ||
        (config->not_found_content_type != NULL &&
         r->config.not_found_content_type == NULL)) {
        free(path);
        release_route(r);
        return knurl_fail(error, KNURL_NOMEM, "static_route",
                          "copying static options");
    }
    if (strchr(r->config.index_file, '/') != NULL ||
        r->config.index_file[0] == 0 ||
        strcmp(r->config.index_file, ".") == 0 ||
        strcmp(r->config.index_file, "..") == 0 ||
        strchr(r->config.index_file, '%') != NULL ||
        strchr(r->config.index_file, '\\') != NULL) {
        free(path);
        release_route(r);
        return knurl_fail(error, KNURL_INVALID, "static_route",
                          "index_file must be one safe filename");
    }
    if (kind == 2) {
        r->provider = config->provider;
        status = r->provider != NULL
                     ? KNURL_OK
                     : knurl_fail(error, KNURL_INVALID, "static_route",
                                  "custom asset provider is required");
    } else {
        if (kind == 0) {
            file = config->file_path;
            if (file == NULL || file[0] == 0) {
                free(path);
                release_route(r);
                return knurl_fail(error, KNURL_INVALID, "static_route",
                                  "file_path is required");
            }
            slash = strrchr(file, '/');
            root = slash != NULL ? malloc((size_t)(slash - file) + 2)
                                 : knurl_copy_text(".");
            if (root != NULL && slash != NULL) {
                size_t length = (size_t)(slash - file);
                if (length == 0)
                    root[0] = '/';
                else
                    memcpy(root, file, length);
                root[length ? length : 1] = 0;
            }
            file = slash != NULL ? slash + 1 : file;
            r->file = malloc(strlen(file) + 2);
            if (r->file != NULL) {
                r->file[0] = '/';
                strcpy(r->file + 1, file);
            }
        } else
            root = knurl_copy_text(config->root_dir);
        if (root == NULL || (kind == 0 && r->file == NULL)) {
            free(root);
            free(path);
            release_route(r);
            return knurl_fail(
                error,
                config->root_dir == NULL && kind == 1 ? KNURL_INVALID
                                                      : KNURL_NOMEM,
                "static_route", "provide root_dir or a valid file path");
        }
        if (kind == 0 && !knurl_asset_path_valid(r->file)) {
            free(root);
            free(path);
            release_route(r);
            return knurl_fail(error, KNURL_INVALID, "static_route",
                              "file_path must select a safe basename");
        }
        status = knurl_asset_filesystem_new(root, &r->provider, error);
        free(root);
        r->own_provider = status == KNURL_OK;
    }
    if (status != KNURL_OK) {
        free(path);
        release_route(r);
        return status;
    }
    pattern = kind == 0 ? knurl_copy_text(r->prefix) : mount_regex(r->prefix);
    free(path);
    if (pattern == NULL) {
        release_route(r);
        return knurl_fail(error, KNURL_NOMEM, "static_route",
                          "allocating mount matcher");
    }
    knurl_route_config_init(&route);
    route.path = pattern;
    route.path_kind = kind == 0 ? KNURL_PATH_LITERAL : KNURL_PATH_REGEX;
    route.methods = kind == 0 ? r->config.methods : KNURL_HTTP_METHODS_ALL;
    route.handler = serve;
    route.userdata = r;
    route.hooks = config->hooks;
    status = knurl_router_route(group != NULL ? group->router : router, &route,
                                &registered, error);
    free(pattern);
    if (status != KNURL_OK)
        release_route(r);
    else {
        registered->group = group;
        registered->release_userdata = release_route;
        registered->preflight = preflight;
        registered->directory_mount = kind != 0;
        if (out != NULL)
            *out = registered;
    }
    return status;
}
knurl_status knurl_router_static_file(knurl_router *router,
                                      const knurl_static_config *config,
                                      knurl_route **out, knurl_error *error)
{
    return add_route(router, NULL, config, 0, out, error);
}
knurl_status knurl_router_static_directory(knurl_router *router,
                                           const knurl_static_config *config,
                                           knurl_route **out,
                                           knurl_error *error)
{
    return add_route(router, NULL, config, 1, out, error);
}
knurl_status knurl_router_static_assets(knurl_router *router,
                                        const knurl_static_config *config,
                                        knurl_route **out, knurl_error *error)
{
    return add_route(router, NULL, config, 2, out, error);
}
knurl_status knurl_route_group_static_file(knurl_route_group *group,
                                           const knurl_static_config *config,
                                           knurl_route **out,
                                           knurl_error *error)
{
    return add_route(NULL, group, config, 0, out, error);
}
knurl_status
knurl_route_group_static_directory(knurl_route_group *group,
                                   const knurl_static_config *config,
                                   knurl_route **out, knurl_error *error)
{
    return add_route(NULL, group, config, 1, out, error);
}
knurl_status knurl_route_group_static_assets(knurl_route_group *group,
                                             const knurl_static_config *config,
                                             knurl_route **out,
                                             knurl_error *error)
{
    return add_route(NULL, group, config, 2, out, error);
}
