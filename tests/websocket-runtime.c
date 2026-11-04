#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            (void)fprintf(stderr, "WS %d: %s\n", __LINE__, #t);                \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#define OBSERVE(t)                                                             \
    do {                                                                       \
        if (!(t)) {                                                            \
            (void)fprintf(stderr, "WS callback %d: %s\n", __LINE__, #t);       \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

struct fixture {
    pid_t client;
    int tls;
    char *bundle;
};
struct context {
    int request_closed;
    int opens;
    int holds;
    int frames;
    int writable;
    char path[32];
    const void *held;
    pthread_t owner;
    rotmos_websocket *ws;
    rotmos_timer *timer;
};
struct client {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
};
static int failures;
static int closes;
static int failed_accepts;
static unsigned char output[60];

static rotmos_bytes view(const char *s)
{
    rotmos_bytes b;
    b.data = s;
    b.length = strlen(s);
    return b;
}

static void request_close(rotmos_request *request,
                          const rotmos_request_outcome *out, void *data)
{
    struct context *c;
    c = data;
    OBSERVE(!c->request_closed && !c->opens &&
            pthread_equal(c->owner, pthread_self()));
    OBSERVE(rotmos_request_target(request).length != 0);
    c->request_closed = 1;
    if (strcmp(c->path, "/bad") == 0) {
        OBSERVE(!out->upgraded);
        ++failed_accepts;
        free(c);
    } else if (strcmp(c->path, "/wrong") == 0)
        OBSERVE(!out->upgraded);
    else
        OBSERVE(out->upgraded && out->body_complete &&
                out->response.status == 101);
}

static void resume(rotmos_timer *timer, void *data)
{
    struct context *c;
    (void)timer;
    c = data;
    c->timer = NULL;
    OBSERVE(rotmos_websocket_resume(c->ws, NULL) == ROTMOS_OK);
}

static rotmos_status frame(rotmos_websocket *ws, rotmos_websocket_opcode opcode,
                           rotmos_bytes bytes, void *data, rotmos_error *error)
{
    struct context *c;
    c = data;
    OBSERVE(c->request_closed && c->opens == 1 &&
            pthread_equal(c->owner, pthread_self()));
    if (strcmp(c->path, "/protocol") == 0)
        return rotmos_websocket_send(ws, opcode, bytes.data, bytes.length,
                                     error);
    if (bytes.length == 256) {
        OBSERVE(opcode == ROTMOS_WEBSOCKET_BINARY && c->frames++ == 2);
        return rotmos_websocket_send_text(ws, "wide", 4, error);
    }
    OBSERVE(bytes.length == 4);
    if (opcode == ROTMOS_WEBSOCKET_TEXT) {
        OBSERVE(memcmp(bytes.data, "hold", 4) == 0 && c->frames == 0);
        if (c->holds++ == 0) {
            c->held = bytes.data;
            OBSERVE(rotmos_loop_timer(rotmos_websocket_loop(ws), 30, 0, resume,
                                      NULL, c, &c->timer, error) == ROTMOS_OK);
            return ROTMOS_AGAIN;
        }
        OBSERVE(c->held == bytes.data && c->holds == 2);
    } else
        OBSERVE(opcode == ROTMOS_WEBSOCKET_BINARY && c->frames == 1 &&
                memcmp(bytes.data, "next", 4) == 0);
    ++c->frames;
    return rotmos_websocket_send(ws, opcode, bytes.data, bytes.length, error);
}

static void writable(rotmos_websocket *ws, void *data)
{
    struct context *c;
    c = data;
    OBSERVE(c->request_closed && c->opens == 1 && c->writable++ == 0);
    OBSERVE(rotmos_websocket_send_binary(ws, output, sizeof(output), NULL) ==
            ROTMOS_OK);
}

static rotmos_status open_ws(rotmos_websocket *ws, void *data,
                             rotmos_error *error)
{
    struct context *c;
    size_t queued;
    c = data;
    c->ws = ws;
    OBSERVE(c->request_closed && !c->opens++ &&
            pthread_equal(c->owner, pthread_self()));
    OBSERVE(rotmos_websocket_state(ws) == ROTMOS_WEBSOCKET_OPEN &&
            rotmos_websocket_server(ws) != NULL &&
            rotmos_websocket_loop(ws) != NULL);
    OBSERVE(rotmos_websocket_userdata(ws) == c);
    OBSERVE(rotmos_websocket_set_userdata(ws, c, error) == ROTMOS_OK);
    if (strcmp(c->path, "/echo") == 0) {
        OBSERVE(rotmos_websocket_send_text(ws, "x", 257, error) ==
                ROTMOS_LIMIT);
        OBSERVE(rotmos_websocket_send_binary(ws, output, 126, error) ==
                ROTMOS_LIMIT);
        OBSERVE(rotmos_websocket_send_binary(ws, output, sizeof(output),
                                             error) == ROTMOS_OK);
        OBSERVE(rotmos_websocket_send_binary(ws, output, sizeof(output),
                                             error) == ROTMOS_OK);
        queued = rotmos_websocket_queued_bytes(ws);
        OBSERVE(queued == 124);
        OBSERVE(rotmos_websocket_send_binary(ws, output, sizeof(output),
                                             error) == ROTMOS_AGAIN);
        {
            const unsigned char closing[2] = {3, 232};
            OBSERVE(rotmos_websocket_send(ws, ROTMOS_WEBSOCKET_CLOSE, closing,
                                          2, error) == ROTMOS_AGAIN);
            OBSERVE(rotmos_websocket_state(ws) == ROTMOS_WEBSOCKET_OPEN);
        }
        OBSERVE(rotmos_websocket_queued_bytes(ws) == queued);
    } else if (strcmp(c->path, "/timeout") == 0) {
        OBSERVE(rotmos_websocket_close(ws, 1005, view(""), error) ==
                ROTMOS_INVALID);
        OBSERVE(rotmos_websocket_close(ws, 1000, view("bye"), error) ==
                ROTMOS_OK);
        OBSERVE(rotmos_websocket_close(ws, 1000, view("again"), error) ==
                ROTMOS_OK);
    } else if (strcmp(c->path, "/open-error") == 0)
        return ROTMOS_INVALID;
    return ROTMOS_OK;
}

static void close_ws(rotmos_websocket *ws, const rotmos_websocket_outcome *out,
                     void *data)
{
    struct context *c;
    c = data;
    ++closes;
    OBSERVE(c->request_closed && out->opened == c->opens &&
            pthread_equal(c->owner, pthread_self()));
    OBSERVE(rotmos_websocket_state(ws) == ROTMOS_WEBSOCKET_CLOSED);
    OBSERVE(rotmos_websocket_resume(ws, NULL) == ROTMOS_STATE);
    OBSERVE(rotmos_websocket_send_text(ws, "x", 1, NULL) == ROTMOS_STATE);
    OBSERVE(rotmos_websocket_close(ws, 1000, view(""), NULL) == ROTMOS_OK);
    if (strcmp(c->path, "/echo") == 0 && out->cause != ROTMOS_OK)
        (void)fprintf(stderr,
                      "echo closed: cause=%s local=%u peer=%u frames=%d "
                      "holds=%d writable=%d\n",
                      rotmos_status_string(out->cause), out->local_code,
                      out->peer_code, c->frames, c->holds, c->writable);
    if (strcmp(c->path, "/echo") == 0)
        OBSERVE(out->cause == ROTMOS_OK && out->clean &&
                out->peer_code == 1000 && c->frames == 3 && c->holds == 2 &&
                c->writable == 1 && out->peer_reason.length == 2 &&
                memcmp(out->peer_reason.data, "ok", 2) == 0);
    else if (strcmp(c->path, "/wrong") == 0)
        OBSERVE(!out->opened && out->cause == ROTMOS_INVALID);
    else if (strcmp(c->path, "/timeout") == 0)
        OBSERVE(!out->clean && out->cause == ROTMOS_TIMEOUT);
    else if (strcmp(c->path, "/open-error") == 0)
        OBSERVE(out->cause == ROTMOS_INVALID && out->local_code == 1011);
    else if (strcmp(c->path, "/stop-ws") == 0)
        OBSERVE(out->cause == ROTMOS_CANCELLED && out->local_code == 1001);
    else if (strcmp(c->path, "/protocol") == 0)
        OBSERVE(c->frames == 0);
    else
        OBSERVE(!out->clean && out->cause != ROTMOS_OK && c->frames == 0);
    if (c->timer != NULL)
        rotmos_timer_cancel(c->timer);
    free(c);
}

static rotmos_status admission(rotmos_request *request,
                               rotmos_request_plan *plan, void *data,
                               rotmos_error *error)
{
    struct context *c;
    rotmos_bytes path;
    (void)data;
    (void)error;
    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return ROTMOS_NOMEM;
    path = rotmos_request_path(request);
    OBSERVE(path.length < sizeof(c->path));
    memcpy(c->path, path.data, path.length);
    c->owner = pthread_self();
    plan->on_close = request_close;
    plan->userdata = c;
    return ROTMOS_OK;
}

static rotmos_status route_admission(rotmos_request *request,
                                     rotmos_websocket_config *config,
                                     rotmos_request_plan *plan, void *data,
                                     rotmos_error *error)
{
    rotmos_status status;
    status = admission(request, plan, data, error);
    if (status == ROTMOS_OK)
        config->userdata = plan->userdata;
    return status;
}

static rotmos_status headers(rotmos_request *request, rotmos_request_plan *plan,
                             void *data, rotmos_error *error)
{
    rotmos_websocket_config config;
    rotmos_websocket *ws;
    struct context *c;
    rotmos_bytes path;
    rotmos_status status;
    path = rotmos_request_path(request);
    if (path.length == 5 && memcmp(path.data, "/stop", 5) == 0) {
        status = rotmos_response_bytes(rotmos_request_response(request), 200,
                                       "text/plain", view("stopped"), error);
        plan->disposition = ROTMOS_REQUEST_REPLY;
        if (status == ROTMOS_OK)
            (void)rotmos_server_request_stop(rotmos_request_server(request),
                                             error);
        return status;
    }
    if (path.length == 5 && memcmp(path.data, "/echo", 5) == 0)
        return rotmos_router_dispatch_headers(
            rotmos_server_router(rotmos_request_server(request)), request, plan,
            error);
    status = admission(request, plan, data, error);
    if (status != ROTMOS_OK)
        return status;
    c = plan->userdata;
    rotmos_websocket_config_init(&config);
    config.on_open = open_ws;
    config.on_frame = frame;
    config.on_writable = writable;
    config.on_close = close_ws;
    config.userdata = c;
    config.max_frame_bytes = 256;
    config.output_queue_bytes = 127;
    config.close_timeout_ms = 40;
    if (strcmp(c->path, "/bad") == 0) {
        config.protocol = "not-offered";
        OBSERVE(rotmos_websocket_accept(request, &config, &ws, error) ==
                    ROTMOS_INVALID &&
                ws == NULL);
        config.protocol = NULL;
        {
            rotmos_header extension;
            extension.name = view("Sec-WebSocket-Extensions");
            extension.value = view("permessage-deflate");
            config.headers = &extension;
            config.header_count = 1;
            OBSERVE(rotmos_websocket_accept(request, &config, &ws, error) ==
                        ROTMOS_NOT_SUPPORTED &&
                    ws == NULL);
            config.headers = NULL;
            config.header_count = 0;
        }
        config.output_queue_bytes = 126;
        OBSERVE(rotmos_websocket_accept(request, &config, &ws, error) ==
                    ROTMOS_LIMIT &&
                ws == NULL);
        OBSERVE(rotmos_response_bytes(rotmos_request_response(request), 403,
                                      "text/plain", view("rejected"),
                                      error) == ROTMOS_OK);
        plan->disposition = ROTMOS_REQUEST_REPLY;
        return ROTMOS_OK;
    }
    status = rotmos_websocket_accept(request, &config, &ws, error);
    if (status == ROTMOS_OK) {
        OBSERVE(rotmos_response_replace_uncommitted(
                    rotmos_request_response(request), error) == ROTMOS_STATE);
        rotmos_error_clear(error);
    }
    plan->disposition = strcmp(c->path, "/wrong") == 0
                            ? ROTMOS_REQUEST_REPLY
                            : ROTMOS_REQUEST_WEBSOCKET;
    return status;
}

static int write_all(struct client *c, const void *data, size_t length)
{
    const unsigned char *p;
    int count;
    p = data;
    while (length != 0) {
        count = c->ssl != NULL ? SSL_write(c->ssl, p, (int)length)
                               : (int)write(c->fd, p, length);
        CHECK(count > 0);
        p += count;
        length -= (size_t)count;
    }
    return 0;
}

static int read_exact(struct client *c, void *data, size_t length)
{
    unsigned char *p;
    int count;
    p = data;
    while (length != 0) {
        count = c->ssl != NULL ? SSL_read(c->ssl, p, (int)length)
                               : (int)read(c->fd, p, length);
        CHECK(count > 0);
        p += count;
        length -= (size_t)count;
    }
    return 0;
}

static void disconnect(struct client *c)
{
    SSL_free(c->ssl);
    SSL_CTX_free(c->ctx);
    (void)close(c->fd);
}

static int connect_to(struct client *c, unsigned short port, int tls,
                      const char *path, int upgrade)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char request[512];
    char response[2048];
    size_t used;
    memset(c, 0, sizeof(*c));
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(c->fd >= 0);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    CHECK(setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) == 0);
    CHECK(setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout)) == 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(c->fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    if (tls) {
        c->ctx = SSL_CTX_new(TLS_client_method());
        CHECK(c->ctx != NULL);
        c->ssl = SSL_new(c->ctx);
        CHECK(c->ssl != NULL && SSL_set_fd(c->ssl, c->fd) &&
              SSL_set_tlsext_host_name(c->ssl, "localhost") &&
              SSL_connect(c->ssl) == 1);
    }
    (void)snprintf(
        request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: localhost\r\n%s\r\n", path,
        upgrade ? "Upgrade: websocket\r\nConnection: "
                  "Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "
                  "dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: test\r\n"
                : "Connection: close\r\n");
    if (strcmp(path, "/echo") == 0) {
        const unsigned char early[7] = {137, 129, 0, 0, 0, 0, 'e'};
        used = strlen(request);
        memcpy(request + used, early, sizeof(early));
        CHECK(write_all(c, request, used + sizeof(early)) == 0);
    } else
        CHECK(write_all(c, request, strlen(request)) == 0);
    if (strcmp(path, "/wrong") == 0)
        return 0;
    used = 0;
    do {
        CHECK(used + 1 < sizeof(response) &&
              read_exact(c, response + used, 1) == 0);
        response[++used] = '\0';
    } while (strstr(response, "\r\n\r\n") == NULL);
    if (strcmp(path, "/bad") == 0)
        CHECK(strstr(response, "403 ") != NULL);
    else if (strcmp(path, "/stop") != 0 && strcmp(path, "/wrong") != 0) {
        CHECK(strstr(response, "101 ") != NULL &&
              strstr(response, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
        if (strcmp(path, "/echo") == 0)
            CHECK(strstr(response, "test") != NULL &&
                  strstr(response, "copied") != NULL);
    }
    return 0;
}

static int send_frame(struct client *c, unsigned int first, const void *data,
                      size_t length, int masked)
{
    unsigned char wire[300];
    const unsigned char *p;
    size_t at;
    size_t i;
    CHECK(length <= 257);
    wire[0] = (unsigned char)first;
    wire[1] =
        (unsigned char)((masked ? 128 : 0) | (length < 126 ? length : 126));
    at = 2;
    if (length >= 126) {
        wire[at++] = (unsigned char)(length >> 8);
        wire[at++] = (unsigned char)length;
    }
    if (masked) {
        memset(wire + at, 0x37, 4);
        at += 4;
    }
    p = data;
    for (i = 0; i < length; ++i)
        wire[at + i] = p[i] ^ (masked ? 0x37 : 0);
    return write_all(c, wire, at + length);
}

static int expect_frame(struct client *c, unsigned int opcode, const void *data,
                        size_t length)
{
    unsigned char wire[260];
    size_t size;
    CHECK(read_exact(c, wire, 2) == 0 && wire[0] == (128 | opcode) &&
          !(wire[1] & 128));
    size = wire[1];
    if (size == 126) {
        CHECK(read_exact(c, wire, 2) == 0);
        size = wire[0] * 256 + wire[1];
    }
    CHECK(size == length && length <= sizeof(wire));
    CHECK(read_exact(c, wire, length) == 0 &&
          (length == 0 || memcmp(wire, data, length) == 0));
    return 0;
}

static int expect_close(struct client *c, unsigned short code)
{
    unsigned char head[2];
    unsigned char payload[125];
    CHECK(read_exact(c, head, 2) == 0 && head[0] == 136 && head[1] >= 2 &&
          head[1] <= 125);
    CHECK(read_exact(c, payload, head[1]) == 0 &&
          payload[0] * 256 + payload[1] == code);
    return 0;
}

static int client(unsigned short port, int tls)
{
    struct client c;
    unsigned char close_data[4] = {3, 232, 'o', 'k'};
    unsigned char large[257];
    unsigned char invalid[2] = {0xc0, 0xaf};
    const unsigned char noncanonical[9] = {129, 254, 0, 1, 0, 0, 0, 0, 'x'};
    const unsigned char high[14] = {130, 255, 128, 0, 0, 0, 0,
                                    0,   0,   0,   0, 0, 0, 0};
    int i;
    memset(large, 'x', sizeof(large));
    CHECK(connect_to(&c, port, tls, "/bad", 1) == 0);
    disconnect(&c);
    CHECK(connect_to(&c, port, tls, "/echo", 1) == 0);
    for (i = 0; i < 2; ++i)
        CHECK(expect_frame(&c, 2, output, sizeof(output)) == 0);
    CHECK(expect_frame(&c, 10, "e", 1) == 0);
    CHECK(expect_frame(&c, 2, output, sizeof(output)) == 0);
    CHECK(send_frame(&c, 137, "hi", 2, 1) == 0 &&
          expect_frame(&c, 10, "hi", 2) == 0);
    CHECK(send_frame(&c, 137, large, 125, 1) == 0 &&
          expect_frame(&c, 10, large, 125) == 0);
    CHECK(send_frame(&c, 129, "hold", 4, 1) == 0 &&
          send_frame(&c, 130, "next", 4, 1) == 0);
    CHECK(expect_frame(&c, 1, "hold", 4) == 0 &&
          expect_frame(&c, 2, "next", 4) == 0);
    CHECK(send_frame(&c, 130, large, 256, 1) == 0 &&
          expect_frame(&c, 1, "wide", 4) == 0);
    CHECK(send_frame(&c, 136, close_data, sizeof(close_data), 1) == 0 &&
          expect_frame(&c, 8, "", 0) == 0);
    disconnect(&c);
    for (i = 0; i < 11; ++i) {
        CHECK(connect_to(&c, port, tls, "/protocol", 1) == 0);
        if (i == 0)
            CHECK(send_frame(&c, 129, "x", 1, 0) == 0);
        if (i == 1)
            CHECK(send_frame(&c, 128, "x", 1, 1) == 0);
        if (i == 2)
            CHECK(send_frame(&c, 1, "x", 1, 1) == 0);
        if (i == 3)
            CHECK(send_frame(&c, 129, invalid, 2, 1) == 0);
        if (i == 4)
            CHECK(send_frame(&c, 136, "x", 1, 1) == 0);
        if (i == 5) {
            close_data[1] = 237;
            CHECK(send_frame(&c, 136, close_data, 2, 1) == 0);
        }
        if (i == 6) {
            memset(large, 'x', sizeof(large));
            CHECK(send_frame(&c, 130, large, sizeof(large), 1) == 0);
        }
        if (i == 7)
            CHECK(write_all(&c, noncanonical, sizeof(noncanonical)) == 0);
        if (i == 8)
            CHECK(write_all(&c, high, sizeof(high)) == 0);
        if (i == 9)
            CHECK(send_frame(&c, 193, "x", 1, 1) == 0);
        if (i == 10)
            CHECK(send_frame(&c, 137, large, 126, 1) == 0);
        if (i == 2 || i == 3 || i == 7) {
            CHECK(expect_frame(
                      &c, 1, i == 3 ? (const void *)invalid : (const void *)"x",
                      i == 3 ? 2 : 1) == 0);
            CHECK(send_frame(&c, 136, "", 0, 1) == 0 &&
                  expect_frame(&c, 8, "", 0) == 0);
        } else if (i == 4 || i == 5)
            CHECK(expect_frame(&c, 8, "", 0) == 0);
        else {
            unsigned char byte;
            int count;
            count = c.ssl != NULL ? SSL_read(c.ssl, &byte, 1)
                                  : (int)read(c.fd, &byte, 1);
            CHECK(count <= 0);
        }
        disconnect(&c);
    }
    CHECK(connect_to(&c, port, tls, "/open-error", 1) == 0 &&
          expect_close(&c, 1011) == 0);
    close_data[1] = 243;
    CHECK(send_frame(&c, 136, close_data, 2, 1) == 0);
    disconnect(&c);
    CHECK(connect_to(&c, port, tls, "/timeout", 1) == 0 &&
          expect_close(&c, 1000) == 0);
    {
        unsigned char byte;
        int count;
        count = c.ssl != NULL ? SSL_read(c.ssl, &byte, 1)
                              : (int)read(c.fd, &byte, 1);
        CHECK(count == 0);
    }
    disconnect(&c);
    CHECK(connect_to(&c, port, tls, "/wrong", 1) == 0);
    disconnect(&c);
    CHECK(connect_to(&c, port, tls, "/stop-ws", 1) == 0);
    {
        struct client stop;
        CHECK(connect_to(&stop, port, tls, "/stop", 0) == 0);
        disconnect(&stop);
    }
    CHECK(expect_close(&c, 1001) == 0);
    close_data[1] = 233;
    CHECK(send_frame(&c, 136, close_data, 2, 1) == 0);
    disconnect(&c);
    return 0;
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *p, void *data,
                           rotmos_error *error)
{
    struct fixture *f;
    rotmos_bound_listener listener;
    int result;
    (void)p;
    f = data;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    f->client = fork();
    if (f->client == -1)
        return ROTMOS_IO;
    if (f->client == 0) {
        result = client(listener.port, f->tls);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    return ROTMOS_OK;
}

static int exited(rotmos_server *s, const rotmos_process_context *p, void *data)
{
    (void)s;
    (void)p;
    (void)data;
    OBSERVE(failed_accepts == 1 && closes == 16);
    return failures ? 1 : 0;
}

static int material(struct fixture *f)
{
    EVP_PKEY *key;
    X509 *cert;
    X509_NAME *name;
    X509_EXTENSION *san;
    BIO *bio;
    char *pem;
    long length;
    key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
    cert = X509_new();
    bio = BIO_new(BIO_s_mem());
    CHECK(key != NULL && cert != NULL && bio != NULL);
    name = X509_get_subject_name(cert);
    CHECK(X509_set_version(cert, 2) &&
          ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) &&
          X509_gmtime_adj(X509_getm_notBefore(cert), -60) != NULL &&
          X509_gmtime_adj(X509_getm_notAfter(cert), 3600) != NULL &&
          X509_set_pubkey(cert, key) &&
          X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                     (const unsigned char *)"localhost", -1, -1,
                                     0) &&
          X509_set_issuer_name(cert, name));
    san =
        X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, "DNS:localhost");
    CHECK(san != NULL && X509_add_ext(cert, san, -1));
    X509_EXTENSION_free(san);
    CHECK(X509_sign(cert, key, EVP_sha256()) && PEM_write_bio_X509(bio, cert) &&
          PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL));
    length = BIO_get_mem_data(bio, &pem);
    CHECK(length > 0 && (f->bundle = malloc((size_t)length + 1)) != NULL);
    memcpy(f->bundle, pem, (size_t)length);
    f->bundle[length] = '\0';
    BIO_free(bio);
    X509_free(cert);
    EVP_PKEY_free(key);
    return 0;
}

