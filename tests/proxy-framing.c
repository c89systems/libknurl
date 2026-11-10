#include "proxy_framing.h"

#include "proxy-check.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct observed_body {
    char bytes[256];
    size_t length;
    size_t capacity;
    size_t trailers;
} observed_body;

static size_t accept_body(void *userdata, const unsigned char *data,
                          size_t length)
{
    observed_body *out;
    size_t available;

    out = (observed_body *)userdata;
    available = out->capacity - out->length;
    if (length > available) {
        length = available;
    }
    PROXY_CHECK(out->length + length <= sizeof(out->bytes));
    memcpy(out->bytes + out->length, data, length);
    out->length += length;
    return length;
}

static int accept_trailer(void *userdata, const char *name, const char *value)
{
    observed_body *out;

    out = (observed_body *)userdata;
    if (strcmp(name, "X-Trace") != 0 || strcmp(value, "done") != 0) {
        return 0;
    }
    out->trailers++;
    return 1;
}

static void test_chunked_split_every_byte(void)
{
    static const char wire[] =
        "3;foo=\"a b\"\r\nabc\r\n2 ; bar = \"x\\\"y\"\r\nde\r\n"
        "0;end=ok\r\nX-Trace: done\r\n\r\nGET /next";
    knurl_proxy_body_framer framer;
    knurl_proxy_frame_result result;
    observed_body out;
    size_t offset;
    size_t consumed;

    memset(&out, 0, sizeof(out));
    out.capacity = sizeof(out.bytes);
    knurl_proxy_body_framer_chunked(&framer);
    offset = 0u;
    do {
        result = knurl_proxy_body_framer_feed(
            &framer, (const unsigned char *)wire + offset, 1u, &consumed,
            accept_body, accept_trailer, &out);
        PROXY_CHECK(result != KNURL_PROXY_FRAME_INVALID);
        PROXY_CHECK(consumed == 1u);
        offset++;
    } while (result != KNURL_PROXY_FRAME_COMPLETE);
    PROXY_CHECK(offset == sizeof(wire) - sizeof("GET /next"));
    PROXY_CHECK(out.length == 5u);
    PROXY_CHECK(memcmp(out.bytes, "abcde", 5u) == 0);
    PROXY_CHECK(out.trailers == 1u);
    result = knurl_proxy_body_framer_feed(
        &framer, (const unsigned char *)wire + offset, sizeof("GET /next") - 1u,
        &consumed, accept_body, accept_trailer, &out);
    PROXY_CHECK(result == KNURL_PROXY_FRAME_COMPLETE && consumed == 0u);
}

static void test_pause_and_resume(void)
{
    static const unsigned char fixed[] = "abcdefghNEXT";
    static const unsigned char chunked[] = "8\r\nabcdefgh\r\n0\r\n\r\nNEXT";
    knurl_proxy_body_framer framer;
    observed_body out;
    knurl_proxy_frame_result result;
    size_t consumed;
    size_t offset;

    memset(&out, 0, sizeof(out));
    out.capacity = 3u;
    knurl_proxy_body_framer_fixed(&framer, 8u);
    result = knurl_proxy_body_framer_feed(&framer, fixed, sizeof(fixed) - 1u,
                                          &consumed, accept_body, NULL, &out);
    PROXY_CHECK(result == KNURL_PROXY_FRAME_PAUSED && consumed == 3u);
    PROXY_CHECK(framer.remaining == 5u);
    out.capacity = sizeof(out.bytes);
    result = knurl_proxy_body_framer_feed(&framer, fixed + consumed,
                                          sizeof(fixed) - 1u - consumed,
                                          &offset, accept_body, NULL, &out);
    PROXY_CHECK(result == KNURL_PROXY_FRAME_COMPLETE && offset == 5u);
    PROXY_CHECK(out.length == 8u && memcmp(out.bytes, "abcdefgh", 8u) == 0);

    memset(&out, 0, sizeof(out));
    out.capacity = 2u;
    knurl_proxy_body_framer_chunked(&framer);
    result = knurl_proxy_body_framer_feed(&framer, chunked,
                                          sizeof(chunked) - 1u, &consumed,
                                          accept_body, accept_trailer, &out);
    PROXY_CHECK(result == KNURL_PROXY_FRAME_PAUSED && consumed == 5u);
    out.capacity = sizeof(out.bytes);
    result = knurl_proxy_body_framer_feed(
        &framer, chunked + consumed, sizeof(chunked) - 1u - consumed, &offset,
        accept_body, accept_trailer, &out);
    PROXY_CHECK(result == KNURL_PROXY_FRAME_COMPLETE);
    PROXY_CHECK(consumed + offset == sizeof(chunked) - sizeof("NEXT"));
    PROXY_CHECK(out.length == 8u && memcmp(out.bytes, "abcdefgh", 8u) == 0);
}

