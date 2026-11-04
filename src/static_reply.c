#include "static_internal.h"
#include <rotmos/request.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>

static const char *header(rotmos_request *request, const char *name)
{
    rotmos_bytes bytes;
    return rotmos_request_header_first(request, name, &bytes, NULL) == ROTMOS_OK
               ? bytes.data
               : NULL;
}
rotmos_status rotmos_static_reply_asset(const rotmos_static_config *config,
                                        int custom, rotmos_request *request,
                                        rotmos_reply *reply,
                                        rotmos_asset *asset, const char *path,
                                        rotmos_error *error)
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
    type = custom && info->content_type != NULL ? info->content_type
                                                : config->content_type;
    if (custom && (type == NULL || type[0] == 0))
        type = "application/octet-stream";
    else if (type == NULL)
        type = info->content_type != NULL ? info->content_type
                                          : rotmos_static_content_type(path);
    if (config->cache_control != NULL || custom)
        status = rotmos_reply_header_set(
            reply, "cache-control",
            config->cache_control != NULL ? config->cache_control : "no-cache",
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
