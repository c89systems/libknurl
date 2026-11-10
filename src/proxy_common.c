#include "proxy_common.h"
#include <knurl/proxy.h>
#include <string.h>

int knurl_proxy_format_length(uint64_t value, char *text)
{
    knurl_u64 wide;
    size_t written;
    wide.high = (unsigned long)(value >> 32);
    wide.low = (unsigned long)(value & 0xffffffffUL);
    return knurl_u64_format_decimal(wide, text, 21, &written, NULL) == KNURL_OK;
}

void knurl_proxy_error(knurl_error *error, knurl_status status,
                       const char *message)
{
    (void)knurl_fail(error, status, "proxy", message);
}

static int knurl_proxy_hex_digit(unsigned char value)
{
    if (value >= (unsigned char)'0' && value <= (unsigned char)'9') {
        return (int)(value - (unsigned char)'0');
    }
    if (value >= (unsigned char)'a' && value <= (unsigned char)'f') {
        return (int)(value - (unsigned char)'a') + 10;
    }
    if (value >= (unsigned char)'A' && value <= (unsigned char)'F') {
        return (int)(value - (unsigned char)'A') + 10;
    }
    return -1;
}

knurl_status knurl_proxy_validate_path(const char *path, knurl_error *error)
{
    const unsigned char *cursor;
    unsigned char segment[2];
    unsigned char value;
    unsigned char previous;
    unsigned char before_previous;
    size_t segment_size;
    int high;
    int low;

    if (path == NULL || path[0] != '/') {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy request path must start with '/'");
        return KNURL_INVALID;
    }
    cursor = (const unsigned char *)path;
    segment_size = 0u;
    previous = 0u;
    before_previous = 0u;
    for (; *cursor != '\0'; ++cursor) {
        value = *cursor;
        if (value == (unsigned char)'%') {
            if (cursor[1] == '\0' || cursor[2] == '\0' ||
                (high = knurl_proxy_hex_digit(cursor[1])) < 0 ||
                (low = knurl_proxy_hex_digit(cursor[2])) < 0) {
                knurl_proxy_error(
                    error, KNURL_INVALID,
                    "proxy request path has an invalid percent escape");
                return KNURL_INVALID;
            }
            value = (unsigned char)((high << 4) | low);
            cursor += 2u;
        } else if (value == (unsigned char)'?' || value == (unsigned char)'#') {
            knurl_proxy_error(
                error, KNURL_INVALID,
                "proxy request path must not contain a query or fragment");
            return KNURL_INVALID;
        }
        if (value < 0x20u || value == 0x7fu || value == (unsigned char)'\\') {
            knurl_proxy_error(error, KNURL_INVALID,
                              "proxy request path has an unsafe byte");
            return KNURL_INVALID;
        }
        if (before_previous == (unsigned char)'%' &&
            knurl_proxy_hex_digit(previous) >= 0 &&
            knurl_proxy_hex_digit(value) >= 0) {
            knurl_proxy_error(
                error, KNURL_INVALID,
                "proxy request path has ambiguous double escaping");
            return KNURL_INVALID;
        }
        before_previous = previous;
        previous = value;
        if (value == (unsigned char)'/') {
            if ((segment_size == 1u && segment[0] == (unsigned char)'.') ||
                (segment_size == 2u && segment[0] == (unsigned char)'.' &&
                 segment[1] == (unsigned char)'.')) {
                knurl_proxy_error(error, KNURL_INVALID,
                                  "proxy request path has a dot segment");
                return KNURL_INVALID;
            }
            segment_size = 0u;
        } else {
            if (segment_size < 2u) {
                segment[segment_size] = value;
            }
            if (segment_size < 3u) {
                segment_size++;
            }
        }
    }
    if ((segment_size == 1u && segment[0] == (unsigned char)'.') ||
        (segment_size == 2u && segment[0] == (unsigned char)'.' &&
         segment[1] == (unsigned char)'.')) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy request path has a dot segment");
        return KNURL_INVALID;
    }
    return KNURL_OK;
}

void knurl_proxy_config_init(knurl_proxy_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}
