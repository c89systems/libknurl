#include <rotmos/static.h>
#include <rotmos/request.h>
#include "static_internal.h"
#include "router_internal.h"
#include "request_internal.h"
#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct static_route {
    rotmos_static_config config;
    rotmos_asset_provider *provider;
    char *prefix, *file;
    int own_provider, custom, directory;
};
void rotmos_static_config_init(rotmos_static_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}
static const char *header(rotmos_request *request, const char *name)
{
    rotmos_bytes bytes;
    return rotmos_request_header_first(request, name, &bytes, NULL) == ROTMOS_OK
               ? (const char *)bytes.data
               : NULL;
}
static rotmos_status not_found(struct static_route *r, rotmos_reply *reply,
                               rotmos_error *error)
{
    const char *body = r->config.not_found_body;
    rotmos_status status;
    if (body == NULL && !r->custom)
        return rotmos_reply_status(reply, 404, error);
    status = rotmos_reply_header_set(reply, "cache-control", "no-store", error);
    if (status != ROTMOS_OK)
        return status;
    return rotmos_reply_text(reply, 404,
                             r->config.not_found_content_type != NULL
                                 ? r->config.not_found_content_type
                                 : "text/plain; charset=utf-8",
                             body != NULL ? body : "not found\n", error);
}
/* Canonical redirects preserve the raw query, encoding the decoded path once.
 */
