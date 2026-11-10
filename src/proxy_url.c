#include "proxy_url.h"

#include "proxy_common.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int knurl_proxy_query_hex(unsigned char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static int knurl_proxy_query_valid(const char *query)
{
    const unsigned char *cursor;
    unsigned char value;
    int hi;
    int lo;

    if (query == NULL) {
        return 1;
    }
    for (cursor = (const unsigned char *)query; *cursor != '\0'; ++cursor) {
        value = *cursor;
        if (value == '#') {
            return 0;
        }
        if (value == '%') {
            if (!knurl_proxy_query_hex(cursor[1]) || cursor[1] == '\0' ||
                !knurl_proxy_query_hex(cursor[2]) || cursor[2] == '\0') {
                return 0;
            }
            hi = cursor[1] <= '9' ? cursor[1] - '0'
                                  : (cursor[1] | 0x20) - 'a' + 10;
            lo = cursor[2] <= '9' ? cursor[2] - '0'
                                  : (cursor[2] | 0x20) - 'a' + 10;
            value = (unsigned char)((hi << 4) | lo);
            cursor += 2u;
        }
        if (value < 0x20u || value == 0x7fu || value == '\\') {
            return 0;
        }
    }
    return 1;
}

knurl_status knurl_proxy_target_build(const char *base_url,
                                      const char *raw_path,
                                      const char *raw_query,
                                      char **request_target, char **authority,
                                      knurl_error *error)
{
    const char *authority_start;
    const char *base_path;
    size_t authority_length;
    size_t base_length;
    size_t path_length;
    size_t query_length;
    size_t joined_length;
    size_t cursor;
    char *target;
    char *host;

    if (request_target != NULL) {
        *request_target = NULL;
    }
    if (authority != NULL) {
        *authority = NULL;
    }
    if (base_url == NULL || raw_path == NULL || request_target == NULL ||
        authority == NULL) {
        knurl_proxy_error(
            error, KNURL_INVALID,
            "proxy target builder requires base URL, path and outputs");
        return KNURL_INVALID;
    }
    if (strncasecmp(base_url, "http://", 7u) == 0) {
        authority_start = base_url + 7u;
    } else if (strncasecmp(base_url, "https://", 8u) == 0) {
        authority_start = base_url + 8u;
    } else {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy target base must use HTTP or HTTPS");
        return KNURL_INVALID;
    }
    base_path = strchr(authority_start, '/');
    authority_length = base_path != NULL ? (size_t)(base_path - authority_start)
                                         : strlen(authority_start);
    if (authority_length == 0u ||
        memchr(authority_start, '@', authority_length) != NULL ||
        memchr(authority_start, '?', authority_length) != NULL ||
        memchr(authority_start, '#', authority_length) != NULL) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy target authority is invalid");
        return KNURL_INVALID;
    }
    if (knurl_proxy_validate_path(raw_path, error) != KNURL_OK) {
        return KNURL_INVALID;
    }
    if (base_path != NULL &&
        knurl_proxy_validate_path(base_path, error) != KNURL_OK) {
        return KNURL_INVALID;
    }
    if (!knurl_proxy_query_valid(raw_query)) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy request query has an unsafe byte or escape");
        return KNURL_INVALID;
    }
    base_length = base_path != NULL ? strlen(base_path) : 0u;
    path_length = strlen(raw_path);
    query_length =
        raw_query != NULL && raw_query[0] != '\0' ? strlen(raw_query) + 1u : 0u;
    /* A trailing base slash and the leading inbound slash join at one byte. */
    if (base_length != 0u && base_path[base_length - 1u] == '/') {
        base_length--;
    }
    if (base_length > SIZE_MAX - path_length ||
        base_length + path_length > SIZE_MAX - query_length - 1u) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy request target is too long");
        return KNURL_INVALID;
    }
    joined_length = base_length + path_length + query_length;
    target = (char *)malloc(joined_length + 1u);
    host = (char *)malloc(authority_length + 1u);
    if (target == NULL || host == NULL) {
        free(target);
        free(host);
        knurl_proxy_error(error, KNURL_NOMEM,
                          "failed to allocate proxy request target");
        return KNURL_NOMEM;
    }
    cursor = 0u;
    if (base_length != 0u) {
        memcpy(target, base_path, base_length);
        cursor = base_length;
    }
    memcpy(target + cursor, raw_path, path_length);
    cursor += path_length;
    if (query_length != 0u) {
        target[cursor++] = '?';
        memcpy(target + cursor, raw_query, query_length - 1u);
        cursor += query_length - 1u;
    }
    target[cursor] = '\0';
    memcpy(host, authority_start, authority_length);
    host[authority_length] = '\0';
    *request_target = target;
    *authority = host;
    knurl_error_clear(error);
    return KNURL_OK;
}
