#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            (void)fprintf(stderr, "proxy WS %d: %s\n", __LINE__, #t);          \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct wire {
    int fd;
    SSL *tls;
};
struct fixture {
    int listener;
    int control[2];
    int tls;
    pid_t client;
    pid_t origin;
    SSL_CTX *server_ctx;
    SSL_CTX *client_ctx;
    char *bundle;
};
static int failures;
static int modifications;
static int errors;
static rotmos_bytes view(const char *text)
{
    rotmos_bytes b;
    b.data = text;
    b.length = strlen(text);
    return b;
}
static int put(struct wire *w, const void *data, size_t size)
{
    const unsigned char *p;
    int n;
    p = data;
    while (size != 0) {
        n = w->tls != NULL ? SSL_write(w->tls, p, (int)size)
                           : (int)write(w->fd, p, size);
        CHECK(n > 0);
        p += n;
        size -= (size_t)n;
    }
    return 0;
}
static int get(struct wire *w, void *data, size_t size)
{
    unsigned char *p;
    int n;
    p = data;
    while (size != 0) {
        n = w->tls != NULL ? SSL_read(w->tls, p, (int)size)
                           : (int)read(w->fd, p, size);
        CHECK(n > 0);
        p += n;
        size -= (size_t)n;
    }
    return 0;
}
static int text(struct wire *w, const char *s) { return put(w, s, strlen(s)); }
static int line(struct wire *w, char *out, size_t capacity)
{
    size_t n;
    n = 0;
    do {
        CHECK(n + 1 < capacity && get(w, out + n, 1) == 0);
        out[++n] = 0;
    } while (n < 2 || memcmp(out + n - 2, "\r\n", 2) != 0);
    return 0;
}
static int head(struct wire *w, char *out, size_t capacity)
{
    size_t n;
    n = 0;
    do {
        CHECK(n + 1 < capacity && get(w, out + n, 1) == 0);
        out[++n] = 0;
    } while (strstr(out, "\r\n\r\n") == NULL);
    return 0;
}
static void close_wire(struct wire *w)
{
    if (w->tls != NULL) {
        (void)SSL_shutdown(w->tls);
        SSL_free(w->tls);
    }
    (void)close(w->fd);
}
static int connect_to(struct fixture *f, unsigned short port, struct wire *w)
{
    struct sockaddr_in address;
    struct timeval timeout;
    memset(w, 0, sizeof(*w));
    w->fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(w->fd >= 0);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    CHECK(setsockopt(w->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) == 0 &&
          setsockopt(w->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout)) == 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    CHECK(connect(w->fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    if (f->tls) {
        w->tls = SSL_new(f->client_ctx);
        CHECK(w->tls != NULL && SSL_set_fd(w->tls, w->fd) &&
              SSL_set_tlsext_host_name(w->tls, "localhost") &&
              SSL_set1_host(w->tls, "localhost") && SSL_connect(w->tls) == 1);
    }
    return 0;
}
static int opening(struct wire *w, const char *path)
{
    char request[1024];
    (void)snprintf(
        request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: "
        "13\r\n"
        "Sec-WebSocket-Key: "
        "dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: test\r\n"
        "Sec-WebSocket-Extensions: x-test\r\n\r\n",
        path);
    return text(w, request);
}
static void payload(unsigned char *bytes, int masked)
{
    size_t i;
    for (i = 0; i < 65536; ++i)
        bytes[i] = (unsigned char)(i % 251);
    for (i = 0; i < 65536; i += 32768) {
        bytes[i] = i == 0 ? 0x42 : 0x80;
        bytes[i + 1] = masked ? 0xfe : 0x7e;
        bytes[i + 2] = 0x7f;
        bytes[i + 3] = masked ? 0xf8 : 0xfc;
    }
}
static int no_early_bytes(struct wire *w)
{
    int flags;
    int n;
    int error;
    char byte;
    flags = fcntl(w->fd, F_GETFL, 0);
    CHECK(flags >= 0 && fcntl(w->fd, F_SETFL, flags | O_NONBLOCK) == 0);
    if (w->tls != NULL) {
        n = SSL_peek(w->tls, &byte, 1);
        error = SSL_get_error(w->tls, n);
        CHECK(n < 0 && error == SSL_ERROR_WANT_READ);
    } else
        CHECK(recv(w->fd, &byte, 1, MSG_PEEK) < 0);
    CHECK(fcntl(w->fd, F_SETFL, flags) == 0);
    return 0;
}
static int origin(struct fixture *f)
{
    struct wire w;
    struct wire control;
    char metadata[4096];
    char byte;
    unsigned char *expected;
    unsigned char *received;
    unsigned char *output;
    size_t at;
    control.fd = f->control[1];
    control.tls = NULL;
    expected = malloc(65536);
    received = malloc(65536);
    output = malloc(65536);
    CHECK(expected != NULL && received != NULL && output != NULL);
    payload(expected, 1);
    payload(output, 0);
    for (;;) {
        w.fd = accept(f->listener, NULL, NULL);
        w.tls = NULL;
        CHECK(w.fd >= 0);
        if (f->tls) {
            w.tls = SSL_new(f->server_ctx);
            CHECK(w.tls != NULL && SSL_set_fd(w.tls, w.fd) &&
                  SSL_accept(w.tls) == 1);
            CHECK(SSL_get0_peer_certificate(w.tls) != NULL);
        }
        CHECK(head(&w, metadata, sizeof(metadata)) == 0);
        if (strstr(metadata, " /base/tunnel ") != NULL) {
            CHECK((strstr(metadata, "Sec-WebSocket-Protocol: test") != NULL ||
                   strstr(metadata, "sec-websocket-protocol: test") != NULL) &&
                  no_early_bytes(&w) == 0);
            CHECK(text(&w,
                       "HTTP/1.1 103 Early Hints\r\nLink: </ws>\r\n\r\n"
                       "HTTP/1.1 101 Switching Protocols\r\nConnection: "
                       "Upgrade\r\nUpgrade: websocket\r\n"
                       "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                       "Sec-WebSocket-Protocol: "
                       "test\r\nSec-WebSocket-Extensions: x-test\r\n\r\n") ==
                  0);
            CHECK(put(&w, output, 128) == 0 && get(&w, received, 65536) == 0 &&
                  memcmp(received, expected, 65536) == 0);
            if (!f->tls)
                CHECK(read(w.fd, &byte, 1) == 0);
            CHECK(put(&w, output + 128, 65536 - 128) == 0);
        } else if (strstr(metadata, " /base/reject ") != NULL) {
            CHECK(text(&w, "HTTP/1.1 103 Early Hints\r\nLink: </denied>\r\n\r\n"
                           "HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: "
                           "chunked\r\nTrailer: X-End\r\n"
                           "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\nConnection: "
                           "close\r\n\r\n400\r\n") == 0 &&
                  put(&w, output, 1024) == 0 && text(&w, "\r\n") == 0 &&
                  get(&control, &byte, 1) == 0);
            for (at = 1024; at < 65536; at += 1024)
                CHECK(text(&w, "400\r\n") == 0 &&
                      put(&w, output + at, 1024) == 0 && text(&w, "\r\n") == 0);
            CHECK(text(&w, "0\r\nX-End: yes\r\n\r\n") == 0);
        } else if (strstr(metadata, " /base/invalid ") != NULL) {
            CHECK(no_early_bytes(&w) == 0 &&
                  text(&w,
                       "HTTP/1.1 101 Switching Protocols\r\n"
                       "Connection: Upgrade\r\nUpgrade: "
                       "websocket\r\nSec-WebSocket-Accept: invalid\r\n\r\n") ==
                      0);
        } else if (strstr(metadata, " /base/stall ") != NULL) {
            CHECK(text(&control, "r") == 0 && get(&control, &byte, 1) == 0);
        } else if (strstr(metadata, " /base/truncated ") != NULL) {
            CHECK(text(&w, "HTTP/1.1 403 Forbidden\r\nContent-Length: "
                           "10\r\n\r\nabc") == 0 &&
                  get(&control, &byte, 1) == 0);
        } else if (strstr(metadata, " /base/badedit ") != NULL) {
            CHECK(text(&w, "HTTP/1.1 403 Forbidden\r\nX-Bad: "
                           "yes\r\nContent-Length: 0\r\n\r\n") == 0);
        } else {
            CHECK(strstr(metadata, " /base/finish ") != NULL);
            CHECK(text(&w, "HTTP/1.1 200 OK\r\nContent-Length: "
                           "4\r\nConnection: close\r\n\r\npong") == 0);
            close_wire(&w);
            break;
        }
        close_wire(&w);
    }
    free(expected);
    free(received);
    free(output);
    return 0;
}
static int chunks(struct wire *w, unsigned char *data, size_t *count,
                  int *trailer)
{
    char line_buffer[256];
    unsigned long size;
    *count = 0;
    CHECK(line(w, line_buffer, sizeof(line_buffer)) == 0);
    size = strtoul(line_buffer, NULL, 16);
    CHECK(size <= 4096);
    if (size == 0) {
        do {
            CHECK(line(w, line_buffer, sizeof(line_buffer)) == 0);
            if (strcmp(line_buffer, "X-End: yes\r\n") == 0)
                ++*trailer;
        } while (strcmp(line_buffer, "\r\n") != 0);
    } else {
        CHECK(get(w, data, (size_t)size) == 0 && get(w, line_buffer, 2) == 0 &&
              memcmp(line_buffer, "\r\n", 2) == 0);
        *count = (size_t)size;
    }
    return 0;
}
static int local(struct fixture *f, unsigned short port, const char *path,
                 int code, int upgrade)
{
    struct wire w;
    char metadata[4096];
    char request[512];
    char drain[1024];
    int n;
    CHECK(connect_to(f, port, &w) == 0);
    if (upgrade)
        CHECK(opening(&w, path) == 0);
    else {
        (void)snprintf(
            request, sizeof(request),
            "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
            path);
        CHECK(text(&w, request) == 0);
    }
    CHECK(head(&w, metadata, sizeof(metadata)) == 0);
    (void)snprintf(request, sizeof(request), " %d ", code);
    if (strstr(metadata, request) == NULL)
        (void)fprintf(stderr, "%s: %s\n", path, metadata);
    CHECK(strstr(metadata, request) != NULL);
    do {
        n = w.tls != NULL ? SSL_read(w.tls, drain, sizeof(drain))
                          : (int)read(w.fd, drain, sizeof(drain));
    } while (n > 0);
    CHECK(w.tls != NULL || n == 0);
    close_wire(&w);
    return 0;
}
static int client(struct fixture *f, unsigned short port)
{
    struct wire w;
    struct wire control;
    char metadata[4096];
    unsigned char chunk[4096];
    unsigned char *up;
    unsigned char *down;
    unsigned char *expected;
    size_t received;
    size_t count;
    int trailers;
    char byte;
    control.fd = f->control[0];
    control.tls = NULL;
    up = malloc(65536);
    down = malloc(65536);
    expected = malloc(65536);
    CHECK(up != NULL && down != NULL && expected != NULL);
    payload(up, 1);
    payload(expected, 0);
    CHECK(connect_to(f, port, &w) == 0 && opening(&w, "/tunnel") == 0 &&
          put(&w, up, 16) == 0);
    CHECK(head(&w, metadata, sizeof(metadata)) == 0);
    if (strstr(metadata, " 103 ") == NULL)
        (void)fprintf(stderr, "opening response: %s\n", metadata);
    CHECK(strstr(metadata, " 103 ") != NULL);
    CHECK(head(&w, metadata, sizeof(metadata)) == 0);
    if (strstr(metadata, " 101 ") == NULL)
        (void)fprintf(stderr, "tunnel: %s\n", metadata);
    CHECK(strstr(metadata, " 101 ") != NULL &&
          strstr(metadata, "x-test") != NULL &&
          strstr(metadata, "X-Edited") == NULL && get(&w, down, 128) == 0);
    CHECK(put(&w, up + 16, 65536 - 16) == 0);
    if (!f->tls)
        CHECK(shutdown(w.fd, SHUT_WR) == 0);
    CHECK(get(&w, down + 128, 65536 - 128) == 0 &&
          memcmp(down, expected, 65536) == 0);
    if (w.tls == NULL)
        CHECK(read(w.fd, &byte, 1) == 0);
    else
        CHECK(SSL_read(w.tls, &byte, 1) <= 0);
    close_wire(&w);
    CHECK(connect_to(f, port, &w) == 0 && opening(&w, "/reject") == 0 &&
          head(&w, metadata, sizeof(metadata)) == 0 &&
          strstr(metadata, " 103 ") != NULL &&
          head(&w, metadata, sizeof(metadata)) == 0);
    CHECK(strstr(metadata, " 429 ") != NULL &&
          strstr(metadata, "a=1") != NULL && strstr(metadata, "b=2") != NULL &&
          strstr(metadata, "X-Edited") != NULL);
    received = 0;
    trailers = 0;
    do {
        CHECK(chunks(&w, chunk, &count, &trailers) == 0);
        CHECK(count <= 65536 - received &&
              memcmp(chunk, expected + received, count) == 0);
        if (received == 0)
            CHECK(text(&control, "n") == 0);
        received += count;
    } while (count != 0);
    CHECK(received == 65536 && trailers == 1);
    close_wire(&w);
    CHECK(local(f, port, "/invalid", 503, 1) == 0 &&
          local(f, port, "/badedit", 503, 1) == 0);
    CHECK(connect_to(f, port, &w) == 0 && opening(&w, "/stall") == 0 &&
          get(&control, &byte, 1) == 0);
    CHECK(local(f, port, "/ping", 200, 0) == 0 &&
          local(f, port, "/finish", 503, 0) == 0 &&
          local(f, port, "/tunnel", 503, 1) == 0);
    CHECK(head(&w, metadata, sizeof(metadata)) == 0 &&
          strstr(metadata, " 504 ") != NULL && text(&control, "n") == 0);
    close_wire(&w);
    CHECK(connect_to(f, port, &w) == 0 && opening(&w, "/truncated") == 0 &&
          head(&w, metadata, sizeof(metadata)) == 0 &&
          strstr(metadata, " 429 ") != NULL);
    CHECK(chunks(&w, chunk, &count, &trailers) == 0 && count == 3 &&
          memcmp(chunk, "abc", 3) == 0 && text(&control, "n") == 0);
    if (!f->tls)
        CHECK(read(w.fd, &byte, 1) == 0);
    else
        CHECK(SSL_read(w.tls, &byte, 1) <= 0);
    close_wire(&w);
    CHECK(local(f, port, "/finish", 200, 0) == 0 &&
          local(f, port, "/stop", 200, 0) == 0);
    free(up);
    free(down);
    free(expected);
    return 0;
}
static rotmos_status modify(rotmos_proxy_response *response, void *data,
                            rotmos_error *error)
{
    size_t i;
    const char *name;
    const char *value;
    (void)data;
    ++modifications;
    for (i = 0; i < rotmos_proxy_response_header_count(response); ++i) {
        if (rotmos_proxy_response_header_at(response, i, &name, &value) !=
            ROTMOS_OK)
            return ROTMOS_INVALID;
        if (strcmp(name, "X-Bad") == 0) {
            (void)rotmos_proxy_response_add_header(response, "Content-Length",
                                                   "0", error);
            return ROTMOS_OK;
        }
    }
    if (rotmos_proxy_response_status(response) == 403 &&
        rotmos_proxy_response_set_status(response, 429, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    return rotmos_proxy_response_add_header(response, "X-Edited", "yes", error);
}
static rotmos_status gateway(const rotmos_error *cause, int code,
                             rotmos_proxy_local_response *reply, void *data,
                             rotmos_error *error)
{
    (void)data;
    if (cause->status == ROTMOS_OK)
        ++failures;
    ++errors;
    return rotmos_proxy_local_respond(reply, code == 502 ? 503 : code,
                                      "gateway", 7, error);
}
static rotmos_status ordinary(rotmos_request *request, rotmos_reply *reply,
                              void *data, rotmos_error *error)
{
    (void)data;
    if (rotmos_request_path(request).length == 5 &&
        memcmp(rotmos_request_path(request).data, "/stop", 5) == 0)
        (void)rotmos_server_request_stop(rotmos_request_server(request), error);
    return rotmos_reply_bytes(reply, 200, "text/plain", view("pong"), error);
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
    f->origin = fork();
    if (f->origin == 0) {
        (void)signal(SIGTERM, SIG_DFL);
        (void)close(f->control[0]);
        result = origin(f);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    if (f->origin < 0)
        return ROTMOS_IO;
    f->client = fork();
    if (f->client == 0) {
        (void)signal(SIGTERM, SIG_DFL);
        (void)close(f->control[1]);
        (void)close(f->listener);
        result = client(f, listener.port);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    if (f->client < 0)
        return ROTMOS_IO;
    (void)close(f->control[0]);
    (void)close(f->control[1]);
    (void)close(f->listener);
    return ROTMOS_OK;
}
static rotmos_status worker_start(rotmos_server *server,
                                  const rotmos_process_context *p, void *data,
                                  rotmos_error *error)
{
    struct fixture *f;
    (void)server;
    (void)p;
    (void)error;
    f = data;
    (void)close(f->control[0]);
    (void)close(f->control[1]);
    (void)close(f->listener);
    return ROTMOS_OK;
}
static int exited(rotmos_server *server, const rotmos_process_context *p,
                  void *data)
{
    (void)server;
    (void)p;
    (void)data;
    if (modifications != 4 || errors != 3) {
        (void)fprintf(stderr, "proxy WS hooks: modify=%d errors=%d\n",
                      modifications, errors);
        ++failures;
    }
    return failures ? 1 : 0;
}
static void worker_event(rotmos_server *server,
                         const rotmos_worker_event *event, void *data)
{
    (void)server;
    (void)data;
    if (event->kind == ROTMOS_WORKER_EXITED &&
        (!event->expected || event->signal_number || event->exit_status))
        (void)fprintf(stderr, "proxy WS child: role=%d signal=%d exit=%d %s\n",
                      (int)event->process.role, event->signal_number,
                      event->exit_status, event->error.message);
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
    f->bundle[length] = 0;
    f->server_ctx = SSL_CTX_new(TLS_server_method());
    f->client_ctx = SSL_CTX_new(TLS_client_method());
    CHECK(f->server_ctx != NULL && f->client_ctx != NULL &&
          SSL_CTX_use_certificate(f->server_ctx, cert) &&
          SSL_CTX_use_PrivateKey(f->server_ctx, key) &&
          X509_STORE_add_cert(SSL_CTX_get_cert_store(f->server_ctx), cert) &&
          X509_STORE_add_cert(SSL_CTX_get_cert_store(f->client_ctx), cert));
    SSL_CTX_set_verify(f->server_ctx,
                       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    SSL_CTX_set_verify(f->client_ctx, SSL_VERIFY_PEER, NULL);
    /* Downstream fixture client also uses this identity if the listener later
     * enables client authentication; upstream mTLS is mandatory in this test.
     */
    CHECK(SSL_CTX_use_certificate(f->client_ctx, cert) &&
          SSL_CTX_use_PrivateKey(f->client_ctx, key));
    BIO_free(bio);
    X509_free(cert);
    EVP_PKEY_free(key);
    return 0;
}
int main(int argc, char **argv)
{
    struct fixture f;
    struct sockaddr_in address;
    socklen_t length;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_tls_identity identity;
    rotmos_proxy_config proxy;
    rotmos_route_config route;
    rotmos_server *server;
    rotmos_run_result result;
    rotmos_error error;
    rotmos_status status;
    const char *domain = "localhost";
    const char *paths[] = {"/tunnel", "/reject",    "/invalid", "/badedit",
                           "/stall",  "/truncated", "/finish"};
    char target[128];
    size_t i;
    int child_status;
    (void)argv;
    memset(&f, 0, sizeof(f));
    f.tls = argc > 1;
    f.client = f.origin = -1;
    (void)signal(SIGPIPE, SIG_IGN);
    f.listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(f.listener >= 0 &&
          socketpair(AF_UNIX, SOCK_STREAM, 0, f.control) == 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(f.listener, (struct sockaddr *)&address, sizeof(address)) == 0 &&
          listen(f.listener, 8) == 0);
    length = sizeof(address);
    CHECK(getsockname(f.listener, (struct sockaddr *)&address, &length) == 0);
    (void)snprintf(target, sizeof(target), "%s://%s:%u/base",
                   f.tls ? "https" : "http", f.tls ? "localhost" : "127.0.0.1",
                   ntohs(address.sin_port));
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    listener.bind = "127.0.0.1";
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.proxy.active_exchanges = 1;
    config.io.input_queue_bytes = config.io.output_queue_bytes = 4096;
    config.io.chunk_bytes = 1024;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_start = worker_start;
    config.callbacks.on_worker_exit = exited;
    config.callbacks.on_worker_event = worker_event;
    config.callbacks.userdata = &f;
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
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        rotmos_proxy_config_init(&proxy);
        proxy.path = paths[i];
        proxy.target = target;
        proxy.modify_response = modify;
        proxy.on_error = gateway;
        proxy.buffer_limit_bytes = 8192;
        proxy.connect_timeout_ms = 1000;
        proxy.idle_timeout_ms = strcmp(paths[i], "/stall") == 0 ? 200 : 2000;
        if (f.tls) {
            proxy.tls_ca_pem = proxy.tls_client_cert_pem =
                proxy.tls_client_key_pem = f.bundle;
        }
        CHECK(rotmos_router_proxy(rotmos_server_router(server), &proxy, NULL,
                                  &error) == ROTMOS_OK);
    }
    rotmos_route_config_init(&route);
    route.path = "/ping";
    route.handler = ordinary;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    route.path = "/stop";
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    if (f.bundle != NULL)
        f.bundle[0] = '!';
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK) {
        (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
        if (f.client > 0) {
            (void)kill(f.client, SIGTERM);
            (void)waitpid(f.client, NULL, 0);
        }
        if (f.origin > 0) {
            (void)kill(f.origin, SIGTERM);
            (void)waitpid(f.origin, NULL, 0);
        }
        (void)rotmos_server_destroy(server, NULL);
        return 1;
    }
    CHECK(!result.escalated && result.recovered_failures.high == 0 &&
          result.recovered_failures.low == 0);
    CHECK(waitpid(f.client, &child_status, 0) == f.client &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(waitpid(f.origin, &child_status, 0) == f.origin &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    SSL_CTX_free(f.server_ctx);
    SSL_CTX_free(f.client_ctx);
    free(f.bundle);
    return 0;
}
