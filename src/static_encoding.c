#include "static_internal.h"
#include <knurl/request.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int equal(const char *p, size_t n, const char *word)
{
    size_t i;
    unsigned char c;
    if (n != strlen(word))
        return 0;
    for (i = 0; i < n; ++i) {
        c = (unsigned char)p[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + 'a' - 'A');
        if (c != (unsigned char)word[i])
            return 0;
    }
    return 1;
}
static int token(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}
static int contains(const char *p, const char *name)
{
    const char *begin;
    int quoted;
    if (p == NULL)
        return 0;
    while (*p != 0) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            ++p;
        begin = p;
        while (token((unsigned char)*p))
            ++p;
        if (equal(begin, (size_t)(p - begin), name))
            return 1;
        quoted = 0;
        while (*p != 0) {
            if (*p == '"')
                quoted = !quoted;
            else if (*p == '\\' && quoted && p[1] != 0)
                ++p;
            else if (*p == ',' && !quoted) {
                ++p;
                break;
            }
            ++p;
        }
    }
    return 0;
}
static int weights(const char *p, int quality[4], size_t *empty)
{
    const char *begin;
    size_t length;
    int q, whole, digits, scale, index;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == 0 || *p == ',') {
            if (++*empty > 32)
                return 0;
            if (*p == 0)
                return 1;
            ++p;
            continue;
        }
        begin = p;
        while (token((unsigned char)*p))
            ++p;
        length = (size_t)(p - begin);
        if (length == 0)
            return 0;
        while (*p == ' ' || *p == '\t')
            ++p;
        q = 1000;
        if (*p == ';') {
            ++p;
            while (*p == ' ' || *p == '\t')
                ++p;
            if ((*p != 'q' && *p != 'Q') || p[1] != '=')
                return 0;
            p += 2;
            if (*p != '0' && *p != '1')
                return 0;
            whole = *p++ - '0';
            q = whole * 1000;
            if (*p == '.') {
                ++p;
                digits = 0;
                scale = 100;
                while (*p >= '0' && *p <= '9') {
                    if (++digits > 3 || (whole && *p != '0'))
                        return 0;
                    if (!whole)
                        q += (*p - '0') * scale;
                    scale /= 10;
                    ++p;
                }
            }
            while (*p == ' ' || *p == '\t')
                ++p;
        }
        index = equal(begin, length, "identity") ? 0
                : equal(begin, length, "gzip")   ? 1
                : equal(begin, length, "br")     ? 2
                : equal(begin, length, "*")      ? 3
                                                 : -1;
        if (index >= 0) {
            if (quality[index] >= 0)
                return 0;
            quality[index] = q;
        }
        if (*p == 0)
            return 1;
        if (*p++ != ',')
            return 0;
    }
}
static int eligible(const char *type)
{
    const char *end;
    size_t n;
    while (*type == ' ' || *type == '\t')
        ++type;
    end = strchr(type, ';');
    n = end != NULL ? (size_t)(end - type) : strlen(type);
    while (n != 0 && (type[n - 1] == ' ' || type[n - 1] == '\t'))
        --n;
    return (n >= 5 && equal(type, 5, "text/")) ||
           equal(type, n, "application/json") ||
           equal(type, n, "application/xml") ||
           equal(type, n, "application/javascript") ||
           equal(type, n, "application/wasm") ||
           (n >= 5 && equal(type + n - 5, 5, "+json")) ||
           (n >= 4 && equal(type + n - 4, 4, "+xml"));
}
static int fresh(const knurl_asset_info *original,
                 const knurl_asset_info *encoded)
{
    int order;
    time_t a, b;
    if (encoded->kind != KNURL_ASSET_FILE)
        return 0;
    if (original->has_modification_time && encoded->has_modification_time) {
        (void)knurl_u64_compare(encoded->modification_seconds,
                                original->modification_seconds, &order, NULL);
        return order > 0 ||
               (order == 0 && encoded->modification_nanoseconds >=
                                  original->modification_nanoseconds);
    }
    if (!original->last_modified_strong || !encoded->last_modified_strong ||
        original->last_modified == NULL || encoded->last_modified == NULL)
        return 0;
    a = curl_getdate(original->last_modified, NULL);
    b = curl_getdate(encoded->last_modified, NULL);
    return a != (time_t)-1 && b != (time_t)-1 && b >= a;
}
static knurl_status sidecar(knurl_asset_provider *provider, const char *path,
                            const char *suffix,
                            const knurl_asset_info *original, knurl_asset **out,
                            knurl_error *error)
{
    char *name;
    size_t n = strlen(path), extra = strlen(suffix);
    knurl_status status;
    *out = NULL;
    if (provider == NULL || n > KNURL_ASSET_PATH_MAX - extra)
        return KNURL_EOF;
    name = malloc(n + extra + 1);
    if (name == NULL)
        return knurl_fail(error, KNURL_NOMEM, "static_encoding",
                          "cannot allocate sidecar path");
    memcpy(name, path, n);
    memcpy(name + n, suffix, extra + 1);
    status = knurl_asset_provider_open(provider, name, out, error);
    free(name);
    if (status == KNURL_OK && !fresh(original, knurl_asset_stat(*out))) {
        (void)knurl_asset_close(*out, NULL);
        *out = NULL;
        return KNURL_EOF;
    }
    return status;
}
static knurl_status vary(knurl_reply *reply, knurl_error *error)
{
    size_t i;
    knurl_header h;
    for (i = 0; i < knurl_reply_header_count(reply); ++i) {
        (void)knurl_reply_header_at(reply, i, &h, NULL);
        if (equal(h.name.data, h.name.length, "vary") &&
            (contains(h.value.data, "accept-encoding") ||
             contains(h.value.data, "*")))
            return KNURL_OK;
    }
    return knurl_reply_header_add(reply, "vary", "Accept-Encoding", error);
}
knurl_status knurl_static_encoding_select(
    const knurl_static_config *config, knurl_asset_provider *provider,
    knurl_request *request, knurl_reply *reply, knurl_asset *identity,
    const char *path, const char *type, struct knurl_static_encoding *out,
    knurl_error *error)
{
    int quality[4] = {-1, -1, -1, -1};
    int q_identity, q_gzip, q_br, best, allowed = !config->compression_disabled;
    size_t i, empty = 0;
    knurl_header h;
    knurl_asset *candidate = NULL;
    const knurl_asset_info *original = knurl_asset_stat(identity);
    knurl_status status;
    const char *tag;
    memset(out, 0, sizeof(*out));
    out->asset = identity;
    for (i = 0; i < knurl_request_header_count(request); ++i) {
        (void)knurl_request_header_at(request, i, &h, NULL);
        if (equal(h.name.data, h.name.length, "accept-encoding") &&
            !weights(h.value.data, quality, &empty))
            return knurl_fail(error, KNURL_INVALID, "static_encoding",
                              "invalid or ambiguous Accept-Encoding weights");
        if ((equal(h.name.data, h.name.length, "range") &&
             knurl_request_method(request) == KNURL_HTTP_GET) ||
            (equal(h.name.data, h.name.length, "cache-control") &&
             contains(h.value.data, "no-transform")))
            allowed = 0;
    }
    if (contains(config->cache_control, "no-transform"))
        allowed = 0;
    for (i = 0; i < knurl_reply_header_count(reply); ++i) {
        (void)knurl_reply_header_at(reply, i, &h, NULL);
        if (equal(h.name.data, h.name.length, "content-encoding") ||
            (equal(h.name.data, h.name.length, "cache-control") &&
             contains(h.value.data, "no-transform")))
            allowed = 0;
    }
    if (!config->compression_disabled) {
        status = vary(reply, error);
        if (status != KNURL_OK)
            return status;
    }
    q_identity = quality[0] >= 0 ? quality[0] : quality[3] == 0 ? 0 : 1000;
    q_gzip = quality[1] >= 0 ? quality[1] : quality[3] >= 0 ? quality[3] : 0;
    q_br = quality[2] >= 0 ? quality[2] : quality[3] >= 0 ? quality[3] : 0;
    best = q_identity;
    if (allowed && (original->size.high != 0 || original->size.low != 0)) {
        if (q_gzip > 0 && q_gzip >= best && eligible(type)) {
            out->gzip = 1;
            out->coding = "gzip";
            best = q_gzip;
        }
        if (!config->precompressed_disabled) {
            if (q_gzip > 0 && q_gzip >= best) {
                status =
                    sidecar(provider, path, ".gz", original, &candidate, error);
                if (status != KNURL_OK && status != KNURL_EOF)
                    return status;
                if (candidate != NULL) {
                    out->asset = candidate;
                    out->gzip = 0;
                    out->coding = "gzip";
                    best = q_gzip;
                }
            }
            if (q_br > 0 && q_br >= best) {
                status =
                    sidecar(provider, path, ".br", original, &candidate, error);
                if (status != KNURL_OK && status != KNURL_EOF)
                    return status;
                if (candidate != NULL) {
                    if (out->asset != identity)
                        (void)knurl_asset_close(out->asset, NULL);
                    out->asset = candidate;
                    out->gzip = 0;
                    out->coding = "br";
                    best = q_br;
                }
            }
        }
    }
    out->info = *knurl_asset_stat(out->asset);
    if (best == 0)
        return KNURL_EOF;
    if (out->coding != NULL && out->info.etag != NULL &&
        out->info.etag[0] != 0) {
        tag = out->info.etag;
        if (tag[0] == 'W' && tag[1] == '/')
            tag += 2;
        (void)snprintf(out->etag, sizeof(out->etag), "%s\"%s:%s",
                       out->gzip || out->info.etag[0] == 'W' ? "W/" : "",
                       out->coding, tag + 1);
        out->info.etag = out->etag;
    }
    return KNURL_OK;
}
