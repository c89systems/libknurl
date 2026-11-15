#include "http_framing.h"
#include "proxy_headers.h"

#include "proxy-check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add(knurl_proxy_headers *headers, const char *name,
                const char *value)
{
    PROXY_CHECK(knurl_proxy_headers_add(headers, name, value) ==
                KNURL_PROXY_HEADER_OK);
}

static int contains(const knurl_proxy_headers *headers, const char *name,
                    const char *value)
{
    size_t i;

    for (i = 0u; i < headers->count; ++i) {
        if (strcmp(headers->fields[i].name, name) == 0 &&
            strcmp(headers->fields[i].value, value) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_sanitized_request(void)
{
    knurl_proxy_headers inbound;
    knurl_proxy_headers outbound;
    knurl_proxy_request_head head;
    const char *reason;

    knurl_proxy_headers_init(&inbound);
    knurl_proxy_headers_init(&outbound);
    add(&inbound, "Host", "public.example:8443");
    add(&inbound, "Connection", "keep-alive, X-Secret");
    add(&inbound, "Content-Length", "0005");
    add(&inbound, "content-length", "5");
    add(&inbound, "X-Secret", "strip me");
    add(&inbound, "Forwarded", "for=spoofed");
    add(&inbound, "X-Forwarded-For", "1.2.3.4");
    add(&inbound, "X-Real-IP", "1.2.3.4");
    add(&inbound, "Expect", "100-continue");
    add(&inbound, "Content-Type", "application/octet-stream");
    add(&inbound, "X-Multi", "first");
    add(&inbound, "X-Multi", "second");

    reason = "stale";
    PROXY_CHECK(knurl_proxy_request_head_parse(&inbound, &head, &reason) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(reason == NULL);
    PROXY_CHECK(strcmp(head.host, "public.example:8443") == 0);
    PROXY_CHECK(head.has_content_length && head.content_length == 5u);
    PROXY_CHECK(head.expect_continue && !head.chunked &&
                !head.websocket_upgrade);
    PROXY_CHECK(knurl_proxy_headers_sanitize_request(&inbound, &outbound) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(outbound.count == 3u);
    PROXY_CHECK(
        contains(&outbound, "Content-Type", "application/octet-stream"));
    PROXY_CHECK(contains(&outbound, "X-Multi", "first"));
    PROXY_CHECK(contains(&outbound, "X-Multi", "second"));
    knurl_proxy_headers_cleanup(&outbound);
    knurl_proxy_headers_cleanup(&inbound);
}

static void test_chunked_trailer_and_websocket(void)
{
    knurl_proxy_headers inbound;
    knurl_proxy_request_head head;

    knurl_proxy_headers_init(&inbound);
    add(&inbound, "Host", "[::1]:8080");
    add(&inbound, "Transfer-Encoding", "chunked");
    add(&inbound, "Trailer", "X-Trace, X-Signature");
    PROXY_CHECK(knurl_proxy_request_head_parse(&inbound, &head, NULL) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(head.chunked && head.has_trailers);
    PROXY_CHECK(knurl_proxy_request_trailer_declared(&inbound, "x-trace"));
    PROXY_CHECK(knurl_proxy_request_trailer_declared(&inbound, "X-Signature"));
    PROXY_CHECK(!knurl_proxy_request_trailer_declared(&inbound, "X-Other"));
    PROXY_CHECK(
        !knurl_proxy_request_trailer_declared(&inbound, "Content-Length"));
    knurl_proxy_headers_cleanup(&inbound);

    knurl_proxy_headers_init(&inbound);
    add(&inbound, "Host", "svc_name.local:8080");
    PROXY_CHECK(knurl_proxy_request_head_parse(&inbound, &head, NULL) ==
                KNURL_PROXY_HEADER_OK);
    knurl_proxy_headers_cleanup(&inbound);

    knurl_proxy_headers_init(&inbound);
    add(&inbound, "Host", "backend.local");
    add(&inbound, "Connection", "keep-alive, Upgrade");
    add(&inbound, "Upgrade", "websocket");
    add(&inbound, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    PROXY_CHECK(knurl_proxy_request_head_parse(&inbound, &head, NULL) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(head.websocket_upgrade);
    knurl_proxy_headers_cleanup(&inbound);
}

static size_t consume_body(void *userdata, const unsigned char *data,
                           size_t length)
{
    (void)userdata;
    (void)data;
    return length;
}

static int declared_trailer(void *userdata, const char *name, const char *value)
{
    (void)value;
    return knurl_proxy_request_trailer_declared(
        (const knurl_proxy_headers *)userdata, name);
}

static void test_trailer_handoff(void)
{
    static const unsigned char allowed[] = "0\r\nX-Trace: done\r\n\r\nNEXT";
    static const unsigned char undeclared[] = "0\r\nX-Other: bad\r\n\r\n";
    knurl_proxy_headers inbound;
    knurl_http_body_framer framer;
    knurl_proxy_request_head head;
    size_t consumed;

    knurl_proxy_headers_init(&inbound);
    add(&inbound, "Host", "example.test");
    add(&inbound, "Transfer-Encoding", "chunked");
    add(&inbound, "Trailer", "X-Trace");
    PROXY_CHECK(knurl_proxy_request_head_parse(&inbound, &head, NULL) ==
                KNURL_PROXY_HEADER_OK);
    knurl_http_body_framer_chunked(&framer);
    PROXY_CHECK(
        knurl_http_body_framer_feed(&framer, allowed, sizeof(allowed) - 1u,
                                    &consumed, consume_body, declared_trailer,
                                    &inbound) == KNURL_HTTP_FRAME_COMPLETE);
    PROXY_CHECK(consumed == sizeof(allowed) - sizeof("NEXT"));

    knurl_http_body_framer_chunked(&framer);
    PROXY_CHECK(knurl_http_body_framer_feed(
                    &framer, undeclared, sizeof(undeclared) - 1u, &consumed,
                    consume_body, declared_trailer,
                    &inbound) == KNURL_HTTP_FRAME_INVALID);
    knurl_proxy_headers_cleanup(&inbound);
}

static void test_rejected_framing(void)
{
    struct case_entry {
        const char *name;
        const char *value;
        knurl_proxy_header_status expected;
    } cases[] = {
        {"Content-Length", "-1", KNURL_PROXY_HEADER_INVALID},
        {"Content-Length", "18446744073709551616", KNURL_PROXY_HEADER_INVALID},
        {"Content-Length", "3, 3", KNURL_PROXY_HEADER_INVALID},
        {"Transfer-Encoding", "gzip, chunked", KNURL_PROXY_HEADER_INVALID},
        {"Transfer-Encoding", "chunked", KNURL_PROXY_HEADER_INVALID},
        {"Expect", "fancy", KNURL_PROXY_HEADER_INVALID},
        {"Trailer", "Content-Length", KNURL_PROXY_HEADER_INVALID},
        {"Connection", "Content-Length", KNURL_PROXY_HEADER_INVALID},
        {"Connection", "X-Good,", KNURL_PROXY_HEADER_INVALID},
        {"Connection", "Upgrade", KNURL_PROXY_HEADER_UNSUPPORTED_UPGRADE},
        {"Upgrade", "h2c", KNURL_PROXY_HEADER_UNSUPPORTED_UPGRADE},
        {"Upgrade", "websocket, h2c", KNURL_PROXY_HEADER_UNSUPPORTED_UPGRADE}};
    knurl_proxy_headers inbound;
    knurl_proxy_request_head head;
    const char *reason;
    size_t i;

    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        knurl_proxy_headers_init(&inbound);
        add(&inbound, "Host", "example.test");
        if (strcmp(cases[i].name, "Transfer-Encoding") == 0 &&
            strcmp(cases[i].value, "chunked") == 0) {
            add(&inbound, "Content-Length", "5");
        }
        add(&inbound, cases[i].name, cases[i].value);
        reason = NULL;
        PROXY_CHECK(knurl_proxy_request_head_parse(&inbound, &head, &reason) ==
                    cases[i].expected);
        PROXY_CHECK(reason != NULL && reason[0] != '\0');
        knurl_proxy_headers_cleanup(&inbound);
    }
    knurl_proxy_headers_init(&inbound);
    add(&inbound, "Host", "example.test");
    add(&inbound, "Content-Length", "5");
    add(&inbound, "Content-Length", "6");
    PROXY_CHECK(knurl_proxy_request_head_parse(&inbound, &head, NULL) ==
                KNURL_PROXY_HEADER_INVALID);
    knurl_proxy_headers_cleanup(&inbound);

    {
        static const char *const invalid_host[] = {
            "bad:port", "[]",        "[::1",         "foo/bar",
            "foo@evil", "bad..name", "-bad.example", "bad-.example",
            "name:0",   "name:65536"};
        for (i = 0u; i < sizeof(invalid_host) / sizeof(invalid_host[0]); ++i) {
            knurl_proxy_headers_init(&inbound);
            add(&inbound, "Host", invalid_host[i]);
            PROXY_CHECK(knurl_proxy_request_head_parse(&inbound, &head, NULL) ==
                        KNURL_PROXY_HEADER_INVALID);
            knurl_proxy_headers_cleanup(&inbound);
        }
    }
}

static void test_header_limits(void)
{
    knurl_proxy_headers inbound;
    char *large;
    size_t i;

    knurl_proxy_headers_init(&inbound);
    PROXY_CHECK(knurl_proxy_headers_add(&inbound, "Bad Name", "x") ==
                KNURL_PROXY_HEADER_INVALID);
    PROXY_CHECK(knurl_proxy_headers_add(&inbound, "X-Test", "bad\r\nvalue") ==
                KNURL_PROXY_HEADER_INVALID);
    for (i = 0u; i < KNURL_PROXY_HEADER_COUNT_LIMIT; ++i) {
        add(&inbound, "X-Test", "x");
    }
    PROXY_CHECK(knurl_proxy_headers_add(&inbound, "X-Test", "x") ==
                KNURL_PROXY_HEADER_LIMIT);
    knurl_proxy_headers_cleanup(&inbound);

    large = (char *)malloc(KNURL_PROXY_HEADER_BLOCK_LIMIT + 1u);
    PROXY_CHECK(large != NULL);
    memset(large, 'a', KNURL_PROXY_HEADER_BLOCK_LIMIT);
    large[KNURL_PROXY_HEADER_BLOCK_LIMIT] = '\0';
    PROXY_CHECK(knurl_proxy_headers_add(&inbound, "X-Test", large) ==
                KNURL_PROXY_HEADER_LIMIT);
    free(large);
    knurl_proxy_headers_cleanup(&inbound);
}

static void test_replacement_limits(void)
{
    knurl_proxy_headers headers;
    char name[32];
    size_t i;
    size_t bytes;

    knurl_proxy_headers_init(&headers);
    for (i = 0u; i < KNURL_PROXY_HEADER_COUNT_LIMIT; ++i) {
        (void)snprintf(name, sizeof(name), "X-%lu", (unsigned long)i);
        add(&headers, name, "original");
    }
    bytes = headers.bytes;
    PROXY_CHECK(knurl_proxy_headers_set(&headers, "X-New", "value") ==
                KNURL_PROXY_HEADER_LIMIT);
    PROXY_CHECK(headers.count == KNURL_PROXY_HEADER_COUNT_LIMIT);
    PROXY_CHECK(headers.bytes == bytes);
    PROXY_CHECK(contains(&headers, "X-0", "original"));
    PROXY_CHECK(knurl_proxy_headers_set(&headers, "X-0", "bad\r\nvalue") ==
                KNURL_PROXY_HEADER_INVALID);
    PROXY_CHECK(headers.bytes == bytes);
    PROXY_CHECK(contains(&headers, "X-0", "original"));
    PROXY_CHECK(knurl_proxy_headers_set(&headers, headers.fields[0].name,
                                        headers.fields[0].value) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(headers.count == KNURL_PROXY_HEADER_COUNT_LIMIT);
    PROXY_CHECK(headers.bytes == bytes);
    PROXY_CHECK(contains(&headers, "X-0", "original"));
    knurl_proxy_headers_cleanup(&headers);
}

int main(void)
{
    test_sanitized_request();
    test_chunked_trailer_and_websocket();
    test_trailer_handoff();
    test_rejected_framing();
    test_header_limits();
    test_replacement_limits();
    puts("proxy headers ok");
    return 0;
}
