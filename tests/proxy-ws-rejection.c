#include "proxy-check.h"
#include <stdlib.h>
#include <string.h>

#include "proxy_ws_rejection.h"

static void fixed_body(void)
{
    static const char head[] =
        "HTTP/1.1 403 Forbidden\r\nContent-Length: 1048576\r\n"
        "Connection: close\r\nX-Reason: policy\r\n\r\n";
    rotmos_proxy_ws_rejection rejection;
    rotmos_error error;
    char input[4096];
    char output[8192];
    char *wire;
    size_t wire_length;
    size_t consumed;
    size_t written;
    size_t i;
    int final;

    rotmos_proxy_ws_rejection_init(&rejection);
    PROXY_CHECK(rotmos_proxy_ws_rejection_head(
                    &rejection, (const unsigned char *)head, sizeof(head) - 1u,
                    &final, &wire, &wire_length, NULL, NULL,
                    &error) == ROTMOS_OK);
    PROXY_CHECK(final);
    PROXY_CHECK(strstr(wire, "HTTP/1.1 403 ") == wire);
    PROXY_CHECK(strstr(wire, "Content-Length: 1048576\r\n") != NULL);
    PROXY_CHECK(strstr(wire, "X-Reason: policy\r\n") != NULL);
    PROXY_CHECK(strstr(wire, "Connection: close\r\n") != NULL);
    free(wire);
    memset(input, 'A', sizeof(input));
    for (i = 0u; i < 256u; ++i) {
        PROXY_CHECK(rotmos_proxy_ws_rejection_feed(
                        &rejection, (const unsigned char *)input, sizeof(input),
                        &consumed, output, sizeof(output), &written,
                        &error) == ROTMOS_OK);
        PROXY_CHECK(consumed == sizeof(input));
        PROXY_CHECK(written == sizeof(input));
        PROXY_CHECK(memcmp(output, input, sizeof(input)) == 0);
    }
    PROXY_CHECK(rejection.mode == ROTMOS_PROXY_WS_REJECTION_COMPLETE);
    PROXY_CHECK(rotmos_proxy_ws_rejection_finish(
                    &rejection, 0, &wire, &wire_length, &error) == ROTMOS_OK);
    PROXY_CHECK(wire == NULL && wire_length == 0u);
    rotmos_proxy_ws_rejection_cleanup(&rejection);
}

static void chunked_trailer(void)
{
    static const char head[] =
        "HTTP/1.1 429 Too Many Requests\r\nTransfer-Encoding: chunked\r\n"
        "Trailer: Digest\r\nConnection: close\r\n\r\n";
    static const char body[] = "5\r\nhello\r\n0\r\nDigest: sha-256=abc\r\n\r\n";
    rotmos_proxy_ws_rejection rejection;
    rotmos_error error;
    char output[64];
    char *wire;
    size_t wire_length;
    size_t consumed;
    size_t written;
    size_t pos;
    int final;
    int saw_body;

    rotmos_proxy_ws_rejection_init(&rejection);
    PROXY_CHECK(rotmos_proxy_ws_rejection_head(
                    &rejection, (const unsigned char *)head, sizeof(head) - 1u,
                    &final, &wire, &wire_length, NULL, NULL,
                    &error) == ROTMOS_OK);
    PROXY_CHECK(final);
    PROXY_CHECK(strstr(wire, "Transfer-Encoding: chunked\r\n") != NULL);
    PROXY_CHECK(strstr(wire, "Trailer: Digest\r\n") != NULL);
    free(wire);
    pos = 0u;
    saw_body = 0;
    while (pos < sizeof(body) - 1u) {
        PROXY_CHECK(rotmos_proxy_ws_rejection_feed(
                        &rejection, (const unsigned char *)body + pos,
                        sizeof(body) - 1u - pos, &consumed, output,
                        sizeof(output), &written, &error) == ROTMOS_OK);
        PROXY_CHECK(consumed != 0u);
        pos += consumed;
        if (written != 0u) {
            PROXY_CHECK(written == 10u);
            PROXY_CHECK(memcmp(output, "5\r\nhello\r\n", 10u) == 0);
            saw_body++;
        }
    }
    PROXY_CHECK(saw_body == 1);
    PROXY_CHECK(rejection.mode == ROTMOS_PROXY_WS_REJECTION_COMPLETE);
    PROXY_CHECK(rotmos_proxy_ws_rejection_finish(
                    &rejection, 0, &wire, &wire_length, &error) == ROTMOS_OK);
    PROXY_CHECK(wire_length == strlen("0\r\nDigest: sha-256=abc\r\n\r\n"));
    PROXY_CHECK(memcmp(wire, "0\r\nDigest: sha-256=abc\r\n\r\n", wire_length) ==
                0);
    free(wire);
    rotmos_proxy_ws_rejection_cleanup(&rejection);
}

