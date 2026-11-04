#include "proxy_ws_handshake.h"

#include "proxy-check.h"
#include <string.h>

static void add(rotmos_proxy_headers *headers, const char *name,
                const char *value)
{
    PROXY_CHECK(rotmos_proxy_headers_add(headers, name, value) ==
                ROTMOS_PROXY_HEADER_OK);
}

static void request(rotmos_proxy_headers *headers)
{
    rotmos_proxy_headers_init(headers);
    add(headers, "Host", "example.test");
    add(headers, "Connection", "keep-alive, Upgrade");
    add(headers, "Upgrade", "websocket");
    add(headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    add(headers, "Sec-WebSocket-Version", "13");
    add(headers, "Sec-WebSocket-Protocol", "chat, superchat");
    add(headers, "Sec-WebSocket-Extensions", "permessage-deflate");
}

static void response(rotmos_proxy_headers *headers)
{
    rotmos_proxy_headers_init(headers);
    add(headers, "Upgrade", "websocket");
    add(headers, "Connection", "Upgrade");
    add(headers, "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    add(headers, "Sec-WebSocket-Protocol", "chat");
    add(headers, "Sec-WebSocket-Extensions",
        "permessage-deflate; server_no_context_takeover");
}

static void test_valid(void)
{
    rotmos_proxy_headers inbound;
    rotmos_proxy_headers upstream;
    const char *reason;

    request(&inbound);
    response(&upstream);
    reason = "not reset";
    PROXY_CHECK(
        rotmos_proxy_ws_request_valid(ROTMOS_HTTP_GET, &inbound, &reason));
    PROXY_CHECK(reason == NULL);
    PROXY_CHECK(
        rotmos_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
    PROXY_CHECK(reason == NULL);
    rotmos_proxy_headers_cleanup(&upstream);
    rotmos_proxy_headers_cleanup(&inbound);
}

static void test_request_rejections(void)
{
    rotmos_proxy_headers inbound;
    const char *reason;

    request(&inbound);
    PROXY_CHECK(
        !rotmos_proxy_ws_request_valid(ROTMOS_HTTP_POST, &inbound, &reason));
    PROXY_CHECK(reason != NULL);
    add(&inbound, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    PROXY_CHECK(
        !rotmos_proxy_ws_request_valid(ROTMOS_HTTP_GET, &inbound, &reason));
    rotmos_proxy_headers_cleanup(&inbound);

    request(&inbound);
    add(&inbound, "Content-Length", "0");
    PROXY_CHECK(
        !rotmos_proxy_ws_request_valid(ROTMOS_HTTP_GET, &inbound, &reason));
    rotmos_proxy_headers_cleanup(&inbound);

    request(&inbound);
    add(&inbound, "Sec-WebSocket-Version", "12");
    PROXY_CHECK(
        !rotmos_proxy_ws_request_valid(ROTMOS_HTTP_GET, &inbound, &reason));
    rotmos_proxy_headers_cleanup(&inbound);

    request(&inbound);
    add(&inbound, "Sec-WebSocket-Protocol", "chat,,other");
    PROXY_CHECK(
        !rotmos_proxy_ws_request_valid(ROTMOS_HTTP_GET, &inbound, &reason));
    rotmos_proxy_headers_cleanup(&inbound);

    request(&inbound);
    add(&inbound, "Sec-WebSocket-Accept", "injected");
    PROXY_CHECK(
        !rotmos_proxy_ws_request_valid(ROTMOS_HTTP_GET, &inbound, &reason));
    rotmos_proxy_headers_cleanup(&inbound);
}

static void test_response_rejections(void)
{
    rotmos_proxy_headers inbound;
    rotmos_proxy_headers upstream;
    const char *reason;

    request(&inbound);
    response(&upstream);
    PROXY_CHECK(
        !rotmos_proxy_ws_response_valid(&inbound, 403u, &upstream, &reason));
    rotmos_proxy_headers_cleanup(&upstream);

    response(&upstream);
    add(&upstream, "Sec-WebSocket-Accept", "wrong");
    PROXY_CHECK(
        !rotmos_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
    rotmos_proxy_headers_cleanup(&upstream);

    response(&upstream);
    add(&upstream, "Content-Length", "0");
    PROXY_CHECK(
        !rotmos_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
    rotmos_proxy_headers_cleanup(&upstream);

    response(&upstream);
    add(&upstream, "Sec-WebSocket-Protocol", "unoffered");
    PROXY_CHECK(
        !rotmos_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
    rotmos_proxy_headers_cleanup(&upstream);

    rotmos_proxy_headers_init(&upstream);
    add(&upstream, "Upgrade", "websocket");
    add(&upstream, "Connection", "Upgrade");
    add(&upstream, "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    add(&upstream, "Sec-WebSocket-Protocol", "unoffered");
    PROXY_CHECK(
        !rotmos_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
    PROXY_CHECK(strstr(reason, "subprotocol") != NULL);
    rotmos_proxy_headers_cleanup(&upstream);

    rotmos_proxy_headers_init(&upstream);
    add(&upstream, "Upgrade", "websocket");
    add(&upstream, "Connection", "Upgrade");
    add(&upstream, "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    add(&upstream, "Sec-WebSocket-Extensions", "unoffered-extension");
    PROXY_CHECK(
        !rotmos_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
    PROXY_CHECK(strstr(reason, "extension") != NULL);
    rotmos_proxy_headers_cleanup(&upstream);

    rotmos_proxy_headers_init(&upstream);
    add(&upstream, "Upgrade", "websocket");
    add(&upstream, "Connection", "Upgrade");
    add(&upstream, "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    add(&upstream, "Sec-WebSocket-Extensions",
        "permessage-deflate; bad==value");
    PROXY_CHECK(
        !rotmos_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
    rotmos_proxy_headers_cleanup(&upstream);

    rotmos_proxy_headers_init(&upstream);
    add(&upstream, "Upgrade", "websocket");
    add(&upstream, "Connection", "Upgrade");
    add(&upstream, "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    add(&upstream, "Sec-WebSocket-Extensions",
        "permessage-deflate; note=\"a,b\"");
    PROXY_CHECK(
        rotmos_proxy_ws_response_valid(&inbound, 101u, &upstream, &reason));
    rotmos_proxy_headers_cleanup(&upstream);
    rotmos_proxy_headers_cleanup(&inbound);
}

int main(void)
{
    test_valid();
    test_request_rejections();
    test_response_rejections();
    return 0;
}
