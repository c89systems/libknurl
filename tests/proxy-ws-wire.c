#include "proxy_ws_wire.h"

#include "proxy-check.h"
#include <stdlib.h>
#include <string.h>

static void test_request(void)
{
    knurl_proxy_headers fields;
    knurl_error error;
    char *oversized_target;
    char *wire;
    size_t length;

    knurl_proxy_headers_init(&fields);
    PROXY_CHECK(knurl_proxy_headers_add(&fields, "Sec-WebSocket-Key",
                                        "dGhlIHNhbXBsZSBub25jZQ==") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_headers_add(&fields, "Sec-WebSocket-Version",
                                        "13") == KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_ws_wire_request("/chat?q=1&q=2",
                                            "example.test:8080", &fields, &wire,
                                            &length, &error) == KNURL_OK);
    PROXY_CHECK(length == strlen(wire));
    PROXY_CHECK(strstr(wire, "GET /chat?q=1&q=2 HTTP/1.1\r\n") == wire);
    PROXY_CHECK(strstr(wire, "Host: example.test:8080\r\n") != NULL);
    PROXY_CHECK(strstr(wire, "Connection: Upgrade\r\nUpgrade: websocket\r\n") !=
                NULL);
    PROXY_CHECK(
        strstr(wire, "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") !=
        NULL);
    PROXY_CHECK(length >= 4u &&
                memcmp(wire + length - 4u, "\r\n\r\n", 4u) == 0);
    free(wire);
    PROXY_CHECK(knurl_proxy_headers_add(&fields, "Connection", "close") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_ws_wire_request("/chat", "example.test", &fields,
                                            &wire, &length,
                                            &error) == KNURL_INVALID);
    PROXY_CHECK(wire == NULL && length == 0u);
    PROXY_CHECK(knurl_proxy_ws_wire_request("/chat\r\nInjected: yes",
                                            "example.test", &fields, &wire,
                                            &length, &error) == KNURL_INVALID);
    knurl_proxy_headers_cleanup(&fields);

    oversized_target = (char *)malloc(131074u);
    PROXY_CHECK(oversized_target != NULL);
    memset(oversized_target, 'a', 131073u);
    oversized_target[0] = '/';
    oversized_target[131073u] = '\0';
    knurl_proxy_headers_init(&fields);
    PROXY_CHECK(knurl_proxy_ws_wire_request(oversized_target, "example.test",
                                            &fields, &wire, &length,
                                            &error) == KNURL_INVALID);
    PROXY_CHECK(wire == NULL && length == 0u);
    knurl_proxy_headers_cleanup(&fields);
    free(oversized_target);
}