static void informational_and_close_body(void)
{
    static const char interim[] =
        "HTTP/1.1 103 Early Hints\r\nLink: </next>; rel=preload\r\n\r\n";
    static const char final_head[] =
        "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n";
    rotmos_proxy_ws_rejection rejection;
    rotmos_error error;
    char output[64];
    char *wire;
    size_t wire_length;
    size_t consumed;
    size_t written;
    int final;

    rotmos_proxy_ws_rejection_init(&rejection);
    PROXY_CHECK(rotmos_proxy_ws_rejection_head(
                    &rejection, (const unsigned char *)interim,
                    sizeof(interim) - 1u, &final, &wire, &wire_length, NULL,
                    NULL, &error) == ROTMOS_OK);
    PROXY_CHECK(!final);
    PROXY_CHECK(strstr(wire, "HTTP/1.1 103 ") == wire);
    PROXY_CHECK(strstr(wire, "Link: </next>; rel=preload\r\n") != NULL);
    free(wire);
    PROXY_CHECK(rotmos_proxy_ws_rejection_head(
                    &rejection, (const unsigned char *)final_head,
                    sizeof(final_head) - 1u, &final, &wire, &wire_length, NULL,
                    NULL, &error) == ROTMOS_OK);
    PROXY_CHECK(final);
    PROXY_CHECK(strstr(wire, "HTTP/1.1 503 ") == wire);
    PROXY_CHECK(strstr(wire, "Transfer-Encoding: chunked\r\n") != NULL);
    free(wire);
    PROXY_CHECK(rotmos_proxy_ws_rejection_feed(
                    &rejection, (const unsigned char *)"slow", 4u, &consumed,
                    output, sizeof(output), &written, &error) == ROTMOS_OK);
    PROXY_CHECK(consumed == 4u && written == 9u);
    PROXY_CHECK(memcmp(output, "4\r\nslow\r\n", 9u) == 0);
    PROXY_CHECK(rotmos_proxy_ws_rejection_finish(
                    &rejection, 1, &wire, &wire_length, &error) == ROTMOS_OK);
    PROXY_CHECK(wire_length == 5u);
    PROXY_CHECK(memcmp(wire, "0\r\n\r\n", 5u) == 0);
    free(wire);
    rotmos_proxy_ws_rejection_cleanup(&rejection);
}

static void truncated_fixed_body(void)
{
    static const char head[] =
        "HTTP/1.1 403 Forbidden\r\nContent-Length: 5\r\n\r\n";
    rotmos_proxy_ws_rejection rejection;
    rotmos_error error;
    char output[64];
    char *wire;
    size_t wire_length;
    size_t consumed;
    size_t written;
    int final;

    rotmos_proxy_ws_rejection_init(&rejection);
    PROXY_CHECK(rotmos_proxy_ws_rejection_head(
                    &rejection, (const unsigned char *)head, sizeof(head) - 1u,
                    &final, &wire, &wire_length, NULL, NULL,
                    &error) == ROTMOS_OK);
    free(wire);
    PROXY_CHECK(rotmos_proxy_ws_rejection_feed(
                    &rejection, (const unsigned char *)"abc", 3u, &consumed,
                    output, sizeof(output), &written, &error) == ROTMOS_OK);
    PROXY_CHECK(rotmos_proxy_ws_rejection_finish(
                    &rejection, 1, &wire, &wire_length, &error) != ROTMOS_OK);
    rotmos_proxy_ws_rejection_cleanup(&rejection);
}

static void undeclared_trailer(void)
{
    static const char head[] =
        "HTTP/1.1 418 Teapot\r\nTransfer-Encoding: chunked\r\n\r\n";
    static const char body[] = "1\r\nx\r\n0\r\nDigest: abc\r\n\r\n";
    rotmos_proxy_ws_rejection rejection;
    rotmos_error error;
    char output[64];
    char *wire;
    size_t wire_length;
    size_t consumed;
    size_t written;
    size_t pos;
    int final;

    rotmos_proxy_ws_rejection_init(&rejection);
    PROXY_CHECK(rotmos_proxy_ws_rejection_head(
                    &rejection, (const unsigned char *)head, sizeof(head) - 1u,
                    &final, &wire, &wire_length, NULL, NULL,
                    &error) == ROTMOS_OK);
    PROXY_CHECK(final);
    free(wire);
    pos = 0u;
    while (pos < sizeof(body) - 1u) {
        PROXY_CHECK(rotmos_proxy_ws_rejection_feed(
                        &rejection, (const unsigned char *)body + pos,
                        sizeof(body) - 1u - pos, &consumed, output,
                        sizeof(output), &written, &error) == ROTMOS_OK);
        PROXY_CHECK(consumed != 0u);
        pos += consumed;
    }
    PROXY_CHECK(rotmos_proxy_ws_rejection_finish(
                    &rejection, 0, &wire, &wire_length, &error) == ROTMOS_OK);
    PROXY_CHECK(wire_length == strlen("0\r\nDigest: abc\r\n\r\n"));
    PROXY_CHECK(memcmp(wire, "0\r\nDigest: abc\r\n\r\n", wire_length) == 0);
    free(wire);
    rotmos_proxy_ws_rejection_cleanup(&rejection);
}

int main(void)
{
    fixed_body();
    chunked_trailer();
    informational_and_close_body();
    truncated_fixed_body();
    undeclared_trailer();
    return 0;
}