static void test_reject_malformed(void)
{
    static const char *const bad[] = {"\r\n",
                                      "z\r\n",
                                      "10000000000000000\r\n",
                                      "1 \r\na\r\n0\r\n\r\n",
                                      "1;\r\na\r\n0\r\n\r\n",
                                      "1;foo=\r\na\r\n0\r\n\r\n",
                                      "1;foo=\"open\r\na\r\n0\r\n\r\n",
                                      "1;foo=\"bad\001\"\r\na\r\n0\r\n\r\n",
                                      "1;foo=\"bad\\\001\"\r\na\r\n0\r\n\r\n",
                                      "1;foo=bar junk\r\na\r\n0\r\n\r\n",
                                      "1\na\r\n0\r\n\r\n",
                                      "1\r\naX0\r\n\r\n",
                                      "0\r\n\rX",
                                      "0\r\nContent-Length: 3\r\n\r\n",
                                      "0\r\nTransfer-Encoding: chunked\r\n\r\n",
                                      "0\r\nX-Trace : done\r\n\r\n",
                                      "0\r\nX-Trace: nope\r\n\r\n",
                                      "0\r\nX-Trace: bad\001value\r\n\r\n"};
    knurl_proxy_body_framer framer;
    knurl_proxy_frame_result result;
    observed_body out;
    size_t i;
    size_t consumed;

    for (i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memset(&out, 0, sizeof(out));
        out.capacity = sizeof(out.bytes);
        knurl_proxy_body_framer_chunked(&framer);
        result = knurl_proxy_body_framer_feed(
            &framer, (const unsigned char *)bad[i], strlen(bad[i]), &consumed,
            accept_body, accept_trailer, &out);
        PROXY_CHECK(result == KNURL_PROXY_FRAME_INVALID);
        PROXY_CHECK(framer.phase == KNURL_PROXY_PHASE_INVALID);
    }
}

static void test_bounded_lines(void)
{
    knurl_proxy_body_framer framer;
    observed_body out;
    knurl_proxy_frame_result result;
    char wire[KNURL_PROXY_TRAILER_LINE_LIMIT + 10u];
    size_t consumed;

    memset(&out, 0, sizeof(out));
    out.capacity = sizeof(out.bytes);
    memset(wire, 'f', sizeof(wire));
    knurl_proxy_body_framer_chunked(&framer);
    result = knurl_proxy_body_framer_feed(
        &framer, (const unsigned char *)wire, KNURL_PROXY_CHUNK_LINE_LIMIT + 1u,
        &consumed, accept_body, accept_trailer, &out);
    PROXY_CHECK(result == KNURL_PROXY_FRAME_INVALID);

    knurl_proxy_body_framer_chunked(&framer);
    result = knurl_proxy_body_framer_feed(
        &framer, (const unsigned char *)"0\r\n", 3u, &consumed, accept_body,
        accept_trailer, &out);
    PROXY_CHECK(result == KNURL_PROXY_FRAME_MORE);
    wire[0] = 'X';
    wire[1] = '-';
    wire[2] = 'T';
    wire[3] = ':';
    result = knurl_proxy_body_framer_feed(&framer, (const unsigned char *)wire,
                                          KNURL_PROXY_TRAILER_LINE_LIMIT + 1u,
                                          &consumed, accept_body,
                                          accept_trailer, &out);
    PROXY_CHECK(result == KNURL_PROXY_FRAME_INVALID);
}

int main(void)
{
    test_chunked_split_every_byte();
    test_pause_and_resume();
    test_reject_malformed();
    test_bounded_lines();
    puts("proxy framing ok");
    return 0;
}
