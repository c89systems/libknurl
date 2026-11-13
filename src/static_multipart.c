#include "static_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct multipart {
    knurl_asset *asset;
    struct knurl_static_ranges ranges;
    char headers[KNURL_STATIC_MAX_RANGES][512];
    size_t lengths[KNURL_STATIC_MAX_RANGES];
    char ending[64];
    size_t ending_length, index, used;
    knurl_u64 position, remaining;
    int phase;
    knurl_status failure;
};
static knurl_status read_part(void *bytes, size_t capacity, size_t *produced,
                              void *data, knurl_error *error)
{
    struct multipart *s = data;
    const char *piece;
    size_t length, amount;
    knurl_u64 count;
    knurl_status status;
    *produced = 0;
    if (s->phase == 4)
        return KNURL_EOF;
    if (s->phase == 1) {
        if (knurl_u64_to_size(s->remaining, &amount, NULL) == KNURL_OK &&
            capacity > amount)
            capacity = amount;
        status = knurl_asset_read_owned(s->asset, s, s->position, bytes,
                                        capacity, produced, error);
        if (status != KNURL_OK) {
            if (status != KNURL_AGAIN)
                s->failure = status;
            return status;
        }
        (void)knurl_u64_from_size(*produced, &count, NULL);
        (void)knurl_u64_add(s->position, count, &s->position, NULL);
        s->remaining = knurl_asset_subtract(s->remaining, count);
        if (s->remaining.high == 0 && s->remaining.low == 0)
            s->phase = 2;
        return KNURL_OK;
    }
    piece = s->phase == 0   ? s->headers[s->index]
            : s->phase == 2 ? "\r\n"
                            : s->ending;
    length = s->phase == 0   ? s->lengths[s->index]
             : s->phase == 2 ? 2
                             : s->ending_length;
    amount = length - s->used;
    if (amount > capacity)
        amount = capacity;
    memcpy(bytes, piece + s->used, amount);
    s->used += amount;
    *produced = amount;
    if (s->used == length) {
        s->used = 0;
        if (s->phase == 0) {
            s->position = s->ranges.parts[s->index].start;
            s->remaining = s->ranges.parts[s->index].length;
            s->phase = 1;
        } else if (s->phase == 2)
            s->phase = ++s->index < s->ranges.count ? 0 : 3;
        else
            s->phase = 4;
    }
    return KNURL_OK;
}
static knurl_status reset(void *data, knurl_error *error)
{
    struct multipart *s = data;
    if (s->failure != KNURL_OK)
        return knurl_fail(error, s->failure, "static_multipart",
                          "asset previously failed");
    s->phase = 0;
    s->index = s->used = 0;
    return KNURL_OK;
}
static void close_part(void *data, knurl_status outcome)
{
    struct multipart *s = data;
    (void)outcome;
    knurl_asset_unclaim(s->asset, s);
    (void)knurl_asset_close(s->asset, NULL);
    free(s);
}
knurl_status
knurl_static_multipart_take(knurl_asset *asset,
                            const struct knurl_static_ranges *ranges,
                            const char *type, const char *boundary,
                            knurl_source **out, knurl_error *error)
{
    const knurl_asset_info *info = knurl_asset_stat(asset);
    struct multipart *s;
    knurl_source_config config;
    knurl_u64 total = {0, 0}, size, end, one = {0, 1};
    knurl_status status = KNURL_OK;
    size_t i, written;
    char full[21], first[21], last[21];
    int length;
    if (out == NULL || info == NULL || ranges == NULL || ranges->count < 2 ||
        ranges->count > KNURL_STATIC_MAX_RANGES || type == NULL ||
        boundary == NULL)
        return knurl_fail(error, KNURL_INVALID, "static_multipart",
                          "provide file, ranges and output");
    *out = NULL;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return knurl_fail(error, KNURL_NOMEM, "static_multipart",
                          "cannot allocate range source");
    s->ranges = *ranges;
    s->asset = asset;
    (void)knurl_u64_format_decimal(info->size, full, sizeof(full), &written,
                                   NULL);
    for (i = 0; i < ranges->count; ++i) {
        status = knurl_u64_add(ranges->parts[i].start, ranges->parts[i].length,
                               &end, error);
        if (status != KNURL_OK || knurl_asset_compare(end, info->size) > 0 ||
            (ranges->parts[i].length.high == 0 &&
             ranges->parts[i].length.low == 0)) {
            status = knurl_fail(error, KNURL_INVALID, "static_multipart",
                                "invalid asset range");
            goto failed;
        }
        end = knurl_asset_subtract(end, one);
        (void)knurl_u64_format_decimal(ranges->parts[i].start, first,
                                       sizeof(first), &written, NULL);
        (void)knurl_u64_format_decimal(end, last, sizeof(last), &written, NULL);
        length = snprintf(
            s->headers[i], sizeof(s->headers[i]),
            "--%s\r\nContent-Type: %s\r\nContent-Range: bytes %s-%s/%s\r\n\r\n",
            boundary, type, first, last, full);
        if (length < 0 || (size_t)length >= sizeof(s->headers[i])) {
            status = knurl_fail(error, KNURL_LIMIT, "static_multipart",
                                "part metadata exceeds bound");
            goto failed;
        }
        s->lengths[i] = (size_t)length;
        (void)knurl_u64_from_size((size_t)length + 2, &size, NULL);
        status = knurl_u64_add(total, size, &total, error);
        if (status == KNURL_OK)
            status =
                knurl_u64_add(total, ranges->parts[i].length, &total, error);
        if (status != KNURL_OK)
            goto failed;
    }
    length = snprintf(s->ending, sizeof(s->ending), "--%s--\r\n", boundary);
    if (length < 0 || (size_t)length >= sizeof(s->ending)) {
        status = knurl_fail(error, KNURL_LIMIT, "static_multipart",
                            "boundary exceeds bound");
        goto failed;
    }
    s->ending_length = (size_t)length;
    (void)knurl_u64_from_size(s->ending_length, &size, NULL);
    status = knurl_u64_add(total, size, &total, error);
    if (status != KNURL_OK)
        goto failed;
    knurl_source_config_init(&config);
    config.read = read_part;
    config.reset = reset;
    config.close = close_part;
    config.userdata = s;
    config.has_length = 1;
    config.length = total;
    status = knurl_asset_source_claim(asset, s, error);
    if (status != KNURL_OK)
        goto failed;
    status = knurl_source_new(&config, out, error);
    if (status == KNURL_OK)
        return status;
    knurl_asset_unclaim(asset, s);
failed:
    free(s);
    return status;
}