static rotmos_status redirect(rotmos_request *request, rotmos_reply *reply,
                              rotmos_error *error)
{
    static const char hex[] = "0123456789ABCDEF";
    rotmos_bytes path = rotmos_request_path(request),
                 query = rotmos_request_query(request);
    const unsigned char *p = path.data;
    char *location, *out;
    size_t n, capacity;
    rotmos_status status;
    if (path.length > ((size_t)-1 - query.length - 3) / 3)
        return rotmos_fail(error, ROTMOS_OVERFLOW, "static_redirect",
                           "redirect path is too large");
    capacity = path.length * 3 + query.length + 3;
    location = malloc(capacity);
    if (location == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "static_redirect",
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
    status = rotmos_reply_header_set(reply, "location", location, error);
    free(location);
    return status == ROTMOS_OK ? rotmos_reply_status(reply, 308, error)
                               : status;
}
static rotmos_status send_asset(struct static_route *r, rotmos_request *request,
                                rotmos_reply *reply, rotmos_asset *asset,
                                const char *path, rotmos_error *error)
{
    const rotmos_asset_info *info = rotmos_asset_stat(asset);
    struct rotmos_static_range range;
    rotmos_u64 length, start, end, one;
    const char *type, *value, *range_input;
    char total[21], first[21], last[21], content_range[80];
    size_t written;
    rotmos_source *source;
    rotmos_status status = ROTMOS_OK;
    int match, code = 200;
    type = r->custom && info->content_type != NULL ? info->content_type
                                                   : r->config.content_type;
    if (r->custom && (type == NULL || type[0] == 0))
        type = "application/octet-stream";
    else if (type == NULL)
        type = info->content_type != NULL ? info->content_type
                                          : rotmos_static_content_type(path);
    if (r->config.cache_control != NULL || r->custom)
        status = rotmos_reply_header_set(reply, "cache-control",
                                         r->config.cache_control != NULL
                                             ? r->config.cache_control
                                             : "no-cache",
                                         error);
    if (status == ROTMOS_OK && info->etag != NULL && info->etag[0] != 0)
        status = rotmos_reply_header_set(reply, "etag", info->etag, error);
    if (status == ROTMOS_OK)
        status =
            rotmos_reply_header_set(reply, "accept-ranges", "bytes", error);
    if (status == ROTMOS_OK && info->last_modified != NULL)
        status = rotmos_reply_header_set(reply, "last-modified",
                                         info->last_modified, error);
    value = header(request, "if-match");
    if (status == ROTMOS_OK && value != NULL) {
        match = rotmos_http_tag_matches(value, info->etag, 1, 1);
        if (match != 1) {
            code = match < 0 ? 400 : 412;
            goto empty;
        }
    }
    value = header(request, "if-none-match");
    if (status == ROTMOS_OK && value != NULL) {
        match = rotmos_http_tag_matches(value, info->etag, 1, 0);
        if (match != 0) {
            code = match < 0 ? 400 : 304;
            goto empty;
        }
    }
    if (status == ROTMOS_OK && value == NULL && info->last_modified != NULL) {
        const char *since = header(request, "if-modified-since");
        if (since != NULL && curl_getdate(since, NULL) != (time_t)-1 &&
            curl_getdate(since, NULL) ==
                curl_getdate(info->last_modified, NULL)) {
            code = 304;
            goto empty;
        }
    }
    if (status != ROTMOS_OK)
        goto done;
    range_input = rotmos_request_method(request) == ROTMOS_HTTP_GET
                      ? header(request, "range")
                      : NULL;
    value = header(request, "if-range");
    if (range_input != NULL && value != NULL &&
        !rotmos_static_if_range(value, info->etag))
        range_input = NULL;
    rotmos_static_range_parse(range_input, info->size, &range);
    rotmos_u64_format_decimal(info->size, total, sizeof(total), &written, NULL);
    if (range.requested) {
        if (!range.satisfiable) {
            snprintf(content_range, sizeof(content_range), "bytes */%s", total);
            code = 416;
        } else {
            one.high = 0;
            one.low = 1;
            rotmos_u64_add(range.start, range.length, &end, NULL);
            end = rotmos_asset_subtract(end, one);
            rotmos_u64_format_decimal(range.start, first, sizeof(first),
                                      &written, NULL);
            rotmos_u64_format_decimal(end, last, sizeof(last), &written, NULL);
            snprintf(content_range, sizeof(content_range), "bytes %s-%s/%s",
                     first, last, total);
            code = 206;
        }
        status = rotmos_reply_header_set(reply, "content-range", content_range,
                                         error);
        if (status != ROTMOS_OK || code == 416)
            goto empty;
    }
    start.high = start.low = 0;
    length = info->size;
    if (range.requested) {
        start = range.start;
        length = range.length;
    }
    status = rotmos_asset_source_take(asset, start, length, &source, error);
    if (status != ROTMOS_OK)
        goto done;
    asset = NULL;
    status = rotmos_reply_source_take(reply, code, type, source, error);
    if (status != ROTMOS_OK)
        rotmos_source_destroy(source, NULL);
    return status;
empty:
    if (status == ROTMOS_OK)
        status = rotmos_reply_status(reply, code, error);
done:
    rotmos_asset_close(asset, NULL);
    return status;
}
static rotmos_status serve(rotmos_request *request, rotmos_reply *reply,
                           void *data, rotmos_error *error)
{
    struct static_route *r = data;
    const char *path = rotmos_request_path(request).data, *relative;
    char *copy, *index;
    rotmos_asset *asset = NULL, *selected = NULL;
    const rotmos_asset_info *info;
    size_t n, prefix, index_length;
    rotmos_status status;
    int trailing;
    if (!r->directory) {
        status =
            rotmos_asset_provider_open(r->provider, r->file, &asset, error);
        if (status == ROTMOS_EOF)
            return not_found(r, reply, error);
        if (status != ROTMOS_OK)
            return status;
        if (rotmos_asset_stat(asset)->kind != ROTMOS_ASSET_FILE) {
            rotmos_asset_close(asset, NULL);
            return not_found(r, reply, error);
        }
        return send_asset(r, request, reply, asset, r->file, error);
    }
    prefix = strlen(r->prefix);
    relative = strcmp(r->prefix, "/") == 0 ? path : path + prefix;
    if (*relative == 0)
        relative = "/";
    n = strlen(relative);
    trailing = n > 1 && relative[n - 1] == '/';
    copy = rotmos_copy_text(relative);
    if (copy == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "static",
                           "copying asset lookup path");
    if (trailing)
        copy[--n] = 0;
    status = rotmos_asset_provider_open(r->provider, copy, &asset, error);
    if (status == ROTMOS_INVALID || status == ROTMOS_EOF) {
        free(copy);
        return not_found(r, reply, error);
    }
    if (status != ROTMOS_OK) {
        free(copy);
        return status;
    }
    info = rotmos_asset_stat(asset);
    if (info->kind == ROTMOS_ASSET_FILE) {
        if (trailing) {
            free(copy);
            rotmos_asset_close(asset, NULL);
            return not_found(r, reply, error);
        }
        if (strcmp(copy, "/") == 0 && strcmp(r->prefix, "/") != 0 &&
            path[strlen(path) - 1] != '/') {
            free(copy);
            rotmos_asset_close(asset, NULL);
            return redirect(request, reply, error);
        }
        status = send_asset(r, request, reply, asset, copy, error);
        free(copy);
        return status;
    }
    if (!r->config.index_enabled) {
        free(copy);
        rotmos_asset_close(asset, NULL);
        return not_found(r, reply, error);
    }
    index_length = strlen(r->config.index_file);
    index = malloc(n + index_length + 2);
    if (index == NULL) {
        free(copy);
        rotmos_asset_close(asset, NULL);
        return rotmos_fail(error, ROTMOS_NOMEM, "static",
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
    status = rotmos_asset_provider_open(r->provider, index, &selected, error);
    rotmos_asset_close(asset, NULL);
    if (status != ROTMOS_OK) {
        free(index);
        return status == ROTMOS_EOF || status == ROTMOS_INVALID
                   ? not_found(r, reply, error)
                   : status;
    }
    if (rotmos_asset_stat(selected)->kind != ROTMOS_ASSET_FILE) {
        free(index);
        rotmos_asset_close(selected, NULL);
        return not_found(r, reply, error);
    }
    if (strlen(path) > 1 && path[strlen(path) - 1] != '/') {
        rotmos_asset_close(selected, NULL);
        free(index);
        return redirect(request, reply, error);
    }
    status = send_asset(r, request, reply, selected, index, error);
    free(index);
    return status;
}
static rotmos_status preflight(rotmos_request *request,
                               rotmos_request_plan *plan, void *data,
                               rotmos_error *error)
{
    struct static_route *r = data;
    const char *allow;
    rotmos_response *response;
    rotmos_status status;
    rotmos_u64 zero;
    if ((r->config.methods &
         ROTMOS_HTTP_METHOD_MASK(rotmos_request_method(request))) != 0)
        return ROTMOS_OK;
    allow = r->config.methods ==
                    (ROTMOS_HTTP_METHODS_GET | ROTMOS_HTTP_METHODS_HEAD)
                ? "GET, HEAD"
            : r->config.methods == ROTMOS_HTTP_METHODS_GET ? "GET"
                                                           : "HEAD";
    response = rotmos_request_response(request);
    zero.high = zero.low = 0;
    status = rotmos_response_status(response, 405, error);
    if (status == ROTMOS_OK)
        status = rotmos_response_header_set(response, "allow", allow, error);
    if (status == ROTMOS_OK)
        status = rotmos_response_known_length(response, zero, error);
    if (status == ROTMOS_OK)
        status = rotmos_response_begin(response, error);
    if (status == ROTMOS_OK)
        status = rotmos_response_end(response, NULL, 0, error);
    if (status == ROTMOS_OK)
        plan->disposition = ROTMOS_REQUEST_REPLY;
    return status;
}
static void release_route(void *data)
{
    struct static_route *r = data;
    if (r->own_provider)
        rotmos_asset_provider_close(r->provider, NULL);
    free(r->prefix);
    free(r->file);
    free((void *)r->config.content_type);
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
static rotmos_status add_route(rotmos_router *router, rotmos_route_group *group,
                               const rotmos_static_config *config, int kind,
                               rotmos_route **out, rotmos_error *error)
{
    struct static_route *r;
    rotmos_route_config route;
    rotmos_route *registered;
    rotmos_status status;
    char *path, *pattern, *root = NULL;
    const char *slash, *file;
    size_t n, group_length = 0;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || (router == NULL && group == NULL) ||
        config->path == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "static_route",
                           "provide router, static declaration and path");
    if ((config->methods &
         ~(ROTMOS_HTTP_METHODS_GET | ROTMOS_HTTP_METHODS_HEAD)) != 0 ||
        (config->content_type != NULL &&
         (!rotmos_header_value_valid(config->content_type) ||
          config->content_type[0] == 0)) ||
        (config->cache_control != NULL &&
         !rotmos_header_value_valid(config->cache_control)) ||
        (config->not_found_content_type != NULL &&
         (!rotmos_header_value_valid(config->not_found_content_type) ||
          config->not_found_content_type[0] == 0)))
        return rotmos_fail(
            error, ROTMOS_INVALID, "static_route",
            "static routes accept GET/HEAD and valid content/cache headers");
    path = rotmos_copy_text(config->path);
    if (path == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "static_route",
                           "copying static path");
    n = strlen(path);
    if (kind != 0)
        while (n > 1 && path[n - 1] == '/')
            path[--n] = 0;
    status = rotmos_route_path_validate(path, ROTMOS_PATH_LITERAL, error);
    if (status != ROTMOS_OK) {
        free(path);
        return status;
    }
    if (group != NULL) {
        if (group->config.prefix[0] != 0) {
            status = rotmos_route_path_validate(group->config.prefix,
                                                ROTMOS_PATH_LITERAL, error);
            if (status != ROTMOS_OK) {
                free(path);
                return status;
            }
        }
        group_length = strcmp(group->config.prefix, "/") == 0
                           ? 0
                           : strlen(group->config.prefix);
    }
    if (n > ROTMOS_ASSET_PATH_MAX || group_length > ROTMOS_ASSET_PATH_MAX - n) {
        free(path);
        return rotmos_fail(error, ROTMOS_LIMIT, "static_route",
                           "static prefix exceeds asset path bound");
    }
    r = calloc(1, sizeof(*r));
    if (r == NULL) {
        free(path);
        return rotmos_fail(error, ROTMOS_NOMEM, "static_route",
                           "allocating static route");
    }
    r->directory = kind != 0;
    r->custom = kind == 2;
    r->config.methods =
        config->methods ? config->methods
                        : ROTMOS_HTTP_METHODS_GET | ROTMOS_HTTP_METHODS_HEAD;
    r->config.index_enabled = config->index_enabled;
    r->prefix = malloc(group_length + n + 1);
    if (r->prefix != NULL) {
        if (group_length)
            memcpy(r->prefix, group->config.prefix, group_length);
        memcpy(r->prefix + group_length, path, n + 1);
        if (group_length && n == 1)
            r->prefix[group_length] = 0;
    }
    r->config.content_type = rotmos_copy_text(config->content_type);
    r->config.cache_control = rotmos_copy_text(config->cache_control);
    r->config.index_file = rotmos_copy_text(
        config->index_file != NULL ? config->index_file : "index.html");
    r->config.not_found_body = rotmos_copy_text(config->not_found_body);
    r->config.not_found_content_type =
        rotmos_copy_text(config->not_found_content_type);
    if (r->prefix == NULL ||
        (config->content_type != NULL && r->config.content_type == NULL) ||
        (config->cache_control != NULL && r->config.cache_control == NULL) ||
        r->config.index_file == NULL ||
        (config->not_found_body != NULL && r->config.not_found_body == NULL) ||
        (config->not_found_content_type != NULL &&
         r->config.not_found_content_type == NULL)) {
        free(path);
        release_route(r);
        return rotmos_fail(error, ROTMOS_NOMEM, "static_route",
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
        return rotmos_fail(error, ROTMOS_INVALID, "static_route",
                           "index_file must be one safe filename");
    }
    if (kind == 2) {
        r->provider = config->provider;
        status = r->provider != NULL
                     ? ROTMOS_OK
                     : rotmos_fail(error, ROTMOS_INVALID, "static_route",
                                   "custom asset provider is required");
    } else {
        if (kind == 0) {
            file = config->file_path;
            if (file == NULL || file[0] == 0) {
                free(path);
                release_route(r);
                return rotmos_fail(error, ROTMOS_INVALID, "static_route",
                                   "file_path is required");
            }
            slash = strrchr(file, '/');
            root = slash != NULL ? malloc((size_t)(slash - file) + 2)
                                 : rotmos_copy_text(".");
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
            root = rotmos_copy_text(config->root_dir);
        if (root == NULL || (kind == 0 && r->file == NULL)) {
            free(root);
            free(path);
            release_route(r);
            return rotmos_fail(
                error,
                config->root_dir == NULL && kind == 1 ? ROTMOS_INVALID
                                                      : ROTMOS_NOMEM,
                "static_route", "provide root_dir or a valid file path");
        }
        if (kind == 0 && !rotmos_asset_path_valid(r->file)) {
            free(root);
            free(path);
            release_route(r);
            return rotmos_fail(error, ROTMOS_INVALID, "static_route",
                               "file_path must select a safe basename");
        }
        status = rotmos_asset_filesystem_new(root, &r->provider, error);
        free(root);
        r->own_provider = status == ROTMOS_OK;
    }
    if (status != ROTMOS_OK) {
        free(path);
        release_route(r);
        return status;
    }
    pattern = kind == 0 ? rotmos_copy_text(r->prefix) : mount_regex(r->prefix);
    free(path);
    if (pattern == NULL) {
        release_route(r);
        return rotmos_fail(error, ROTMOS_NOMEM, "static_route",
                           "allocating mount matcher");
    }
    rotmos_route_config_init(&route);
    route.path = pattern;
    route.path_kind = kind == 0 ? ROTMOS_PATH_LITERAL : ROTMOS_PATH_REGEX;
    route.methods = kind == 0 ? r->config.methods : ROTMOS_HTTP_METHODS_ALL;
    route.handler = serve;
    route.userdata = r;
    route.hooks = config->hooks;
    status = rotmos_router_route(group != NULL ? group->router : router, &route,
                                 &registered, error);
    free(pattern);
    if (status != ROTMOS_OK)
        release_route(r);
    else {
        registered->group = group;
        registered->release_userdata = release_route;
        registered->preflight = preflight;
        registered->static_mount = kind != 0;
        if (out != NULL)
            *out = registered;
    }
    return status;
}
rotmos_status rotmos_router_static_file(rotmos_router *router,
                                        const rotmos_static_config *config,
                                        rotmos_route **out, rotmos_error *error)
{
    return add_route(router, NULL, config, 0, out, error);
}
rotmos_status rotmos_router_static_directory(rotmos_router *router,
                                             const rotmos_static_config *config,
                                             rotmos_route **out,
                                             rotmos_error *error)
{
    return add_route(router, NULL, config, 1, out, error);
}
rotmos_status rotmos_router_static_assets(rotmos_router *router,
                                          const rotmos_static_config *config,
                                          rotmos_route **out,
                                          rotmos_error *error)
{
    return add_route(router, NULL, config, 2, out, error);
}
rotmos_status rotmos_route_group_static_file(rotmos_route_group *group,
                                             const rotmos_static_config *config,
                                             rotmos_route **out,
                                             rotmos_error *error)
{
    return add_route(NULL, group, config, 0, out, error);
}
rotmos_status
rotmos_route_group_static_directory(rotmos_route_group *group,
                                    const rotmos_static_config *config,
                                    rotmos_route **out, rotmos_error *error)
{
    return add_route(NULL, group, config, 1, out, error);
}
rotmos_status
rotmos_route_group_static_assets(rotmos_route_group *group,
                                 const rotmos_static_config *config,
                                 rotmos_route **out, rotmos_error *error)
{
    return add_route(NULL, group, config, 2, out, error);
}
