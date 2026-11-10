#include "proxy_http.h"

#include "proxy-check.h"
#include <string.h>

static knurl_proxy_http_event accept(knurl_proxy_http_response *response,
                                     const char *line)
{
    knurl_proxy_http_event event;
    const char *reason;

    reason = NULL;
    PROXY_CHECK(knurl_proxy_http_response_header(response, line, strlen(line),
                                                 &event, &reason) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(reason == NULL);
    return event;
}

static void reject(knurl_proxy_http_response *response, const char *line,
                   knurl_proxy_header_status expected)
{
    knurl_proxy_http_event event;
    const char *reason;

    reason = NULL;
    PROXY_CHECK(knurl_proxy_http_response_header(response, line, strlen(line),
                                                 &event, &reason) == expected);
    PROXY_CHECK(reason != NULL);
}

static void test_fixed_length(void)
{
    knurl_proxy_http_response response;
    knurl_proxy_headers outbound;
    const char *reason;

    knurl_proxy_http_response_init(&response, 0);
    PROXY_CHECK(accept(&response, "HTTP/1.1 200 OK\r\n") ==
                KNURL_PROXY_HTTP_MORE);
    accept(&response, "Content-Length: 5\r\n");
    accept(&response, "Connection: X-Hide\r\n");
    accept(&response, "X-Hide: secret\r\n");
    accept(&response, "Set-Cookie: a=1\r\n");
    accept(&response, "Set-Cookie: b=2\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_FINAL);
    PROXY_CHECK(response.status == 200 && response.has_content_length);
    PROXY_CHECK(response.content_length == 5u);
    PROXY_CHECK(response.headers.count == 5u);
    knurl_proxy_headers_init(&outbound);
    PROXY_CHECK(knurl_proxy_headers_sanitize_response(
                    &response.headers, &outbound) == KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(outbound.count == 2u);
    PROXY_CHECK(strcmp(outbound.fields[0].value, "a=1") == 0);
    PROXY_CHECK(strcmp(outbound.fields[1].value, "b=2") == 0);
    knurl_proxy_headers_cleanup(&outbound);
    reason = NULL;
    PROXY_CHECK(knurl_proxy_http_response_body(&response, 2u, &reason) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_http_response_finish(&response, &reason) ==
                KNURL_PROXY_HEADER_INVALID);
    PROXY_CHECK(knurl_proxy_http_response_body(&response, 3u, &reason) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_http_response_finish(&response, &reason) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_http_response_body(&response, 1u, &reason) ==
                KNURL_PROXY_HEADER_INVALID);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 0);
    accept(&response, "HTTP/1.1 200 OK\r\n");
    accept(&response, "Connection: Content-Length\r\n");
    accept(&response, "Content-Length: 0\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_FINAL);
    knurl_proxy_headers_init(&outbound);
    PROXY_CHECK(
        knurl_proxy_headers_sanitize_response(&response.headers, &outbound) ==
        KNURL_PROXY_HEADER_INVALID);
    knurl_proxy_headers_cleanup(&outbound);
    knurl_proxy_http_response_cleanup(&response);
}

static void test_http10_upstream(void)
{
    knurl_proxy_http_response response;

    knurl_proxy_http_response_init(&response, 0);
    PROXY_CHECK(accept(&response, "HTTP/1.0 201 Created\r\n") ==
                KNURL_PROXY_HTTP_MORE);
    accept(&response, "Content-Length: 2\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_FINAL);
    PROXY_CHECK(response.status == 201 && !response.http2);
    PROXY_CHECK(knurl_proxy_http_response_body(&response, 2u, NULL) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_http_response_finish(&response, NULL) ==
                KNURL_PROXY_HEADER_OK);
    knurl_proxy_http_response_cleanup(&response);
}

static void test_http2_sse_and_trailers(void)
{
    knurl_proxy_http_response response;
    const char *reason;
    size_t i;

    knurl_proxy_http_response_init(&response, 0);
    accept(&response, "HTTP/2 103 Early Hints\r\n");
    accept(&response, "Link: </a.css>; rel=preload\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_INFORMATIONAL);
    PROXY_CHECK(response.informational_count == 1u &&
                response.headers.count == 1u);
    accept(&response, "HTTP/2 200\r\n");
    PROXY_CHECK(response.headers.count == 0u);
    accept(&response, "Content-Type: text/event-stream\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_FINAL);
    PROXY_CHECK(response.http2 && response.body_allowed);
    for (i = 0u; i < 1024u; ++i) {
        PROXY_CHECK(knurl_proxy_http_response_body(&response, 1024u, NULL) ==
                    KNURL_PROXY_HEADER_OK);
    }
    PROXY_CHECK(response.body_bytes == 1048576u &&
                response.headers.count == 1u);
    accept(&response, "X-Trace: done\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_TRAILERS);
    PROXY_CHECK(response.trailers.count == 1u);
    reason = NULL;
    PROXY_CHECK(knurl_proxy_http_response_finish(&response, &reason) ==
                KNURL_PROXY_HEADER_OK);
    knurl_proxy_http_response_cleanup(&response);
}

static void test_chunked_and_head(void)
{
    knurl_proxy_http_response response;
    knurl_proxy_http_event event;

    knurl_proxy_http_response_init(&response, 0);
    accept(&response, "HTTP/1.1 200 OK\r\n");
    accept(&response, "Transfer-Encoding: chunked\r\n");
    accept(&response, "Trailer: X-Trace\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_FINAL);
    PROXY_CHECK(response.chunked);
    PROXY_CHECK(knurl_proxy_http_response_body(&response, 8192u, NULL) ==
                KNURL_PROXY_HEADER_OK);
    accept(&response, "X-Trace: done\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_TRAILERS);
    PROXY_CHECK(knurl_proxy_http_response_finish(&response, NULL) ==
                KNURL_PROXY_HEADER_OK);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 0);
    accept(&response, "HTTP/1.1 200 OK\r\n");
    accept(&response, "Transfer-Encoding: chunked\r\n");
    accept(&response, "\r\n");
    accept(&response, "X-Trace: done\r\n");
    PROXY_CHECK(knurl_proxy_http_response_finish(&response, NULL) ==
                KNURL_PROXY_HEADER_INVALID);
    PROXY_CHECK(knurl_proxy_http_response_curl_complete(
                    &response, &event, NULL) == KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(event == KNURL_PROXY_HTTP_TRAILERS);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 1);
    accept(&response, "HTTP/1.1 200 OK\r\n");
    accept(&response, "Content-Length: 12345\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_FINAL);
    PROXY_CHECK(!response.body_allowed && response.has_content_length);
    PROXY_CHECK(knurl_proxy_http_response_finish(&response, NULL) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_http_response_body(&response, 1u, NULL) ==
                KNURL_PROXY_HEADER_INVALID);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 0);
    accept(&response, "HTTP/1.1 205 Reset Content\r\n");
    accept(&response, "Transfer-Encoding: chunked\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_FINAL);
    PROXY_CHECK(!response.body_allowed && response.chunked);
    PROXY_CHECK(knurl_proxy_http_response_finish(&response, NULL) ==
                KNURL_PROXY_HEADER_OK);
    knurl_proxy_http_response_cleanup(&response);
}

static void test_rejections(void)
{
    knurl_proxy_http_response response;
    knurl_proxy_http_event event;
    const char *reason;
    static const char embedded_nul[] = "X-Test: a\0b\r\n";

    knurl_proxy_http_response_init(&response, 0);
    reject(&response, "HTTP/1.1 \r\n", KNURL_PROXY_HEADER_INVALID);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 0);
    reject(&response, "HTTP/1.1 101 Switching Protocols\r\n",
           KNURL_PROXY_HEADER_UNSUPPORTED_UPGRADE);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 0);
    accept(&response, "HTTP/1.1 204 No Content\r\n");
    accept(&response, "Content-Length: 1\r\n");
    reject(&response, "\r\n", KNURL_PROXY_HEADER_INVALID);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 0);
    accept(&response, "HTTP/1.1 205 Reset Content\r\n");
    accept(&response, "Content-Length: 1\r\n");
    reject(&response, "\r\n", KNURL_PROXY_HEADER_INVALID);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 0);
    accept(&response, "HTTP/1.1 205 Reset Content\r\n");
    accept(&response, "Content-Length: 0\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_FINAL);
    PROXY_CHECK(!response.body_allowed);
    PROXY_CHECK(knurl_proxy_http_response_finish(&response, NULL) ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_http_response_body(&response, 1u, NULL) ==
                KNURL_PROXY_HEADER_INVALID);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 0);
    accept(&response, "HTTP/1.1 200 OK\r\n");
    accept(&response, "Content-Length: 3\r\n");
    accept(&response, "Content-Length: 4\r\n");
    reject(&response, "\r\n", KNURL_PROXY_HEADER_INVALID);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 0);
    accept(&response, "HTTP/1.1 200 OK\r\n");
    reason = NULL;
    PROXY_CHECK(knurl_proxy_http_response_header(
                    &response, embedded_nul, sizeof(embedded_nul) - 1u, &event,
                    &reason) == KNURL_PROXY_HEADER_INVALID);
    PROXY_CHECK(reason != NULL);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 0);
    accept(&response, "HTTP/1.1 200 OK\r\n");
    PROXY_CHECK(accept(&response, "\r\n") == KNURL_PROXY_HTTP_FINAL);
    reject(&response, "X-Trace: done\r\n", KNURL_PROXY_HEADER_INVALID);
    knurl_proxy_http_response_cleanup(&response);
}

int main(void)
{
    test_fixed_length();
    test_http10_upstream();
    test_http2_sse_and_trailers();
    test_chunked_and_head();
    test_rejections();
    return 0;
}
