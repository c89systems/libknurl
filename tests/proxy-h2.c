/* Observable inputs/results from Vectis test_proxy_curl_http2_duplex.c,
 * exercised through the public server/proxy API rather than curl directly. */
#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "proxy H2 %d: %s\n", __LINE__, #t);                \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    SSL_CTX *tls;
    char *ca;
    int listener;
    pid_t client, origin;
};
struct peer {
    char upload[8];
    size_t uploaded, emitted;
    int32_t stream;
    int deferred, complete, method, path, authority, length, encoding;
};
static int put(int fd, SSL *tls, const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    int n;
    while (length) {
        n = tls ? SSL_write(tls, p, (int)length) : (int)write(fd, p, length);
        CHECK(n > 0);
        p += n;
        length -= (size_t)n;
    }
    return 0;
}
static int get(int fd, void *bytes, size_t length)
{
    unsigned char *p = bytes;
    ssize_t n;
    while (length) {
        n = read(fd, p, length);
        CHECK(n > 0);
        p += n;
        length -= (size_t)n;
    }
    return 0;
}
static int alpn(SSL *tls, const unsigned char **out, unsigned char *outlen,
                const unsigned char *in, unsigned int length, void *data)
{
    static const unsigned char protocol[] = {2, 'h', '2'};
    (void)tls;
    (void)data;
    return SSL_select_next_proto((unsigned char **)out, outlen, protocol,
                                 sizeof(protocol), in,
                                 length) == OPENSSL_NPN_NEGOTIATED
               ? SSL_TLSEXT_ERR_OK
               : SSL_TLSEXT_ERR_ALERT_FATAL;
}
static ssize_t produce(nghttp2_session *session, int32_t stream, uint8_t *data,
                       size_t size, uint32_t *flags,
                       nghttp2_data_source *source, void *context)
{
    struct peer *p = context;
    (void)session;
    (void)stream;
    (void)source;
    if (size < 4)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if ((!p->emitted && p->uploaded < 4) || (p->emitted && !p->complete)) {
        p->deferred = 1;
        return NGHTTP2_ERR_DEFERRED;
    }
    memcpy(data, p->emitted ? "done" : "pong", 4);
    p->emitted += 4;
    if (p->emitted == 8)
        *flags |= NGHTTP2_DATA_FLAG_EOF;
    return 4;
}
static int data_received(nghttp2_session *session, uint8_t flags,
                         int32_t stream, const uint8_t *data, size_t size,
                         void *context)
{
    struct peer *p = context;
    (void)flags;
    if (stream != p->stream || size > sizeof(p->upload) - p->uploaded)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    memcpy(p->upload + p->uploaded, data, size);
    p->uploaded += size;
    if (p->deferred && p->uploaded >= 4) {
        p->deferred = 0;
        return nghttp2_session_resume_data(session, stream);
    }
    return 0;
}
static int header(nghttp2_session *session, const nghttp2_frame *frame,
                  const uint8_t *name, size_t name_size, const uint8_t *value,
                  size_t size, uint8_t flags, void *context)
{
    struct peer *p = context;
    static const char target[] = "/rewritten/%2F?x=1&x=2&raw=%2F";
    (void)session;
    (void)frame;
    (void)flags;
#define FIELD(n, v)                                                            \
    (name_size == sizeof(n) - 1 && !memcmp(name, n, sizeof(n) - 1) &&          \
     size == sizeof(v) - 1 && !memcmp(value, v, sizeof(v) - 1))
    if (FIELD(":method", "POST"))
        p->method = 1;
    if (FIELD(":path", target))
        p->path = 1;
    if (FIELD(":authority", "public.example"))
        p->authority = 1;
    if (FIELD("content-length", "8"))
        p->length = 1;
    if (name_size == 17 && !memcmp(name, "transfer-encoding", 17))
        p->encoding = 1;
#undef FIELD
    return 0;
}
static int frame_received(nghttp2_session *session, const nghttp2_frame *frame,
                          void *context)
{
    struct peer *p = context;
    nghttp2_data_provider body;
    nghttp2_nv status = {(uint8_t *)":status", (uint8_t *)"200", 7, 3, 0};
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        if (p->stream)
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        p->stream = frame->hd.stream_id;
        memset(&body, 0, sizeof(body));
        body.read_callback = produce;
        return nghttp2_submit_response(session, p->stream, &status, 1, &body);
    }
    if (frame->hd.type == NGHTTP2_DATA &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        p->complete = 1;
        if (p->deferred) {
            p->deferred = 0;
            return nghttp2_session_resume_data(session, p->stream);
        }
    }
    return 0;
}
static int origin(struct fixture *f)
{
    struct peer p;
    nghttp2_session_callbacks *callbacks;
    nghttp2_session *session;
    const uint8_t *output;
    const unsigned char *protocol;
    unsigned int protocol_size;
    unsigned char input[4096];
    ssize_t count;
    int fd, n;
    SSL *tls;
    memset(&p, 0, sizeof(p));
    fd = accept(f->listener, NULL, NULL);
    CHECK(fd >= 0);
    tls = SSL_new(f->tls);
    CHECK(tls && SSL_set_fd(tls, fd) && SSL_accept(tls) == 1);
    SSL_get0_alpn_selected(tls, &protocol, &protocol_size);
    CHECK(protocol_size == 2 && !memcmp(protocol, "h2", 2));
    CHECK(nghttp2_session_callbacks_new(&callbacks) == 0);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                         frame_received);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                              data_received);
    CHECK(nghttp2_session_server_new(&session, callbacks, &p) == 0);
    nghttp2_session_callbacks_del(callbacks);
    CHECK(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0) == 0);
    do {
        while ((count = nghttp2_session_mem_send(session, &output)) > 0)
            CHECK(put(fd, tls, output, (size_t)count) == 0);
        CHECK(count == 0);
        if (p.emitted == 8)
            break;
        n = SSL_read(tls, input, sizeof(input));
        CHECK(n > 0 &&
              nghttp2_session_mem_recv(session, input, (size_t)n) == n);
    } while (1);
    CHECK(p.complete && p.uploaded == 8 && !memcmp(p.upload, "pingrest", 8));
    CHECK(p.method && p.path && p.authority && p.length && !p.encoding);
    nghttp2_session_del(session);
    SSL_free(tls);
    CHECK(close(fd) == 0);
    return 0;
}
static int connect_to(unsigned short port)
{
    struct sockaddr_in a;
    struct timeval timeout;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
        close(fd);
        return -1;
    }
    return fd;
}
static int line(int fd, char *out, size_t size)
{
    size_t used = 0;
    do {
        CHECK(used + 1 < size && get(fd, out + used, 1) == 0);
        out[++used] = 0;
    } while (used < 2 || memcmp(out + used - 2, "\r\n", 2));
    return 0;
}
static int body_piece(int fd, const char *expected)
{
    char prefix[32], data[8];
    size_t received = 0, size;
    while (received < 4) {
        CHECK(line(fd, prefix, sizeof(prefix)) == 0);
        size = (size_t)strtoul(prefix, NULL, 16);
        CHECK(size && size <= 4 - received && get(fd, data, size) == 0);
        CHECK(!memcmp(data, expected + received, size));
        CHECK(get(fd, data, 2) == 0 && !memcmp(data, "\r\n", 2));
        received += size;
    }
    return 0;
}
static int client(unsigned short port)
{
    static const char request[] = "POST /duplex HTTP/1.1\r\nHost: localhost\r\n"
                                  "Content-Length: 8\r\n\r\nping";
    static const char stop[] = "GET /stop HTTP/1.1\r\nHost: localhost\r\n"
                               "Connection: close\r\n\r\n";
    char text[512];
    int fd = connect_to(port);
    CHECK(fd >= 0 && put(fd, NULL, request, sizeof(request) - 1) == 0);
    CHECK(line(fd, text, sizeof(text)) == 0 && strstr(text, " 200 "));
    do {
        CHECK(line(fd, text, sizeof(text)) == 0);
    } while (strcmp(text, "\r\n"));
    CHECK(body_piece(fd, "pong") == 0);
    CHECK(put(fd, NULL, "rest", 4) == 0 && body_piece(fd, "done") == 0);
    CHECK(line(fd, text, sizeof(text)) == 0 && !strcmp(text, "0\r\n"));
    CHECK(line(fd, text, sizeof(text)) == 0 && !strcmp(text, "\r\n"));
    CHECK(close(fd) == 0);
    fd = connect_to(port);
    CHECK(fd >= 0 && put(fd, NULL, stop, sizeof(stop) - 1) == 0);
    CHECK(line(fd, text, sizeof(text)) == 0 && strstr(text, " 200 "));
    CHECK(close(fd) == 0);
    return 0;
}
static rotmos_status rewrite(const rotmos_proxy_inbound *in,
                             rotmos_proxy_outbound *out, void *context,
                             rotmos_error *error)
{
    rotmos_status status;
    (void)in;
    (void)context;
    status = rotmos_proxy_outbound_set_path(out, "/rewritten/%2F", error);
    if (status == ROTMOS_OK)
        status = rotmos_proxy_outbound_set_query(out, "x=1&x=2&raw=%2F", error);
    if (status == ROTMOS_OK)
        status = rotmos_proxy_outbound_set_host(out, "public.example", error);
    return status;
}
static rotmos_status stop(rotmos_request *request, rotmos_reply *reply,
                          void *data, rotmos_error *error)
{
    (void)data;
    (void)rotmos_server_request_stop(rotmos_request_server(request), NULL);
    return rotmos_reply_text(reply, 200, "text/plain", "ok", error);
}
static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process, void *data,
                           rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_bound_listener bound;
    int status;
    (void)process;
    if (rotmos_server_listener_at(server, 0, &bound, error) != ROTMOS_OK)
        return ROTMOS_STATE;
    f->origin = fork();
    if (!f->origin) {
        status = origin(f);
        if (status)
            (void)kill(getppid(), SIGTERM);
        _exit(status);
    }
    f->client = fork();
    if (!f->client) {
        status = client(bound.port);
        if (status)
            (void)kill(getppid(), SIGTERM);
        _exit(status);
    }
    return f->origin > 0 && f->client > 0 ? ROTMOS_OK : ROTMOS_IO;
}
static int material(struct fixture *f)
{
    EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
    X509 *cert = X509_new();
    BIO *bio = BIO_new(BIO_s_mem());
    X509_NAME *name;
    X509_EXTENSION *san;
    char *pem;
    long length;
    CHECK(key && cert && bio);
    name = X509_get_subject_name(cert);
    CHECK(X509_set_version(cert, 2) &&
          ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) &&
          X509_gmtime_adj(X509_getm_notBefore(cert), -60) &&
          X509_gmtime_adj(X509_getm_notAfter(cert), 3600) &&
          X509_set_pubkey(cert, key) &&
          X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                     (const unsigned char *)"localhost", -1, -1,
                                     0) &&
          X509_set_issuer_name(cert, name));
    san =
        X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, "DNS:localhost");
    CHECK(san && X509_add_ext(cert, san, -1));
    X509_EXTENSION_free(san);
    CHECK(X509_sign(cert, key, EVP_sha256()) && PEM_write_bio_X509(bio, cert));
    length = BIO_get_mem_data(bio, &pem);
    CHECK(length > 0 && (f->ca = malloc((size_t)length + 1)));
    memcpy(f->ca, pem, (size_t)length);
    f->ca[length] = 0;
    f->tls = SSL_CTX_new(TLS_server_method());
    CHECK(f->tls && SSL_CTX_use_certificate(f->tls, cert) &&
          SSL_CTX_use_PrivateKey(f->tls, key));
    SSL_CTX_set_alpn_select_cb(f->tls, alpn, NULL);
    EVP_PKEY_free(key);
    X509_free(cert);
    BIO_free(bio);
    return 0;
}
int main(void)
{
    struct fixture f;
    struct sockaddr_in a;
    socklen_t size = sizeof(a);
    char target[80];
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_proxy_config proxy;
    rotmos_route_config route;
    rotmos_server *server;
    rotmos_run_result result;
    rotmos_error error;
    rotmos_status status;
    int child;
    memset(&f, 0, sizeof(f));
    CHECK(material(&f) == 0);
    f.listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(f.listener >= 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(!bind(f.listener, (struct sockaddr *)&a, sizeof(a)) &&
          !listen(f.listener, 1) &&
          !getsockname(f.listener, (struct sockaddr *)&a, &size));
    (void)snprintf(target, sizeof(target), "https://localhost:%u",
                   ntohs(a.sin_port));
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.userdata = &f;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    rotmos_proxy_config_init(&proxy);
    proxy.path = "/duplex";
    proxy.target = target;
    proxy.tls_ca_pem = f.ca;
    proxy.rewrite = rewrite;
    CHECK(rotmos_router_proxy(rotmos_server_router(server), &proxy, NULL,
                              &error) == ROTMOS_OK);
    rotmos_route_config_init(&route);
    route.path = "/stop";
    route.handler = stop;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(waitpid(f.client, &child, 0) == f.client && WIFEXITED(child) &&
          !WEXITSTATUS(child));
    CHECK(waitpid(f.origin, &child, 0) == f.origin && WIFEXITED(child) &&
          !WEXITSTATUS(child));
    CHECK(status == ROTMOS_OK && !result.escalated && !result.has_failure);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    CHECK(close(f.listener) == 0);
    SSL_CTX_free(f.tls);
    free(f.ca);
    return 0;
}
