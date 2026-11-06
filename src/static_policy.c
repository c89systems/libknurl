#include <rotmos/static.h>
#include "static_internal.h"
#include <ctype.h>
#include <string.h>
static int extension_is(const char *ext, const char *expected)
{
    if (ext == NULL || expected == NULL) {
        return 0;
    }
    while (*ext != '\0' && *expected != '\0') {
        if (tolower((unsigned char)*ext) != tolower((unsigned char)*expected)) {
            return 0;
        }
        ext++;
        expected++;
    }
    return *ext == '\0' && *expected == '\0';
}

const char *rotmos_static_content_type(const char *path)
{
    const char *base;
    const char *dot;
    const char *p;

    if (path == NULL) {
        return "application/octet-stream";
    }
    base = path;
    dot = NULL;
    for (p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
            dot = NULL;
        } else if (*p == '.') {
            dot = p;
        }
    }
    if (dot == NULL || dot == base || dot[1] == '\0') {
        return "application/octet-stream";
    }
    dot++;
    if (extension_is(dot, "html") || extension_is(dot, "htm")) {
        return "text/html; charset=utf-8";
    }
    if (extension_is(dot, "css")) {
        return "text/css; charset=utf-8";
    }
    if (extension_is(dot, "js") || extension_is(dot, "mjs")) {
        return "text/javascript; charset=utf-8";
    }
    if (extension_is(dot, "json") || extension_is(dot, "map")) {
        return "application/json";
    }
    if (extension_is(dot, "md") || extension_is(dot, "markdown")) {
        return "text/markdown; charset=utf-8";
    }
    if (extension_is(dot, "txt") || extension_is(dot, "text") ||
        extension_is(dot, "log")) {
        return "text/plain; charset=utf-8";
    }
    if (extension_is(dot, "csv")) {
        return "text/csv; charset=utf-8";
    }
    if (extension_is(dot, "xml")) {
        return "application/xml";
    }
    if (extension_is(dot, "svg")) {
        return "image/svg+xml";
    }
    if (extension_is(dot, "png")) {
        return "image/png";
    }
    if (extension_is(dot, "jpg") || extension_is(dot, "jpeg")) {
        return "image/jpeg";
    }
    if (extension_is(dot, "gif")) {
        return "image/gif";
    }
    if (extension_is(dot, "webp")) {
        return "image/webp";
    }
    if (extension_is(dot, "ico")) {
        return "image/x-icon";
    }
    if (extension_is(dot, "wasm")) {
        return "application/wasm";
    }
    if (extension_is(dot, "pdf")) {
        return "application/pdf";
    }
    if (extension_is(dot, "woff")) {
        return "font/woff";
    }
    if (extension_is(dot, "woff2")) {
        return "font/woff2";
    }
    return "application/octet-stream";
}

/* Preserve Vectis's complete-list validation, including malformed tails, with
 * weak comparison on both sides for If-None-Match. */
int rotmos_http_tag_matches(const char *p, const char *etag, int exists,
                            int strong)
{
    const char *start, *actual = etag;
    size_t length;
    int matched = 0, weak, actual_weak;
    if (p == NULL)
        return -1;
    if (actual == NULL)
        actual = "";
    actual_weak = actual[0] == 'W' && actual[1] == '/';
    if (actual_weak)
        actual += 2;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p == '*') {
        ++p;
        while (*p == ' ' || *p == '\t')
            ++p;
        return *p == 0 ? exists : -1;
    }
    for (;;) {
        weak = p[0] == 'W' && p[1] == '/';
        if (weak)
            p += 2;
        if (*p != '"')
            return -1;
        start = p++;
        while (*p != '"') {
            if ((unsigned char)*p < 0x21 || (unsigned char)*p == 127)
                return -1;
            ++p;
        }
        ++p;
        length = (size_t)(p - start);
        if (exists && (!strong || (!weak && !actual_weak)) &&
            strlen(actual) == length && memcmp(actual, start, length) == 0)
            matched = 1;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == 0)
            return matched;
        if (*p++ != ',')
            return -1;
        while (*p == ' ' || *p == '\t')
            ++p;
    }
}
int rotmos_static_if_range(const char *input, const char *etag)
{
    size_t n;
    if (input == NULL || etag == NULL || etag[0] == 0 ||
        (etag[0] == 'W' && etag[1] == '/'))
        return 0;
    while (*input == ' ' || *input == '\t')
        ++input;
    n = strlen(input);
    while (n > 0 && (input[n - 1] == ' ' || input[n - 1] == '\t'))
        --n;
    return strlen(etag) == n && memcmp(input, etag, n) == 0;
}
static int number(const char **cursor, rotmos_u64 *value)
{
    const char *p = *cursor;
    char text[21];
    size_t n = 0;
    while (*p >= '0' && *p <= '9') {
        if (n == 20)
            return 0;
        text[n++] = *p++;
    }
    if (n == 0)
        return 0;
    text[n] = 0;
    if (rotmos_u64_from_decimal(text, n, value, NULL) != ROTMOS_OK)
        return 0;
    *cursor = p;
    return 1;
}
void rotmos_static_range_parse(const char *input, rotmos_u64 size,
                               struct rotmos_static_range *out)
{
    const char *p;
    rotmos_u64 start, end, suffix, one;
    memset(out, 0, sizeof(*out));
    if (input == NULL || *input == 0 || strncmp(input, "bytes=", 6) != 0)
        return;
    out->requested = 1;
    if (strchr(input, ',') != NULL || (size.high == 0 && size.low == 0))
        return;
    one.high = 0;
    one.low = 1;
    p = input + 6;
    end = rotmos_asset_subtract(size, one);
    if (*p == '-') {
        ++p;
        if (!number(&p, &suffix) || *p != 0 ||
            (suffix.high == 0 && suffix.low == 0))
            return;
        out->length = rotmos_asset_compare(suffix, size) >= 0 ? size : suffix;
        out->start = rotmos_asset_subtract(size, out->length);
        out->satisfiable = 1;
        return;
    }
    if (!number(&p, &start) || *p++ != '-' ||
        rotmos_asset_compare(start, size) >= 0)
        return;
    if (*p != 0) {
        if (!number(&p, &end) || *p != 0 ||
            rotmos_asset_compare(end, start) < 0)
            return;
        if (rotmos_asset_compare(end, size) >= 0)
            end = rotmos_asset_subtract(size, one);
    }
    out->start = start;
    out->length = rotmos_asset_subtract(end, start);
    rotmos_u64_add(out->length, one, &out->length, NULL);
    out->satisfiable = 1;
}
