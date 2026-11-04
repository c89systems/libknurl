#define _POSIX_C_SOURCE 200809L
#include "events.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "Lua events %d: %s\n", __LINE__, #t);              \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct peer {
    int fd;
    SSL *ssl;
};
static int write_all(struct peer *peer, const void *data, size_t length)
{
    const unsigned char *cursor = data;
    int count;
    while (length) {
        count = peer->ssl ? SSL_write(peer->ssl, cursor, (int)length)
                          : (int)write(peer->fd, cursor, length);
        CHECK(count > 0);
        cursor += count;
        length -= (size_t)count;
    }
    return 0;
}
static int read_all(struct peer *peer, void *data, size_t length)
{
    unsigned char *cursor = data;
    int count;
    while (length) {
        count = peer->ssl ? SSL_read(peer->ssl, cursor, (int)length)
                          : (int)read(peer->fd, cursor, length);
        CHECK(count > 0);
        cursor += count;
        length -= (size_t)count;
    }
    return 0;
}
static int connect_ws(struct peer *peer, unsigned short port, SSL_CTX *context,
                      const char *path)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char head[512], response[2048];
    size_t used = 0;
    memset(peer, 0, sizeof(*peer));
    peer->fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(peer->fd >= 0);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(peer->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(peer->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    CHECK(connect(peer->fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    if (context != NULL) {
        peer->ssl = SSL_new(context);
        CHECK(peer->ssl && SSL_set_fd(peer->ssl, peer->fd) &&
              SSL_set_tlsext_host_name(peer->ssl, "localhost") &&
              SSL_set1_host(peer->ssl, "localhost") &&
              SSL_connect(peer->ssl) == 1);
    }
    snprintf(head, sizeof(head),
             "GET %s HTTP/1.1\r\nHost: localhost\r\nUpgrade: "
             "websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: "
             "13\r\nSec-WebSocket-Key: "
             "dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: test\r\n\r\n",
             path);
    CHECK(write_all(peer, head, strlen(head)) == 0);
    do {
        CHECK(used + 1 < sizeof(response) &&
              read_all(peer, response + used, 1) == 0);
        response[++used] = 0;
    } while (strstr(response, "\r\n\r\n") == NULL);
    CHECK(strstr(response, "101 ") &&
          strstr(response, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    if (!strcmp(path, "/ws"))
        CHECK(strstr(response, "test") && strstr(response, "copied"));
    return 0;
}
static void disconnect(struct peer *peer)
{
    SSL_free(peer->ssl);
    close(peer->fd);
}
static int send_frame(struct peer *peer, unsigned int opcode, const void *data,
                      size_t length)
{
    unsigned char buffer[512];
    const unsigned char *bytes = data;
    size_t offset = 2, i;
    CHECK(length <= 256);
    buffer[0] = (unsigned char)(0x80U | opcode);
    if (length <= 125)
        buffer[1] = (unsigned char)(0x80U | (unsigned int)length);
    else {
        buffer[1] = 0xfe;
        buffer[2] = (unsigned char)(length >> 8);
        buffer[3] = (unsigned char)length;
        offset = 4;
    }
    memset(buffer + offset, 0x37, 4);
    offset += 4;
    for (i = 0; i < length; ++i)
        buffer[offset + i] = bytes[i] ^ 0x37U;
    return write_all(peer, buffer, offset + length);
}
static int expect_frame(struct peer *peer, unsigned int opcode,
                        const void *data, size_t length)
{
    unsigned char head[2], bytes[512];
    size_t received;
    CHECK(read_all(peer, head, 2) == 0 &&
          head[0] == (unsigned char)(0x80U | opcode) && !(head[1] & 0x80U));
    received = head[1];
    if (received == 126) {
        CHECK(read_all(peer, head, 2) == 0);
        received = (size_t)head[0] * 256 + head[1];
    }
    CHECK(received <= sizeof(bytes) && received == length &&
          read_all(peer, bytes, received) == 0 && !memcmp(bytes, data, length));
    return 0;
}
static int finish(struct peer *peer, unsigned short code)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(code >> 8);
    bytes[1] = (unsigned char)code;
    CHECK(send_frame(peer, 8, bytes, 2) == 0 &&
          expect_frame(peer, 8, "", 0) == 0);
    disconnect(peer);
    return 0;
}
static int echo(unsigned short port, SSL_CTX *context)
{
    struct peer peer;
    char large[512];
    memset(large, 'w', sizeof(large));
    CHECK(connect_ws(&peer, port, context, "/ws") == 0 &&
          expect_frame(&peer, 1, "ready", 5) == 0);
    CHECK(expect_frame(&peer, 0, "", 0) == 0 &&
          expect_frame(&peer, 1, "\xc0\xaf", 2) == 0 &&
          expect_frame(&peer, 2, large, sizeof(large)) == 0);
    CHECK(expect_frame(&peer, 8, "", 0) == 0 &&
          expect_frame(&peer, 1, "after-close", 11) == 0);
    CHECK(send_frame(&peer, 1, "pause", 5) == 0 &&
          expect_frame(&peer, 1, "pause", 5) == 0);
    CHECK(send_frame(&peer, 2, "A\0B", 3) == 0 &&
          expect_frame(&peer, 2, "A\0B", 3) == 0);
    CHECK(send_frame(&peer, 9, "p", 1) == 0 &&
          expect_frame(&peer, 10, "p", 1) == 0);
    return finish(&peer, 1000);
}
int rotmos_lua_test_events(unsigned short http, unsigned short tls,
                           SSL_CTX *context, int notify)
{
    struct peer peer;
    unsigned char close_bytes[2] = {3, 243};
    char fill[100];
    const char *bad = "invalid WebSocket handshake\n";
    static const char events[] = "data: A\0B\ndata: C\n\n: c\nid: 1\nevent: "
                                 "ready\nretry: 0\ndata: done\n\n";
    int i;
    static const char *const failures[] = {"/ws-error", "/ws-oom"};
    CHECK(write(notify, "w", 1) == 1);
    CHECK(rotmos_lua_test_exchange(http, "GET", "/sse", "", 0, 200, events,
                                   sizeof(events) - 1) == 0);
    CHECK(rotmos_lua_test_exchange(http, "GET", "/group/sse", "", 0, 200,
                                   "data: g\n\n", 9) == 0);
    CHECK(rotmos_lua_test_exchange(http, "GET", "/sse-denied", "", 0, 403, "no",
                                   2) == 0);
    CHECK(rotmos_lua_test_exchange(http, "GET", "/ws", "", 0, 400, bad,
                                   strlen(bad)) == 0);
    CHECK(echo(http, NULL) == 0 && echo(tls, context) == 0);
    memset(fill, 'x', sizeof(fill));
    CHECK(connect_ws(&peer, http, NULL, "/pressure") == 0);
    CHECK(expect_frame(&peer, 2, fill, sizeof(fill)) == 0 &&
          expect_frame(&peer, 2, fill, sizeof(fill)) == 0 &&
          finish(&peer, 1000) == 0);
    for (i = 0; i < 2; ++i) {
        CHECK(connect_ws(&peer, http, NULL, failures[i]) == 0 &&
              send_frame(&peer, 1, "fail", 4) == 0);
        CHECK(expect_frame(&peer, 8, close_bytes, 2) == 0 &&
              send_frame(&peer, 8, close_bytes, 2) == 0);
        disconnect(&peer);
    }
    CHECK(connect_ws(&peer, http, NULL, "/ws-close-error") == 0 &&
          send_frame(&peer, 1, "echo", 4) == 0 &&
          expect_frame(&peer, 1, "echo", 4) == 0 && finish(&peer, 1000) == 0);
    return rotmos_lua_test_exchange(http, "GET", "/stop", "", 0, 200, "ok", 2);
}
