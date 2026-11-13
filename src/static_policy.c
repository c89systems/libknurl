#include <knurl/static.h>
#include "static_internal.h"
#include <ctype.h>
#include <curl/curl.h>
#include <string.h>
#include <time.h>
static int date_shape(const char *text, size_t length, const char *shape)
{
    size_t i;
    unsigned char c;
    if (length != strlen(shape))
        return 0;
    for (i = 0; i < length; ++i) {
        c = (unsigned char)text[i];
        if (shape[i] == '#') {
            if (c < '0' || c > '9')
                return 0;
        } else if (shape[i] == '@') {
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
                return 0;
        } else if (c != (unsigned char)shape[i])
            return 0;
    }
    return 1;
}
/* curl_getdate accepts more than HTTP-date, including some date lists. Check
 * the three HTTP wire forms before using its calendar/time conversion. */
time_t knurl_static_date(const char *text)
{
    const char *comma;
    size_t n;
    if (text == NULL)
        return (time_t)-1;
    while (*text == ' ' || *text == '\t')
        ++text;
    n = strlen(text);
    while (n != 0 && (text[n - 1] == ' ' || text[n - 1] == '\t'))
        --n;
    if (date_shape(text, n, "@@@, ## @@@ #### ##:##:## GMT") ||
        date_shape(text, n, "@@@ @@@ ## ##:##:## ####") ||
        date_shape(text, n, "@@@ @@@  # ##:##:## ####"))
        return curl_getdate(text, NULL);
    comma = memchr(text, ',', n);
    if (comma != NULL && comma - text >= 6 && comma - text <= 9 &&
        date_shape(comma, n - (size_t)(comma - text),
                   ", ##-@@@-## ##:##:## GMT"))
        return curl_getdate(text, NULL);
    return (time_t)-1;
}
/* Preserve Vectis's complete-list validation, including malformed tails, with
 * weak comparison on both sides for If-None-Match. */
int knurl_http_tag_matches(const char *p, const char *etag, int exists,
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
int knurl_static_if_range(const char *input, const knurl_asset_info *info)
{
    size_t n;
    const char *etag = info->etag;
    time_t date, modified;
    if (input == NULL)
        return 0;
    while (*input == ' ' || *input == '\t')
        ++input;
    if (input[0] != '"' && !(input[0] == 'W' && input[1] == '/')) {
        if (!info->last_modified_strong || info->last_modified == NULL)
            return 0;
        date = knurl_static_date(input);
        modified = knurl_static_date(info->last_modified);
        return date != (time_t)-1 && modified != (time_t)-1 && date == modified;
    }
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
static int number(const char **cursor, knurl_u64 *value)
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
    if (knurl_u64_from_decimal(text, n, value, NULL) != KNURL_OK)
        return 0;
    *cursor = p;
    return 1;
}
void knurl_static_range_parse(const char *input, knurl_u64 size,
                              struct knurl_static_ranges *out)
{
    const char *p;
    knurl_u64 start, end, suffix, one;
    struct knurl_static_range_part part;
    size_t members = 0;
    int available;
    memset(out, 0, sizeof(*out));
    if (input == NULL || strlen(input) < 6 ||
        tolower((unsigned char)input[0]) != 'b' ||
        tolower((unsigned char)input[1]) != 'y' ||
        tolower((unsigned char)input[2]) != 't' ||
        tolower((unsigned char)input[3]) != 'e' ||
        tolower((unsigned char)input[4]) != 's' || input[5] != '=')
        return;
    out->requested = 1;
    one.high = 0;
    one.low = 1;
    p = input + 6;
    for (;;) {
        if (++members > KNURL_STATIC_MAX_RANGES)
            goto invalid;
        while (*p == ' ' || *p == '\t')
            ++p;
        available = size.high != 0 || size.low != 0;
        if (*p == '-') {
            ++p;
            if (!number(&p, &suffix))
                goto invalid;
            if (suffix.high == 0 && suffix.low == 0)
                available = 0;
            part.length =
                knurl_asset_compare(suffix, size) >= 0 ? size : suffix;
            part.start = knurl_asset_subtract(size, part.length);
        } else {
            if (!number(&p, &start) || *p != '-')
                goto invalid;
            ++p;
            if (*p >= '0' && *p <= '9') {
                if (!number(&p, &end) || knurl_asset_compare(end, start) < 0)
                    goto invalid;
                if (available && knurl_asset_compare(end, size) >= 0)
                    end = knurl_asset_subtract(size, one);
            } else
                end = available ? knurl_asset_subtract(size, one) : size;
            if (knurl_asset_compare(start, size) >= 0)
                available = 0;
            part.start = start;
            part.length.high = part.length.low = 0;
            if (available) {
                part.length = knurl_asset_subtract(end, start);
                (void)knurl_u64_add(part.length, one, &part.length, NULL);
            }
        }
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p != 0 && *p != ',')
            goto invalid;
        if (available)
            out->parts[out->count++] = part;
        if (*p == 0)
            break;
        ++p;
    }
    out->satisfiable = out->count != 0;
    return;
invalid:
    out->count = 0;
}
