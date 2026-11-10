#include "static_internal.h"
#include <knurl/request.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>

static const char *header(knurl_request *request, const char *name)
{
    knurl_bytes bytes;
    return knurl_request_header_first(request, name, &bytes, NULL) == KNURL_OK
               ? bytes.data
               : NULL;
}
knurl_status knurl_static_reply_asset(const knurl_static_config *config,
                                      int custom, knurl_request *request,
                                      knurl_reply *reply, knurl_asset *asset,
                                      const char *path, knurl_error *error)
{
    const knurl_asset_info *info = knurl_asset_stat(asset);
    struct knurl_static_range range;
    knurl_u64 length, start, end, one;
    const char *type, *value, *range_input;
    char total[21], first[21], last[21], content_range[80];
    size_t written;
    knurl_source *source;
    knurl_status status = KNURL_OK;
    int match, code = 200;
    type = knurl_mime_resolve(knurl_request_server(request), config, path,
                              info->content_type);
    if (config->cache_control != NULL || custom)
        status = knurl_reply_header_set(
            reply, "cache-control",
            config->cache_control != NULL ? config->cache_control : "no-cache",
            error);
    if (status == KNURL_OK && info->etag != NULL && info->etag[0] != 0)
        status = knurl_reply_header_set(reply, "etag", info->etag, error);
    if (status == KNURL_OK)
        status = knurl_reply_header_set(reply, "accept-ranges", "bytes", error);
    if (status == KNURL_OK && info->last_modified != NULL)
        status = knurl_reply_header_set(reply, "last-modified",
                                        info->last_modified, error);
    value = header(request, "if-match");
    if (status == KNURL_OK && value != NULL) {
        match = knurl_http_tag_matches(value, info->etag, 1, 1);
        if (match != 1) {
            code = match < 0 ? 400 : 412;
            goto empty;
        }
    }
    value = header(request, "if-none-match");
    if (status == KNURL_OK && value != NULL) {
        match = knurl_http_tag_matches(value, info->etag, 1, 0);
        if (match != 0) {
            code = match < 0 ? 400 : 304;
            goto empty;
        }
    }
    if (status == KNURL_OK && value == NULL && info->last_modified != NULL) {
        const char *since = header(request, "if-modified-since");
        if (since != NULL && curl_getdate(since, NULL) != (time_t)-1 &&
            curl_getdate(since, NULL) ==
                curl_getdate(info->last_modified, NULL)) {
            code = 304;
            goto empty;
        }
    }
    if (status != KNURL_OK)
        goto done;
    range_input = knurl_request_method(request) == KNURL_HTTP_GET
                      ? header(request, "range")
                      : NULL;
    value = header(request, "if-range");
    if (range_input != NULL && value != NULL &&
        !knurl_static_if_range(value, info->etag))
        range_input = NULL;
    knurl_static_range_parse(range_input, info->size, &range);
    knurl_u64_format_decimal(info->size, total, sizeof(total), &written, NULL);
    if (range.requested) {
        if (!range.satisfiable) {
            snprintf(content_range, sizeof(content_range), "bytes */%s", total);
            code = 416;
        } else {
            one.high = 0;
            one.low = 1;
            knurl_u64_add(range.start, range.length, &end, NULL);
            end = knurl_asset_subtract(end, one);
            knurl_u64_format_decimal(range.start, first, sizeof(first),
                                     &written, NULL);
            knurl_u64_format_decimal(end, last, sizeof(last), &written, NULL);
            snprintf(content_range, sizeof(content_range), "bytes %s-%s/%s",
                     first, last, total);
            code = 206;
        }
        status = knurl_reply_header_set(reply, "content-range", content_range,
                                        error);
        if (status != KNURL_OK || code == 416)
            goto empty;
    }
    start.high = start.low = 0;
    length = info->size;
    if (range.requested) {
        start = range.start;
        length = range.length;
    }
    status = knurl_asset_source_take(asset, start, length, &source, error);
    if (status != KNURL_OK)
        goto done;
    asset = NULL;
    status = knurl_reply_source_take(reply, code, type, source, error);
    if (status != KNURL_OK)
        knurl_source_destroy(source, NULL);
    return status;
empty:
    if (status == KNURL_OK)
        status = knurl_reply_status(reply, code, error);
done:
    knurl_asset_close(asset, NULL);
    return status;
}
