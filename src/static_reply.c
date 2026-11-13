#include "static_internal.h"
#include "reply_internal.h"
#include <knurl/request.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *header(knurl_request *request, const char *name)
{
    knurl_bytes bytes;
    return knurl_request_header_first(request, name, &bytes, NULL) == KNURL_OK
               ? bytes.data
               : NULL;
}
static const char *single_header(knurl_request *request, const char *name)
{
    const char *value = NULL;
    knurl_header h;
    size_t i;
    for (i = 0; i < knurl_request_header_count(request); ++i) {
        (void)knurl_request_header_at(request, i, &h, NULL);
        if (knurl_text_equal_case(h.name.data, name)) {
            if (value != NULL)
                return NULL;
            value = h.value.data;
        }
    }
    return value;
}
knurl_status knurl_static_reply_asset(const knurl_static_config *config,
                                      int custom, knurl_request *request,
                                      knurl_reply *reply,
                                      knurl_asset_provider *provider,
                                      knurl_asset *asset, const char *path,
                                      knurl_error *error)
{
    const knurl_asset_info *info = knurl_asset_stat(asset);
    knurl_asset *identity = asset;
    struct knurl_static_encoding encoding;
    struct knurl_static_ranges range;
    knurl_u64 length, start, end, one;
    const char *type, *value, *range_input;
    char total[21], first[21], last[21], content_range[80];
    unsigned char random[16];
    char boundary[40], multipart_type[96];
    size_t i;
    size_t written;
    knurl_source *source, *compressed;
    knurl_status status = KNURL_OK;
    int match, code = 200;
    time_t modified, since_time;
    type = knurl_mime_resolve(knurl_request_server(request), config, path,
                              info->content_type);
    status = knurl_static_encoding_select(config, provider, request, reply,
                                          asset, path, type, &encoding, error);
    asset = encoding.asset;
    if (status == KNURL_EOF) {
        status = KNURL_OK;
        code = 406;
        goto empty;
    }
    if (status != KNURL_OK)
        goto done;
    info = &encoding.info;
    if (encoding.coding != NULL)
        status = knurl_reply_header_set(reply, "content-encoding",
                                        encoding.coding, error);
    if (status == KNURL_OK && (config->cache_control != NULL || custom))
        status = knurl_reply_header_set(
            reply, "cache-control",
            config->cache_control != NULL ? config->cache_control : "no-cache",
            error);
    if (status == KNURL_OK && info->etag != NULL && info->etag[0] != 0)
        status = knurl_reply_header_set(reply, "etag", info->etag, error);
    if (status == KNURL_OK)
        status = knurl_reply_header_set(
            reply, "accept-ranges", encoding.coding != NULL ? "none" : "bytes",
            error);
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
    } else if (status == KNURL_OK && info->last_modified != NULL) {
        value = single_header(request, "if-unmodified-since");
        since_time = knurl_static_date(value);
        modified = since_time != (time_t)-1
                       ? knurl_static_date(info->last_modified)
                       : (time_t)-1;
        if (since_time != (time_t)-1 && modified != (time_t)-1 &&
            modified > since_time) {
            code = 412;
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
        const char *since = single_header(request, "if-modified-since");
        since_time = knurl_static_date(since);
        modified = since_time != (time_t)-1
                       ? knurl_static_date(info->last_modified)
                       : (time_t)-1;
        if (since_time != (time_t)-1 && modified != (time_t)-1 &&
            since_time >= modified) {
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
        (single_header(request, "if-range") == NULL ||
         !knurl_static_if_range(value, info)))
        range_input = NULL;
    knurl_static_range_parse(range_input, info->size, &range);
    knurl_u64_format_decimal(info->size, total, sizeof(total), &written, NULL);
    if (range.requested) {
        if (!range.satisfiable) {
            snprintf(content_range, sizeof(content_range), "bytes */%s", total);
            code = 416;
        } else if (range.count == 1) {
            one.high = 0;
            one.low = 1;
            knurl_u64_add(range.parts[0].start, range.parts[0].length, &end,
                          NULL);
            end = knurl_asset_subtract(end, one);
            knurl_u64_format_decimal(range.parts[0].start, first, sizeof(first),
                                     &written, NULL);
            knurl_u64_format_decimal(end, last, sizeof(last), &written, NULL);
            snprintf(content_range, sizeof(content_range), "bytes %s-%s/%s",
                     first, last, total);
            code = 206;
        } else {
            if (RAND_bytes(random, sizeof(random)) != 1) {
                status = knurl_fail(error, KNURL_BACKEND, "static_multipart",
                                    "cannot generate boundary");
                goto done;
            }
            memcpy(boundary, "knurl-", 6);
            for (i = 0; i < sizeof(random); ++i) {
                boundary[6 + i * 2] = "0123456789abcdef"[random[i] >> 4];
                boundary[7 + i * 2] = "0123456789abcdef"[random[i] & 15];
            }
            boundary[38] = 0;
            status = knurl_static_multipart_take(asset, &range, type, boundary,
                                                 &source, error);
            if (status != KNURL_OK)
                goto done;
            if (asset == identity)
                identity = NULL;
            asset = NULL;
            (void)snprintf(multipart_type, sizeof(multipart_type),
                           "multipart/byteranges; boundary=%s", boundary);
            status = knurl_reply_source_take(reply, 206, multipart_type, source,
                                             error);
            if (status != KNURL_OK)
                (void)knurl_source_destroy(source, NULL);
            goto done;
        }
        status = knurl_reply_header_set(reply, "content-range", content_range,
                                        error);
        if (status != KNURL_OK || code == 416)
            goto empty;
    }
    start.high = start.low = 0;
    length = info->size;
    if (range.requested) {
        start = range.parts[0].start;
        length = range.parts[0].length;
    }
    status = knurl_asset_source_take(asset, start, length, &source, error);
    if (status != KNURL_OK)
        goto done;
    if (asset == identity)
        identity = NULL;
    asset = NULL;
    if (encoding.gzip) {
        status = knurl_static_gzip_take(source, &compressed, error);
        if (status != KNURL_OK) {
            (void)knurl_source_destroy(source, NULL);
            goto done;
        }
        source = compressed;
    }
    status = knurl_reply_source_take(reply, code, type, source, error);
    if (status != KNURL_OK)
        knurl_source_destroy(source, NULL);
    goto done;
empty:
    if (status == KNURL_OK)
        status = knurl_reply_status(reply, code, error);
done:
    if (status == KNURL_OK)
        reply->impl->automatic_asset = 1;
    if (identity != asset)
        knurl_asset_close(identity, NULL);
    knurl_asset_close(asset, NULL);
    return status;
}
