#include "proxy_common.h"
#include <rotmos/proxy.h>
#include <string.h>

int rotmos_proxy_format_length(uint64_t value, char *text)
{
    rotmos_u64 wide;
    size_t written;
    wide.high = (unsigned long)(value >> 32);
    wide.low = (unsigned long)(value & 0xffffffffUL);
    return rotmos_u64_format_decimal(wide, text, 21, &written, NULL) ==
           ROTMOS_OK;
}

void rotmos_proxy_error(rotmos_error *error, rotmos_status status,
                        const char *message)
{
    (void)rotmos_fail(error, status, "proxy", message);
}

static int rotmos_proxy_hex_digit(unsigned char value)
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

rotmos_status rotmos_proxy_validate_path(const char *path, rotmos_error *error)
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
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy request path must start with '/'");
        return ROTMOS_INVALID;
    }
    cursor = (const unsigned char *)path;
    segment_size = 0u;
    previous = 0u;
    before_previous = 0u;
    for (; *cursor != '\0'; ++cursor) {
        value = *cursor;
        if (value == (unsigned char)'%') {
            if (cursor[1] == '\0' || cursor[2] == '\0' ||
                (high = rotmos_proxy_hex_digit(cursor[1])) < 0 ||
                (low = rotmos_proxy_hex_digit(cursor[2])) < 0) {
                rotmos_proxy_error(
                    error, ROTMOS_INVALID,
                    "proxy request path has an invalid percent escape");
                return ROTMOS_INVALID;
            }
            value = (unsigned char)((high << 4) | low);
            cursor += 2u;
        } else if (value == (unsigned char)'?' || value == (unsigned char)'#') {
            rotmos_proxy_error(
                error, ROTMOS_INVALID,
                "proxy request path must not contain a query or fragment");
            return ROTMOS_INVALID;
        }
        if (value < 0x20u || value == 0x7fu || value == (unsigned char)'\\') {
            rotmos_proxy_error(error, ROTMOS_INVALID,
                               "proxy request path has an unsafe byte");
            return ROTMOS_INVALID;
        }
        if (before_previous == (unsigned char)'%' &&
            rotmos_proxy_hex_digit(previous) >= 0 &&
            rotmos_proxy_hex_digit(value) >= 0) {
            rotmos_proxy_error(
                error, ROTMOS_INVALID,
                "proxy request path has ambiguous double escaping");
            return ROTMOS_INVALID;
        }
        before_previous = previous;
        previous = value;
        if (value == (unsigned char)'/') {
            if ((segment_size == 1u && segment[0] == (unsigned char)'.') ||
                (segment_size == 2u && segment[0] == (unsigned char)'.' &&
                 segment[1] == (unsigned char)'.')) {
                rotmos_proxy_error(error, ROTMOS_INVALID,
                                   "proxy request path has a dot segment");
                return ROTMOS_INVALID;
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
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy request path has a dot segment");
        return ROTMOS_INVALID;
    }
    return ROTMOS_OK;
}

void rotmos_proxy_config_init(rotmos_proxy_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}