static void test_response(void)
{
    static const unsigned char accepted[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"
        "\201\004pong";
    static const unsigned char bad_status[] =
        "HTTP/2 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n";
    static const unsigned char http10_rejection[] =
        "HTTP/1.0 401 Unauthorized\r\nWWW-Authenticate: Basic "
        "realm=\"site\"\r\n"
        "Content-Length: 2\r\n\r\nno";
    static const unsigned char http10_upgrade[] =
        "HTTP/1.0 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n";
    static const unsigned char bad_control[] =
        "HTTP/1.1 101 Switching Protocols\r\nX-A: yes\000no\r\n\r\n";
    knurl_proxy_headers fields;
    knurl_proxy_ws_head_result result;
    const char *reason;
    size_t head_length;
    size_t expected_head;
    size_t i;
    unsigned status;

    expected_head = (size_t)(strstr((const char *)accepted, "\r\n\r\n") -
                             (const char *)accepted) +
                    4u;
    for (i = 0u; i < expected_head; ++i) {
        knurl_proxy_headers_init(&fields);
        result = knurl_proxy_ws_wire_response_head(accepted, i, &head_length,
                                                   &status, &fields, &reason);
        PROXY_CHECK(result == KNURL_PROXY_WS_HEAD_MORE);
        PROXY_CHECK(fields.count == 0u);
    }
    knurl_proxy_headers_init(&fields);
    result = knurl_proxy_ws_wire_response_head(accepted, sizeof(accepted) - 1u,
                                               &head_length, &status, &fields,
                                               &reason);
    PROXY_CHECK(result == KNURL_PROXY_WS_HEAD_COMPLETE);
    PROXY_CHECK(status == 101u && fields.count == 3u);
    PROXY_CHECK(head_length < sizeof(accepted) - 1u);
    PROXY_CHECK(memcmp(accepted + head_length, "\201\004pong", 6u) == 0);
    knurl_proxy_headers_cleanup(&fields);

    knurl_proxy_headers_init(&fields);
    result = knurl_proxy_ws_wire_response_head(
        bad_status, sizeof(bad_status) - 1u, &head_length, &status, &fields,
        &reason);
    PROXY_CHECK(result == KNURL_PROXY_WS_HEAD_INVALID && reason != NULL);
    PROXY_CHECK(fields.count == 0u);

    knurl_proxy_headers_init(&fields);
    result = knurl_proxy_ws_wire_response_head(
        http10_rejection, sizeof(http10_rejection) - 1u, &head_length, &status,
        &fields, &reason);
    PROXY_CHECK(result == KNURL_PROXY_WS_HEAD_COMPLETE);
    PROXY_CHECK(status == 401u && fields.count == 2u);
    PROXY_CHECK(head_length < sizeof(http10_rejection) - 1u);
    knurl_proxy_headers_cleanup(&fields);

    knurl_proxy_headers_init(&fields);
    result = knurl_proxy_ws_wire_response_head(
        http10_upgrade, sizeof(http10_upgrade) - 1u, &head_length, &status,
        &fields, &reason);
    PROXY_CHECK(result == KNURL_PROXY_WS_HEAD_INVALID && reason != NULL);
    PROXY_CHECK(fields.count == 0u);

    knurl_proxy_headers_init(&fields);
    result = knurl_proxy_ws_wire_response_head(
        bad_control, sizeof(bad_control) - 1u, &head_length, &status, &fields,
        &reason);
    PROXY_CHECK(result == KNURL_PROXY_WS_HEAD_INVALID && reason != NULL);
    PROXY_CHECK(fields.count == 0u);
}

static void test_head_limits(void)
{
    unsigned char *oversized;
    knurl_proxy_headers fields;
    knurl_proxy_ws_head_result result;
    const char *reason;
    size_t head_length;
    unsigned status;

    oversized = (unsigned char *)malloc(KNURL_PROXY_HEADER_BLOCK_LIMIT);
    PROXY_CHECK(oversized != NULL);
    memset(oversized, 'X', KNURL_PROXY_HEADER_BLOCK_LIMIT);
    knurl_proxy_headers_init(&fields);
    result = knurl_proxy_ws_wire_response_head(
        oversized, KNURL_PROXY_HEADER_BLOCK_LIMIT, &head_length, &status,
        &fields, &reason);
    PROXY_CHECK(result == KNURL_PROXY_WS_HEAD_LIMIT && reason != NULL);
    PROXY_CHECK(fields.count == 0u && head_length == 0u);
    free(oversized);
}

static void test_upgrade_response(void)
{
    knurl_proxy_headers upstream;
    knurl_error error;
    char *wire;
    size_t length;

    knurl_proxy_headers_init(&upstream);
    PROXY_CHECK(
        knurl_proxy_headers_add(&upstream, "Connection", "Upgrade, X-Hop") ==
        KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_headers_add(&upstream, "Upgrade", "websocket") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_headers_add(&upstream, "Sec-WebSocket-Accept",
                                        "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_headers_add(&upstream, "Sec-WebSocket-Protocol",
                                        "chat") == KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_headers_add(&upstream, "X-Hop", "discard") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_headers_add(&upstream, "Set-Cookie", "session=1") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_ws_wire_upgrade_response(&upstream, &wire, &length,
                                                     &error) == KNURL_OK);
    PROXY_CHECK(length == strlen(wire));
    PROXY_CHECK(strstr(wire, "HTTP/1.1 101 Switching Protocols\r\n") == wire);
    PROXY_CHECK(strstr(wire, "Connection: Upgrade\r\nUpgrade: websocket\r\n") !=
                NULL);
    PROXY_CHECK(strstr(wire, "Sec-WebSocket-Protocol: chat\r\n") != NULL);
    PROXY_CHECK(strstr(wire, "Set-Cookie: session=1\r\n") != NULL);
    PROXY_CHECK(strstr(wire, "X-Hop:") == NULL);
    free(wire);
    knurl_proxy_headers_cleanup(&upstream);
}

int main(void)
{
    test_request();
    test_response();
    test_head_limits();
    test_upgrade_response();
    return 0;
}
