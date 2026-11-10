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

static knurl_status decode(knurl_bytes input, int query, knurl_bytes *out,
                           knurl_error *error)
{
    char *text;
    size_t i;
    size_t used;
    unsigned char c;
    int high;
    int low;
    text = malloc(input.length + 1);
    if (text == NULL)
        return knurl_fail(error, KNURL_NOMEM, "request_decode",
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
    return KNURL_OK;
invalid:
    free(text);
    return knurl_fail(error, KNURL_INVALID, "request_decode",
                      "invalid percent escape or forbidden decoded byte");
}

knurl_status knurl_request_path_decode(const char *path, knurl_bytes *out,
                                       knurl_error *error)
{
    knurl_bytes input;
    input.data = path;
    input.length = strlen(path);
    return decode(input, 0, out, error);
}
knurl_status knurl_request_decode(knurl_request *request, knurl_error *error)
{
    const char *cursor;
    const char *end;
    const char *eq;
    const char *limit;
    knurl_bytes input;
    knurl_header pair;
    knurl_header *next;
    knurl_status status;
    status = decode(request->raw_path, 0, &request->path, error);
    if (status == KNURL_OK)
        status = knurl_route_path_validate(request->path.data,
                                           KNURL_PATH_LITERAL, error);
    /* A single trailing slash is admitted only when the ordinary first match
     * is a directory/asset/DAV mount. Keep decoded traversal and ordinary
     * route rejection ahead of the existing raw proxy fallback. */
    if (status == KNURL_INVALID && request->server != NULL &&
        request->path.data != NULL && request->path.length > 1) {
        char *path = (char *)request->path.data;
        size_t last = request->path.length - 1;
        if (path[last] == '/' && path[last - 1] != '/') {
            path[last] = 0;
            status = knurl_route_path_validate(path, KNURL_PATH_LITERAL, error);
            path[last] = '/';
            if (status == KNURL_OK) {
                status = knurl_router_select(request->server->router, request,
                                             error);
                if (status == KNURL_EOF ||
                    (status == KNURL_OK && !request->route->directory_mount))
                    status =
                        knurl_fail(error, KNURL_INVALID, "request_decode",
                                   "trailing slash requires a directory mount");
            }
        }
    }
    if (status == KNURL_INVALID && request->server != NULL &&
        knurl_proxy_validate_path(request->raw_path.data, NULL) == KNURL_OK) {
        free((void *)request->path.data);
        request->path.data = knurl_copy_text(request->raw_path.data);
        request->path.length = request->raw_path.length;
        if (request->path.data == NULL)
            return knurl_fail(error, KNURL_NOMEM, "request_decode",
                              "cannot copy raw proxy path");
        status =
            knurl_router_select_raw(request->server->router, request, error);
        if (status == KNURL_EOF)
            status = KNURL_INVALID;
        if (status == KNURL_OK)
            request->raw_selected = 1;
    }
    if (status != KNURL_OK)
        return status;
    cursor = request->query.data;
    if (cursor == NULL)
        return KNURL_OK;
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
            if (status != KNURL_OK)
                return status;
            input.data = eq < end ? eq + 1 : end;
            input.length = (size_t)(end - (const char *)input.data);
            status = decode(input, 1, &pair.value, error);
            if (status != KNURL_OK) {
                free((void *)pair.name.data);
                return status;
            }
            next = realloc(request->queries,
                           (request->query_count + 1) * sizeof(*next));
            if (next == NULL) {
                free((void *)pair.name.data);
                free((void *)pair.value.data);
                return knurl_fail(error, KNURL_NOMEM, "request_query",
                                  "cannot allocate query pair");
            }
            request->queries = next;
            request->queries[request->query_count++] = pair;
        }
        cursor = end < limit ? end + 1 : limit;
    }
    return KNURL_OK;
}
