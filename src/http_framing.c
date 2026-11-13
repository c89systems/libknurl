#include "http_framing.h"

#include <stdint.h>
#include <string.h>

static int knurl_http_token_char(unsigned char c)
{
    if (c == 0u) {
        return 0;
    }
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z')) {
        return 1;
    }
    return strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int knurl_http_chunk_quoted(const char *line, size_t length,
                                   size_t *offset)
{
    unsigned char c;

    ++*offset;
    while (*offset < length) {
        c = (unsigned char)line[(*offset)++];
        if (c == '"') {
            return 1;
        }
        if (c == '\\') {
            if (*offset == length) {
                return 0;
            }
            c = (unsigned char)line[(*offset)++];
            if (c == '\t' || c == ' ' || (c >= 0x21u && c != 0x7fu)) {
                continue;
            }
            return 0;
        }
        if (c != '\t' && c != ' ' && c != '!' &&
            (c < '#' || (c > '[' && c < ']') || c == 0x7fu)) {
            return 0;
        }
    }
    return 0;
}

static int knurl_http_chunk_extensions(const char *line, size_t length,
                                       size_t offset)
{
    size_t start;

    while (offset < length) {
        while (offset < length &&
               (line[offset] == ' ' || line[offset] == '\t')) {
            offset++;
        }
        if (offset == length || line[offset++] != ';') {
            return 0;
        }
        while (offset < length &&
               (line[offset] == ' ' || line[offset] == '\t')) {
            offset++;
        }
        start = offset;
        while (offset < length &&
               knurl_http_token_char((unsigned char)line[offset])) {
            offset++;
        }
        if (offset == start) {
            return 0;
        }
        while (offset < length &&
               (line[offset] == ' ' || line[offset] == '\t')) {
            offset++;
        }
        if (offset < length && line[offset] == '=') {
            offset++;
            while (offset < length &&
                   (line[offset] == ' ' || line[offset] == '\t')) {
                offset++;
            }
            if (offset < length && line[offset] == '"') {
                if (!knurl_http_chunk_quoted(line, length, &offset)) {
                    return 0;
                }
            } else {
                start = offset;
                while (offset < length &&
                       knurl_http_token_char((unsigned char)line[offset])) {
                    offset++;
                }
                if (offset == start) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

int knurl_http_trailer_field_allowed(const char *name, size_t length)
{
    static const char *const forbidden[] = {"authorization",
                                            "authentication-info",
                                            "cache-control",
                                            "connection",
                                            "content-disposition",
                                            "content-encoding",
                                            "content-language",
                                            "content-length",
                                            "content-location",
                                            "content-range",
                                            "content-type",
                                            "cookie",
                                            "expect",
                                            "expires",
                                            "host",
                                            "if-match",
                                            "if-modified-since",
                                            "if-none-match",
                                            "if-range",
                                            "if-unmodified-since",
                                            "keep-alive",
                                            "max-forwards",
                                            "pragma",
                                            "proxy-authenticate",
                                            "proxy-authentication-info",
                                            "proxy-authorization",
                                            "proxy-connection",
                                            "range",
                                            "set-cookie",
                                            "te",
                                            "trailer",
                                            "transfer-encoding",
                                            "upgrade",
                                            "www-authenticate"};
    size_t i;
    size_t j;

    if (name == NULL || length == 0u) {
        return 0;
    }
    for (j = 0u; j < length; ++j) {
        if (!knurl_http_token_char((unsigned char)name[j])) {
            return 0;
        }
    }
    for (i = 0u; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        const char *expected = forbidden[i];
        if (strlen(expected) == length) {
            for (j = 0u; j < length; ++j) {
                unsigned char a = (unsigned char)name[j];
                unsigned char b = (unsigned char)expected[j];
                if (a >= 'A' && a <= 'Z') {
                    a = (unsigned char)(a + ('a' - 'A'));
                }
                if (a != b) {
                    break;
                }
            }
            if (j == length) {
                return 0;
            }
        }
    }
    return 1;
}

static int knurl_http_parse_size(knurl_http_body_framer *framer)
{
    const char *line;
    size_t i;
    size_t digits;
    uint64_t value;
    unsigned digit;
    unsigned char c;

    line = framer->line;
    value = 0u;
    digits = 0u;
    for (i = 0u; i < framer->line_length; ++i) {
        c = (unsigned char)line[i];
        if (c >= '0' && c <= '9') {
            digit = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned)(c - 'A' + 10);
        } else {
            break;
        }
        if (value > (((uint64_t)-1) - digit) / 16u) {
            return 0;
        }
        value = value * 16u + digit;
        digits++;
    }
    if (digits == 0u) {
        return 0;
    }
    if (i < framer->line_length) {
        if (!knurl_http_chunk_extensions(line, framer->line_length, i)) {
            return 0;
        }
    }
    framer->remaining = value;
    framer->phase =
        value == 0u ? KNURL_HTTP_PHASE_TRAILERS : KNURL_HTTP_PHASE_CHUNK_DATA;
    return 1;
}

static int knurl_http_parse_trailer(knurl_http_body_framer *framer,
                                    knurl_http_trailer_accept_fn callback,
                                    void *userdata)
{
    char *line;
    size_t i;
    size_t end;
    char *value;

    if (framer->line_length == 0u) {
        framer->phase = KNURL_HTTP_PHASE_COMPLETE;
        return 1;
    }
    if (++framer->trailer_count > KNURL_HTTP_TRAILER_COUNT_LIMIT) {
        return 0;
    }
    line = framer->line;
    for (i = 0u; i < framer->line_length && line[i] != ':'; ++i) {
        if (!knurl_http_token_char((unsigned char)line[i])) {
            return 0;
        }
    }
    if (i == 0u || i == framer->line_length) {
        return 0;
    }
    line[i++] = '\0';
    if (!knurl_http_trailer_field_allowed(line, strlen(line))) {
        return 0;
    }
    for (; i < framer->line_length && (line[i] == ' ' || line[i] == '\t');
         ++i) {
    }
    value = line + i;
    end = framer->line_length;
    while (end > i && (line[end - 1u] == ' ' || line[end - 1u] == '\t')) {
        end--;
    }
    for (; i < end; ++i) {
        unsigned char c = (unsigned char)line[i];
        if ((c < 0x20u && c != '\t') || c == 0x7fu) {
            return 0;
        }
    }
    line[end] = '\0';
    return callback != NULL && callback(userdata, line, value);
}

void knurl_http_body_framer_fixed(knurl_http_body_framer *framer,
                                  uint64_t length)
{
    if (framer == NULL) {
        return;
    }
    memset(framer, 0, sizeof(*framer));
    framer->remaining = length;
    framer->phase =
        length == 0u ? KNURL_HTTP_PHASE_COMPLETE : KNURL_HTTP_PHASE_FIXED;
}

void knurl_http_body_framer_chunked(knurl_http_body_framer *framer)
{
    if (framer == NULL) {
        return;
    }
    memset(framer, 0, sizeof(*framer));
    framer->phase = KNURL_HTTP_PHASE_CHUNK_SIZE;
}

knurl_http_frame_result knurl_http_body_framer_feed(
    knurl_http_body_framer *framer, const unsigned char *data, size_t length,
    size_t *consumed, knurl_http_body_accept_fn body,
    knurl_http_trailer_accept_fn trailer, void *userdata)
{
    size_t pos;
    size_t offer;
    size_t accepted;
    unsigned char c;
    int line_complete;
    size_t limit;

    if (consumed != NULL) {
        *consumed = 0u;
    }
    if (framer == NULL || consumed == NULL || (data == NULL && length != 0u)) {
        return KNURL_HTTP_FRAME_INVALID;
    }
    if (framer->phase == KNURL_HTTP_PHASE_INVALID) {
        return KNURL_HTTP_FRAME_INVALID;
    }
    if (framer->phase == KNURL_HTTP_PHASE_COMPLETE) {
        return KNURL_HTTP_FRAME_COMPLETE;
    }
    pos = 0u;
    while (pos < length) {
        if (framer->phase == KNURL_HTTP_PHASE_FIXED ||
            framer->phase == KNURL_HTTP_PHASE_CHUNK_DATA) {
            offer = length - pos;
            if (framer->remaining < (uint64_t)offer) {
                offer = (size_t)framer->remaining;
            }
            if (body == NULL || offer == 0u) {
                goto invalid;
            }
            accepted = body(userdata, data + pos, offer);
            if (accepted > offer) {
                goto invalid;
            }
            pos += accepted;
            framer->remaining -= (uint64_t)accepted;
            if (framer->remaining == 0u) {
                framer->phase = framer->phase == KNURL_HTTP_PHASE_FIXED
                                    ? KNURL_HTTP_PHASE_COMPLETE
                                    : KNURL_HTTP_PHASE_CHUNK_DATA_CR;
            }
            if (framer->phase == KNURL_HTTP_PHASE_COMPLETE) {
                *consumed = pos;
                return KNURL_HTTP_FRAME_COMPLETE;
            }
            if (accepted < offer) {
                *consumed = pos;
                return KNURL_HTTP_FRAME_PAUSED;
            }
            continue;
        }
        c = data[pos++];
        if (framer->phase == KNURL_HTTP_PHASE_CHUNK_DATA_CR) {
            if (c != '\r') {
                goto invalid;
            }
            framer->phase = KNURL_HTTP_PHASE_CHUNK_DATA_LF;
            continue;
        }
        if (framer->phase == KNURL_HTTP_PHASE_CHUNK_DATA_LF) {
            if (c != '\n') {
                goto invalid;
            }
            framer->phase = KNURL_HTTP_PHASE_CHUNK_SIZE;
            continue;
        }
        if (framer->phase != KNURL_HTTP_PHASE_CHUNK_SIZE &&
            framer->phase != KNURL_HTTP_PHASE_TRAILERS) {
            goto invalid;
        }
        if (framer->phase == KNURL_HTTP_PHASE_TRAILERS) {
            if (framer->trailer_bytes == KNURL_HTTP_TRAILER_BLOCK_LIMIT) {
                goto invalid;
            }
            framer->trailer_bytes++;
        }
        limit = framer->phase == KNURL_HTTP_PHASE_CHUNK_SIZE
                    ? KNURL_HTTP_CHUNK_LINE_LIMIT
                    : KNURL_HTTP_TRAILER_LINE_LIMIT;
        line_complete = 0;
        if (framer->line_cr) {
            if (c != '\n') {
                goto invalid;
            }
            framer->line_cr = 0;
            line_complete = 1;
        } else if (c == '\r') {
            framer->line_cr = 1;
        } else if (c == '\n' || c == '\0' || framer->line_length == limit) {
            goto invalid;
        } else {
            framer->line[framer->line_length++] = (char)c;
        }
        if (line_complete) {
            framer->line[framer->line_length] = '\0';
            if (framer->phase == KNURL_HTTP_PHASE_CHUNK_SIZE) {
                if (!knurl_http_parse_size(framer)) {
                    goto invalid;
                }
            } else if (!knurl_http_parse_trailer(framer, trailer, userdata)) {
                goto invalid;
            }
            framer->line_length = 0u;
        }
        if (framer->phase == KNURL_HTTP_PHASE_COMPLETE) {
            *consumed = pos;
            return KNURL_HTTP_FRAME_COMPLETE;
        }
    }
    *consumed = pos;
    if (framer->phase == KNURL_HTTP_PHASE_COMPLETE) {
        return KNURL_HTTP_FRAME_COMPLETE;
    }
    return KNURL_HTTP_FRAME_MORE;
invalid:
    *consumed = pos;
    framer->phase = KNURL_HTTP_PHASE_INVALID;
    return KNURL_HTTP_FRAME_INVALID;
}