int main(int argc, char **argv)
{
    struct fixture f;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_tls_identity identity;
    const char *domain = "localhost";
    rotmos_server *server;
    rotmos_websocket_route_config route;
    rotmos_header header;
    rotmos_error error;
    rotmos_run_result result;
    rotmos_status status;
    char copied[16] = "copied";
    int child_status;
    (void)argv;
    memset(&f, 0, sizeof(f));
    f.tls = argc > 1;
    f.client = -1;
    memset(output, 'x', sizeof(output));
    (void)signal(SIGPIPE, SIG_IGN);
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.workers.shutdown_timeout_ms = 500;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.dispatch.mode = ROTMOS_DISPATCH_CALLBACK;
    config.dispatch.on_headers = headers;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_exit = exited;
    config.callbacks.userdata = &f;
    config.timeouts.connection_idle.milliseconds = 20;
    config.timeouts.websocket_idle.milliseconds = 2000;
    if (f.tls) {
        CHECK(material(&f) == 0);
        rotmos_tls_identity_init(&identity);
        identity.mode = ROTMOS_TLS_MANUAL;
        identity.domains = &domain;
        identity.domain_count = 1;
        identity.bundle.kind = ROTMOS_MATERIAL_MEMORY;
        identity.bundle.bytes = view(f.bundle);
        config.tls.identities = &identity;
        config.tls.identity_count = 1;
        listener.kind = ROTMOS_LISTENER_HTTPS;
    }
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    rotmos_websocket_route_config_init(&route);
    route.path = "/echo";
    route.on_headers = route_admission;
    route.websocket.protocol = "test";
    route.websocket.on_open = open_ws;
    route.websocket.on_frame = frame;
    route.websocket.on_writable = writable;
    route.websocket.on_close = close_ws;
    route.websocket.output_queue_bytes = 127;
    route.websocket.max_frame_bytes = 256;
    header.name = view("X-WS");
    header.value = view(copied);
    route.websocket.headers = &header;
    route.websocket.header_count = 1;
    /* The admission callback explicitly transfers the context to WS close. */
    CHECK(rotmos_router_websocket(rotmos_server_router(server), &route, NULL,
                                  &error) == ROTMOS_OK);
    memcpy(copied, "changed", 8);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(status == ROTMOS_OK && !result.escalated);
    CHECK(waitpid(f.client, &child_status, 0) == f.client &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    free(f.bundle);
    return 0;
}
