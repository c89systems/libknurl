#include "request_internal.h"
#include "server_internal.h"
#include "proxy_common.h"

#include <stdlib.h>
#include <string.h>

static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static rotmos_status decode(rotmos_bytes input, int query, rotmos_bytes *out,
                            rotmos_error *error)
{
    char *text;
    size_t i;
    size_t used;
    unsigned char c;
    int high;
    int low;
    text = malloc(input.length + 1);
    if (text == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "request_decode",
                           "cannot allocate decoded component");
    used = 0;
    for (i = 0; i < input.length; ++i) {
        c = ((const unsigned char *)input.data)[i];
        if (c == '%') {
            if (input.length - i < 3 ||
                (high = hex(((const unsigned char *)input.data)[i + 1])) < 0 ||
                (low = hex(((const unsigned char *)input.data)[i + 2])) < 0)
                goto invalid;
            c = (unsigned char)(high * 16 + low);
            i += 2;
            if (!query && c == '/')
                goto invalid;
        } else if (query && c == '+') {
            c = ' ';
        }
        if (c == 0 || (!query && (c < 32 || c == 127 || c == '\\')))
            goto invalid;
        text[used++] = (char)c;
    }
    text[used] = '\0';
    out->data = text;
    out->length = used;
    return ROTMOS_OK;
invalid:
    free(text);
    return rotmos_fail(error, ROTMOS_INVALID, "request_decode",
                       "invalid percent escape or forbidden decoded byte");
}

rotmos_status rotmos_request_decode(rotmos_request *request,
                                    rotmos_error *error)
{
    const char *cursor;
    const char *end;
    const char *eq;
    const char *limit;
    rotmos_bytes input;
    rotmos_header pair;
    rotmos_header *next;
    rotmos_status status;
    status = decode(request->raw_path, 0, &request->path, error);
    if (status == ROTMOS_OK)
        status = rotmos_route_path_validate(request->path.data,
                                            ROTMOS_PATH_LITERAL, error);
    /* A single trailing slash is admitted only when the ordinary first match
     * is a static directory/asset mount. Keep decoded traversal and ordinary
     * route rejection ahead of the existing raw proxy fallback. */
    if (status == ROTMOS_INVALID && request->server != NULL &&
        request->path.data != NULL && request->path.length > 1) {
        char *path = (char *)request->path.data;
        size_t last = request->path.length - 1;
        if (path[last] == '/' && path[last - 1] != '/') {
            path[last] = 0;
            status =
                rotmos_route_path_validate(path, ROTMOS_PATH_LITERAL, error);
            path[last] = '/';
            if (status == ROTMOS_OK) {
                status = rotmos_router_select(request->server->router, request,
                                              error);
                if (status == ROTMOS_EOF ||
                    (status == ROTMOS_OK && !request->route->static_mount))
                    status =
                        rotmos_fail(error, ROTMOS_INVALID, "request_decode",
                                    "trailing slash requires a static mount");
            }
        }
    }
    if (status == ROTMOS_INVALID && request->server != NULL &&
        rotmos_proxy_validate_path(request->raw_path.data, NULL) == ROTMOS_OK) {
        free((void *)request->path.data);
        request->path.data = rotmos_copy_text(request->raw_path.data);
        request->path.length = request->raw_path.length;
        if (request->path.data == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "request_decode",
                               "cannot copy raw proxy path");
        status =
            rotmos_router_select_raw(request->server->router, request, error);
        if (status == ROTMOS_EOF)
            status = ROTMOS_INVALID;
        if (status == ROTMOS_OK)
            request->raw_selected = 1;
    }
    if (status != ROTMOS_OK)
        return status;
    cursor = request->query.data;
    if (cursor == NULL)
        return ROTMOS_OK;
    limit = cursor + request->query.length;
    while (cursor < limit) {
        end = memchr(cursor, '&', (size_t)(limit - cursor));
        if (end == NULL)
            end = limit;
        eq = memchr(cursor, '=', (size_t)(end - cursor));
        if (eq == NULL)
            eq = end;
        if (eq != cursor) {
            input.data = cursor;
            input.length = (size_t)(eq - cursor);
            status = decode(input, 1, &pair.name, error);
            if (status != ROTMOS_OK)
                return status;
            input.data = eq < end ? eq + 1 : end;
            input.length = (size_t)(end - (const char *)input.data);
            status = decode(input, 1, &pair.value, error);
            if (status != ROTMOS_OK) {
                free((void *)pair.name.data);
                return status;
            }
            next = realloc(request->queries,
                           (request->query_count + 1) * sizeof(*next));
            if (next == NULL) {
                free((void *)pair.name.data);
                free((void *)pair.value.data);
                return rotmos_fail(error, ROTMOS_NOMEM, "request_query",
                                   "cannot allocate query pair");
            }
            request->queries = next;
            request->queries[request->query_count++] = pair;
        }
        cursor = end < limit ? end + 1 : limit;
    }
    return ROTMOS_OK;
}
