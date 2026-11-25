#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <openssl/hmac.h>
#include <openssl/core_names.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t server_pid;
static pid_t runner_pid;
static pid_t secondary_runner;

#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "auth:%d: %s\n", __LINE__, #x);                    \
            if (server_pid != 0 && getpid() != server_pid)                     \
                (void)kill(server_pid, SIGTERM);                               \
            if (runner_pid > 0) {                                              \
                (void)kill(runner_pid, SIGTERM);                               \
                (void)waitpid(runner_pid, NULL, 0);                            \
            }                                                                  \
            if (secondary_runner > 0) {                                        \
                (void)kill(secondary_runner, SIGTERM);                         \
                (void)waitpid(secondary_runner, NULL, 0);                      \
            }                                                                  \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static const char password[] = "a test password with spaces";
static const char password_form[] =
    "username=alice&password=a+test+password+with+spaces";
static const char partner_credentials[] =
    "username=remote&password=the+other+test+password";
struct fixture {
    knurl_auth_json *store;
    knurl_auth_json *mail_store;
    knurl_auth_json *users_store;
    char cert[1024], key[1024], static_file[1024];
    char directory_status[1024], store_path[1024], mail_store_path[1024];
    char enrollment[KNURL_AUTH_TOTP_URI_CAPACITY], client_secret[66];
    char flow_enrollment[2][KNURL_AUTH_TOTP_URI_CAPACITY];
    char jwks[1024], next_jwks[1024], issuer[128], ca_pem[2048];
    unsigned short tls, plain, smtp, proxy, oauth;
    int entered[2], resume[2], headers[2], mail[2];
    int token_request[2], token_resume[2];
    pid_t client;
    SSL_CTX *tls_client;
    EVP_PKEY *signing_key, *rotated_key;
};

struct connection {
    int fd;
    SSL *tls;
};

static struct connection connect_to(struct fixture *f, int tls)
{
    struct sockaddr_in address;
    struct timeval timeout = {5, 0};
    struct connection c;
    c.fd = socket(AF_INET, SOCK_STREAM, 0);
    c.tls = NULL;
    CHECK(c.fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(tls ? f->tls : f->plain);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) == 0);
    CHECK(connect(c.fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    if (tls) {
        c.tls = SSL_new(f->tls_client);
        CHECK(c.tls != NULL && SSL_set_fd(c.tls, c.fd) == 1 &&
              SSL_set_tlsext_host_name(c.tls, "localhost") == 1 &&
              SSL_set1_host(c.tls, "localhost") == 1 &&
              SSL_connect(c.tls) == 1);
    }
    return c;
}

static void send_request(struct connection *c, const char *request)
{
    size_t length = strlen(request);
    CHECK((c->tls != NULL ? SSL_write(c->tls, request, (int)length)
                          : write(c->fd, request, length)) == (int)length);
}

static int read_response(struct connection *c, char out[4096])
{
    int length, code = 0;
    size_t size = 0;
    while (size < 4095) {
        length = c->tls != NULL
                     ? SSL_read(c->tls, out + size, (int)(4095 - size))
                     : (int)read(c->fd, out + size, 4095 - size);
        if (length <= 0)
            break;
        size += (size_t)length;
    }
    SSL_free(c->tls);
    close(c->fd);
    out[size] = 0;
    (void)sscanf(out, "HTTP/1.1 %d", &code);
    return code;
}

static void check_response_ready(struct connection *c)
{
    struct timeval quick = {0, 800000}, normal = {5, 0};
    char byte;
    CHECK(setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &quick, sizeof(quick)) ==
          0);
    CHECK(SSL_peek(c->tls, &byte, 1) == 1);
    CHECK(setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &normal, sizeof(normal)) ==
          0);
}

static int exchange(struct fixture *f, int tls, const char *method,
                    const char *path, const char *extra, const char *body,
                    char out[4096])
{
    struct connection c = connect_to(f, tls);
    char wire[2048];
    int n, status;
    n = snprintf(wire, sizeof(wire),
                 "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                 "%s%sContent-Length: %lu\r\n\r\n%s",
                 method, path, extra != NULL ? extra : "",
                 body != NULL
                     ? "Content-Type: application/x-www-form-urlencoded\r\n"
                     : "",
                 body != NULL ? (unsigned long)strlen(body) : 0UL,
                 body != NULL ? body : "");
    CHECK(n > 0 && (size_t)n < sizeof(wire));
    send_request(&c, wire);
    status = read_response(&c, out);
    CHECK(status > 0);
    return status;
}

static char note(int fd, int timeout)
{
    struct pollfd poller;
    char byte = 0;
    poller.fd = fd;
    poller.events = POLLIN;
    if (poll(&poller, 1, timeout) == 1)
        CHECK(read(fd, &byte, 1) == 1);
    return byte;
}

static knurl_status provider(knurl_auth_operation operation,
                             knurl_auth_exchange *x, void *userdata,
                             knurl_error *error)
{
    struct fixture *f = userdata;
    (void)error;
    if (operation == KNURL_AUTH_PASSWORD || operation == KNURL_AUTH_EMAIL) {
        CHECK(strcmp(x->users_id, "staff") == 0);
        if (strcmp(x->subject, "remote") != 0 ||
            (operation == KNURL_AUTH_PASSWORD &&
             strcmp(x->secret, password) != 0))
            return KNURL_EOF;
        strcpy(x->identity.subject, "directory-user");
        if (operation == KNURL_AUTH_EMAIL)
            strcpy(x->email, "remote@example.test");
        return KNURL_OK;
    }
    CHECK(operation == KNURL_AUTH_MACHINE_VERIFY);
    if (strcmp(x->secret, "Bearer fixture-hold") != 0)
        return KNURL_EOF;
    CHECK(write(f->entered[1], "e", 1) == 1);
    CHECK(note(f->resume[0], 3000) == 'r');
    strcpy(x->identity.subject, "sleeping-service");
    return KNURL_OK;
}

static knurl_status partner_users(knurl_auth_operation operation,
                                  knurl_auth_exchange *x, void *userdata,
                                  knurl_error *error)
{
    (void)userdata;
    (void)error;
    CHECK(operation == KNURL_AUTH_PASSWORD &&
          strcmp(x->users_id, "partner") == 0);
    if (strcmp(x->subject, "remote") != 0 ||
        strcmp(x->secret, "the other test password") != 0)
        return KNURL_EOF;
    strcpy(x->identity.subject, "directory-user");
    return KNURL_OK;
}

static knurl_status send_code(const char *address, const char *code, void *data,
                              knurl_error *error)
{
    struct fixture *f = data;
    (void)error;
    CHECK(strcmp(address, "alice@example.test") == 0 ||
          strcmp(address, "dave@example.test") == 0 ||
          strcmp(address, "remote@example.test") == 0);
    CHECK(write(f->mail[1], code, 6) == 6);
    return KNURL_OK;
}

static knurl_status gated_mail(const char *address, const char *code,
                               void *data, knurl_error *error)
{
    struct fixture *f = data;
    char action;
    CHECK(write(f->entered[1], "m", 1) == 1);
    action = note(f->resume[0], 3000);
    CHECK(action == 'f' || action == 'r');
    return action == 'f' ? KNURL_BACKEND
                         : send_code(address, code, data, error);
}

static knurl_status text(knurl_request *request, knurl_reply *reply, void *data,
                         knurl_error *error)
{
    const knurl_auth_identity *identity = knurl_request_auth(request);
    (void)data;
    return knurl_reply_text(reply, 200, "text/plain",
                            identity != NULL ? identity->subject : "fast",
                            error);
}

static knurl_status native(const knurl_request_snapshot *request,
                           knurl_source *source, knurl_reply *reply, void *data,
                           knurl_error *error)
{
    const knurl_auth_identity *identity = knurl_request_snapshot_auth(request);
    (void)source;
    (void)data;
    CHECK(identity != NULL && identity->method == KNURL_AUTH_SESSION);
    return knurl_reply_text(reply, 200, "text/plain", identity->subject, error);
}

static knurl_status upload_open(knurl_request *request, void *data,
                                void **state, knurl_error *error)
{
    struct fixture *f = data;
    const knurl_auth_identity *identity = knurl_request_auth(request);
    (void)state;
    (void)error;
    CHECK(identity != NULL && identity->method == KNURL_AUTH_MACHINE &&
          strcmp(identity->subject, "sleeping-service") == 0);
    CHECK(write(f->headers[1], "a", 1) == 1);
    return KNURL_OK;
}

static knurl_status upload_write(knurl_request *request, const void *bytes,
                                 size_t count, size_t *used, void *state,
                                 void *data, knurl_error *error)
{
    (void)request;
    (void)state;
    (void)data;
    (void)error;
    CHECK(count == 1 && memcmp(bytes, "a", 1) == 0);
    *used = count;
    return KNURL_OK;
}

static knurl_status upload_finish(knurl_request *r, knurl_reply *reply,
                                  void *state, void *data, knurl_error *error)
{
    (void)state;
    return text(r, reply, data, error);
}

static void cookie(const char *wire, char out[256])
{
    const char *line = wire, *end;
    while ((line = strstr(line, "\r\n")) != NULL) {
        line += 2;
        if (strncasecmp(line, "set-cookie: ", 12) == 0)
            break;
    }
    CHECK(line != NULL &&
          strstr(line, "Secure; HttpOnly; SameSite=Strict") != NULL);
    line += 12;
    end = strchr(line, ';');
    CHECK(end != NULL && end - line < 220);
    snprintf(out, 256, "Cookie: %.*s\r\n", (int)(end - line), line);
}

static void concurrency(struct fixture *f)
{
    char out[4096];
    struct connection c = connect_to(f, 1);
    send_request(
        &c, "POST /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
            "Authorization: Bearer fixture-hold\r\nContent-Length: 1\r\n\r\na");
    CHECK(note(f->entered[0], 3000) == 'e');
    CHECK(note(f->headers[0], 0) == 0);
    CHECK(exchange(f, 1, "GET", "/fast", NULL, NULL, out) == 200 &&
          strstr(out, "\r\n\r\nfast") != NULL);
    CHECK(note(f->headers[0], 0) == 0);
    CHECK(write(f->resume[1], "r", 1) == 1);
    CHECK(read_response(&c, out) == 200 &&
          strstr(out, "sleeping-service") != NULL);
    CHECK(note(f->headers[0], 1000) == 'a');
    CHECK(exchange(f, 1, "POST", "/slow", "Expect: 100-continue\r\n", "a",
                   out) == 401);
    CHECK(strstr(out, "100 Continue") == NULL && note(f->headers[0], 0) == 0);
}

static int bound_socket(unsigned short *port)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
    *port = ntohs(address.sin_port);
    return fd;
}

static void smtp_command(SSL *tls, const char *prefix, const char *reply)
{
    char line[512];
    size_t n = 0;
    while (n < sizeof(line) - 1) {
        CHECK(SSL_read(tls, line + n, 1) == 1);
        if (line[n++] == '\n')
            break;
    }
    line[n] = 0;
    CHECK(strncmp(line, prefix, strlen(prefix)) == 0);
    if (reply != NULL)
        CHECK(SSL_write(tls, reply, (int)strlen(reply)) == (int)strlen(reply));
}

static pid_t smtp_fixture(struct fixture *f)
{
    struct timeval timeout = {5, 0};
    int fd, listener = bound_socket(&f->smtp);
    SSL_CTX *server;
    SSL *tls;
    pid_t pid;
    CHECK(listen(listener, 1) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid != 0) {
        close(listener);
        return pid;
    }
    alarm(15);
    fd = accept(listener, NULL, NULL);
    close(listener);
    CHECK(fd >= 0 && setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                                sizeof(timeout)) == 0);
    server = SSL_CTX_new(TLS_server_method());
    CHECK(server != NULL &&
          SSL_CTX_use_certificate_file(server, f->cert, SSL_FILETYPE_PEM) ==
              1 &&
          SSL_CTX_use_PrivateKey_file(server, f->key, SSL_FILETYPE_PEM) == 1);
    tls = SSL_new(server);
    CHECK(tls != NULL && SSL_set_fd(tls, fd) == 1 && SSL_accept(tls) == 1);
    CHECK(SSL_write(tls, "220 localhost SMTP\r\n", 20) == 20);
    smtp_command(tls, "EHLO ", "250 localhost\r\n");
    smtp_command(tls, "MAIL FROM:<knurl@example.test>", "250 OK\r\n");
    smtp_command(tls, "RCPT TO:<alice@example.test>", "250 OK\r\n");
    smtp_command(tls, "DATA\r\n", "354 Continue\r\n");
    smtp_command(tls, "From: <knurl@example.test>\r\n", NULL);
    smtp_command(tls, "To: <alice@example.test>\r\n", NULL);
    smtp_command(tls, "Subject: Sign-in code\r\n", NULL);
    smtp_command(tls, "MIME-Version: 1.0\r\n", NULL);
    smtp_command(tls, "Content-Type: text/plain; charset=us-ascii\r\n", NULL);
    smtp_command(tls, "\r\n", NULL);
    smtp_command(tls, "Your sign-in code is ", NULL);
    smtp_command(tls, ".\r\n", "250 queued\r\n");
    smtp_command(tls, "QUIT\r\n", "221 bye\r\n");
    SSL_free(tls);
    SSL_CTX_free(server);
    close(fd);
    _exit(0);
}

static pid_t proxy_fixture(struct fixture *f)
{
    static const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: "
                                   "5\r\nConnection: close\r\n\r\nproxy";
    struct timeval timeout = {4, 0};
    char wire[4096], *line;
    size_t used;
    ssize_t length;
    int fd, i, forwarded, listener = bound_socket(&f->proxy);
    pid_t pid;
    CHECK(listen(listener, 2) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid != 0) {
        close(listener);
        return pid;
    }
    alarm(15);
    for (i = 0; i < 2; ++i) {
        fd = accept(listener, NULL, NULL);
        CHECK(fd >= 0 && setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                                    sizeof(timeout)) == 0);
        used = 0;
        do {
            CHECK(used < sizeof(wire) - 1);
            length = read(fd, wire + used, sizeof(wire) - used - 1);
            CHECK(length > 0);
            used += (size_t)length;
            wire[used] = 0;
        } while (strstr(wire, "\r\n\r\n") == NULL);
        CHECK(strstr(wire, "__Host-knurl-") == NULL);
        line = wire;
        forwarded = 0;
        while ((line = strstr(line, "\r\n")) != NULL) {
            line += 2;
            CHECK(strncasecmp(line, "authorization:", 14) != 0);
            if (strncasecmp(line, "cookie: theme=light", 19) == 0)
                forwarded = 1;
        }
        CHECK(i != 0 || forwarded);
        CHECK(write(fd, response, sizeof(response) - 1) ==
              (ssize_t)(sizeof(response) - 1));
        close(fd);
    }
    close(listener);
    _exit(0);
}

static EVP_PKEY *fixture_key(const struct fixture *f)
{
    FILE *file = fopen(f->key, "rb");
    EVP_PKEY *key;
    CHECK(file != NULL);
    key = PEM_read_PrivateKey(file, NULL, NULL, NULL);
    CHECK(fclose(file) == 0 && key != NULL);
    return key;
}

static void base64url(const unsigned char *data, size_t length, char *out)
{
    char *p;
    CHECK(EVP_EncodeBlock((unsigned char *)out, data, (int)length) > 0);
    for (p = out; *p != 0; ++p) {
        if (*p == '+')
            *p = '-';
        else if (*p == '/')
            *p = '_';
        else if (*p == '=')
            *p = 0;
    }
}

static void jwks(EVP_PKEY *key, const char *kid, char out[1024])
{
    BIGNUM *modulus = NULL, *exponent = NULL;
    unsigned char bytes[256];
    char n[345], e[16];
    CHECK(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &modulus) == 1 &&
          EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &exponent) == 1);
    CHECK(BN_bn2bin(modulus, bytes) == 256);
    base64url(bytes, 256, n);
    CHECK(BN_bn2bin(exponent, bytes) == 3);
    base64url(bytes, 3, e);
    snprintf(out, 1024,
             "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"%s\",\"alg\":"
             "\"RS256\",\"n\":\"%s\",\"e\":\"%s\"}]}",
             kid, n, e);
    BN_free(modulus);
    BN_free(exponent);
}

static void sign_bearer(EVP_PKEY *key, const char *kind, const char *kid,
                        const char *issuer, const char *audience,
                        const char *scope, const char *subject, long expires,
                        char out[2048])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char signature[256];
    size_t size = sizeof(signature);
    char json[512], head[345], body[689], signed_bytes[1040], proof[345];
    snprintf(json, sizeof(json),
             "{\"typ\":\"%s\",\"alg\":\"RS256\",\"kid\":\"%s\"}", kind, kid);
    base64url((const unsigned char *)json, strlen(json), head);
    snprintf(json, sizeof(json),
             "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"%s\","
             "\"scope\":\"%s\",\"exp\":%ld}",
             issuer, audience, subject, scope, expires);
    base64url((const unsigned char *)json, strlen(json), body);
    snprintf(signed_bytes, sizeof(signed_bytes), "%s.%s", head, body);
    CHECK(ctx != NULL &&
          EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, key) == 1 &&
          EVP_DigestSign(ctx, signature, &size,
                         (const unsigned char *)signed_bytes,
                         strlen(signed_bytes)) == 1 &&
          size == sizeof(signature));
    base64url(signature, size, proof);
    snprintf(out, 2048, "Authorization: Bearer %s.%s\r\n", signed_bytes, proof);
    EVP_MD_CTX_free(ctx);
}

static void bearer(struct fixture *f, const char *kind, const char *audience,
                   const char *scope, long expires, char out[2048])
{
    sign_bearer(f->signing_key, kind, "fixture", "https://issuer.example",
                audience, scope, "jwt-service", expires, out);
}

static void read_oauth_wire(SSL *tls, char wire[4096])
{
    const char *body, *header;
    size_t used = 0, length = 0;
    do {
        int n = SSL_read(tls, wire + used, (int)(4095 - used));
        CHECK(n > 0);
        used += (size_t)n;
        wire[used] = 0;
        body = strstr(wire, "\r\n\r\n");
        if (body != NULL) {
            for (header = wire; (header = strstr(header, "\r\n")) < body;) {
                header += 2;
                if (strncasecmp(header, "content-length: ", 16) == 0)
                    length = strtoul(header + 16, NULL, 10);
            }
        }
    } while (body == NULL || used < (size_t)(body + 4 - wire) + length);
}

static pid_t oauth_fixture(struct fixture *f)
{
    static const char *const requests[] = {
        "grant_type=client_credentials&scope=machine",
        "grant_type=refresh_token&refresh_token=first",
        "grant_type=refresh_token&refresh_token=second",
        "grant_type=client_credentials&scope=machine",
        "grant_type=client_credentials&scope=machine",
        "grant_type=client_credentials&scope=machine"};
    static const char *const results[] = {
        "{\"access_token\":\"initial\",\"token_type\":\"Bearer\","
        "\"expires_in\":2,\"refresh_token\":\"first\"}",
        "{\"access_token\":\"refreshed\",\"token_type\":\"Bearer\","
        "\"expires_in\":2,\"refresh_token\":\"second\"}",
        "{\"error\":\"invalid_grant\"}",
        "{\"access_token\":\"reacquired\",\"token_type\":\"Bearer\","
        "\"expires_in\":2}",
        "{\"access_token\":\"no-refresh\",\"token_type\":\"Bearer\","
        "\"expires_in\":30}",
        "{\"access_token\":\"after-revocation\",\"token_type\":\"Bearer\","
        "\"expires_in\":120}"};
    static const char *const forwarded[] = {
        "Bearer initial",          "Bearer initial",
        "Bearer refreshed",        "Bearer reacquired",
        "Bearer no-refresh",       "Bearer no-refresh",
        "Bearer after-revocation", "Bearer after-revocation"};
    struct timeval timeout = {4, 0};
    SSL_CTX *context;
    unsigned int calls = 0, grants = 0, keys = 0, discovered = 0, posts = 0;
    unsigned int short_posts = 0, retired_posts = 0, replacement_posts = 0;
    int listener = bound_socket(&f->oauth);
    pid_t pid;
    snprintf(f->issuer, sizeof(f->issuer), "https://localhost:%u",
             (unsigned int)f->oauth);
    CHECK(listen(listener, 8) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid != 0) {
        close(listener);
        return pid;
    }
    (void)signal(SIGPIPE, SIG_IGN);
    alarm(25);
    context = SSL_CTX_new(TLS_server_method());
    CHECK(context != NULL &&
          SSL_CTX_use_certificate_file(context, f->cert, SSL_FILETYPE_PEM) ==
              1 &&
          SSL_CTX_use_PrivateKey_file(context, f->key, SSL_FILETYPE_PEM) == 1);
    while (calls < 8 || grants < 6 || keys < 2 || discovered < 5 ||
           posts == 0 || short_posts == 0 || replacement_posts == 0) {
        char wire[4096], json[512], response[2048];
        const char *body = NULL, *line, *challenge = "";
        int length, code = 200, fd = accept(listener, NULL, NULL);
        SSL *tls = SSL_new(context);
        CHECK(fd >= 0 && tls != NULL &&
              setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout)) == 0 &&
              SSL_set_fd(tls, fd) == 1 && SSL_accept(tls) == 1);
        read_oauth_wire(tls, wire);
        if (strncmp(wire, "GET /.well-known/openid-configuration ", 38) == 0) {
            const char *endpoint;
            CHECK(++discovered <= 5);
            if (discovered == 1) {
                CHECK(write(f->token_request[1], "d", 1) == 1);
                CHECK(note(f->token_resume[0], 3000) == 'r');
            }
            endpoint = discovered <= 3   ? "token"
                       : discovered == 4 ? "retired-token"
                                         : "replacement-token";
            snprintf(json, sizeof(json),
                     "{\"issuer\":\"%s\",\"jwks_uri\":\"%s/jwks\","
                     "\"authorization_endpoint\":\"%s/authorize\","
                     "\"token_endpoint\":\"%s/%s\"}",
                     f->issuer, f->issuer, f->issuer, f->issuer, endpoint);
            body = json;
        } else if (strncmp(wire, "GET /jwks ", 10) == 0) {
            CHECK(keys < 2);
            body = keys++ == 0 ? f->jwks : f->next_jwks;
        } else if (strncmp(wire, "POST /token ", 12) == 0) {
            CHECK(grants < 6);
            CHECK(strstr(wire, "\r\nAuthorization: Basic "
                               "cHJveHklM0FjbGllbnQ6cyUyNmVjcmV0\r\n") != NULL);
            CHECK(strcmp(strstr(wire, "\r\n\r\n") + 4, requests[grants]) == 0);
            if (grants == 0) {
                CHECK(write(f->token_request[1], "t", 1) == 1);
                CHECK(note(f->token_resume[0], 3000) == 'r');
            }
            if (grants == 2)
                code = 400;
            body = results[grants++];
        } else if (strncmp(wire, "POST /short-token ", 18) == 0) {
            CHECK(++short_posts == 1 && sleep(2) == 0);
            body = "{\"access_token\":\"just-issued\","
                   "\"token_type\":\"Bearer\",\"expires_in\":2}";
        } else if (strncmp(wire, "POST /retired-token ", 20) == 0) {
            CHECK(++retired_posts <= 2);
            if (retired_posts == 1) {
                body = "{\"access_token\":\"before-move\","
                       "\"token_type\":\"Bearer\",\"expires_in\":2}";
            } else {
                code = 404;
                body = "{}";
            }
        } else if (strncmp(wire, "POST /replacement-token ", 24) == 0) {
            CHECK(++replacement_posts == 1 && retired_posts == 2);
            body = "{\"access_token\":\"after-move\","
                   "\"token_type\":\"Bearer\",\"expires_in\":60}";
        } else if (strncmp(wire, "POST /post-token ", 17) == 0) {
            CHECK(++posts == 1 && strstr(wire, "\r\nAuthorization:") == NULL);
            CHECK(strcmp(strstr(wire, "\r\n\r\n") + 4,
                         "grant_type=client_credentials&scope=machine%20read"
                         "&client_id=post%3Aclient&client_secret=s%26ecret") ==
                  0);
            body = "{\"access_token\":\"form-credentials\","
                   "\"token_type\":\"Bearer\",\"expires_in\":30}";
        } else if (strncmp(wire, "GET /oauth-proxy ", 17) == 0) {
            CHECK(calls < 8);
            for (line = wire; (line = strstr(line, "\r\n")) != NULL;) {
                line += 2;
                if (strncasecmp(line, "authorization: ", 15) == 0)
                    break;
            }
            CHECK(line != NULL &&
                  strncmp(line + 15, forwarded[calls],
                          strlen(forwarded[calls])) == 0 &&
                  strncmp(line + 15 + strlen(forwarded[calls]), "\r\n", 2) ==
                      0);
            CHECK(strstr(wire, "forged") == NULL);
            if (calls == 5) {
                code = 401;
                challenge =
                    "WWW-Authenticate: Basic realm=\"other\", Bearer "
                    "realm=\"native, upstream\", error=\"invalid_token\"\r\n";
            }
            body = forwarded[calls++];
        }
        CHECK(body != NULL);
        length =
            snprintf(response, sizeof(response),
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                     "%sContent-Length: %lu\r\nConnection: close\r\n\r\n%s",
                     code, code == 200 ? "OK" : "Rejected", challenge,
                     (unsigned long)strlen(body), body);
        CHECK(length > 0 && (size_t)length < sizeof(response) &&
              SSL_write(tls, response, length) == length);
        (void)SSL_shutdown(tls);
        SSL_free(tls);
        close(fd);
    }
    SSL_CTX_free(context);
    close(listener);
    _exit(0);
}

static void oauth_lifecycle(struct fixture *f)
{
    char auth[2048], out[4096];
    struct connection pending, second;
    long expiry = (long)time(NULL) + 60;
    sign_bearer(f->signing_key, "at+jwt", "fixture", f->issuer, "knurl",
                "machine", "jwt-service", expiry, auth);
    {
        char wire[2600];
        struct connection independent;
        snprintf(wire, sizeof(wire),
                 "GET /rotating-oidc HTTP/1.1\r\nHost: localhost\r\n"
                 "Connection: close\r\n%s\r\n",
                 auth);
        pending = connect_to(f, 1);
        send_request(&pending, wire);
        CHECK(note(f->token_request[0], 3000) == 'd');
        second = connect_to(f, 1);
        send_request(&second, wire);
        bearer(f, "at+jwt", "knurl", "machine", expiry, auth);
        snprintf(wire, sizeof(wire),
                 "GET /oidc HTTP/1.1\r\nHost: localhost\r\n"
                 "Connection: close\r\n%s\r\n",
                 auth);
        independent = connect_to(f, 1);
        send_request(&independent, wire);
        check_response_ready(&independent);
        CHECK(read_response(&independent, out) == 200);
        CHECK(write(f->token_resume[1], "r", 1) == 1);
        CHECK(read_response(&pending, out) == 200);
        CHECK(read_response(&second, out) == 200);
    }
    pending = connect_to(f, 1);
    send_request(&pending,
                 "GET /oauth-proxy HTTP/1.1\r\nHost: localhost\r\n"
                 "Authorization: Bearer forged\r\nConnection: close\r\n\r\n");
    CHECK(note(f->token_request[0], 3000) == 't');
    second = connect_to(f, 1);
    send_request(&second, "GET /oauth-proxy HTTP/1.1\r\nHost: localhost\r\n"
                          "Connection: close\r\n\r\n");
    CHECK(exchange(f, 1, "GET", "/fast", NULL, NULL, out) == 200);
    CHECK(exchange(f, 1, "POST", "/login", NULL,
                   "username=carol&password=a+test+password+with+spaces",
                   out) == 204);
    CHECK(write(f->token_resume[1], "r", 1) == 1);
    CHECK(read_response(&pending, out) == 200 &&
          strstr(out, "Bearer initial") != NULL);
    CHECK(read_response(&second, out) == 200 &&
          strstr(out, "Bearer initial") != NULL);
    sleep(2);
    CHECK(exchange(f, 1, "GET", "/oauth-proxy", NULL, NULL, out) == 200 &&
          strstr(out, "Bearer refreshed") != NULL);
    sleep(2);
    CHECK(exchange(f, 1, "GET", "/oauth-proxy", NULL, NULL, out) == 200 &&
          strstr(out, "Bearer reacquired") != NULL);
    sleep(2);
    CHECK(exchange(f, 1, "GET", "/oauth-proxy", NULL, NULL, out) == 200 &&
          strstr(out, "Bearer no-refresh") != NULL);
    CHECK(exchange(f, 1, "GET", "/oauth-proxy", NULL, NULL, out) == 401);
    CHECK(exchange(f, 1, "GET", "/oauth-proxy", NULL, NULL, out) == 200 &&
          strstr(out, "Bearer after-revocation") != NULL);
    sign_bearer(f->rotated_key, "at+jwt", "rotated", f->issuer, "knurl",
                "machine", "jwt-service", expiry, auth);
    CHECK(exchange(f, 1, "GET", "/rotating-oidc", auth, NULL, out) == 200);
    sign_bearer(f->signing_key, "at+jwt", "rotated", f->issuer, "knurl",
                "machine", "jwt-service", expiry, auth);
    CHECK(exchange(f, 1, "GET", "/rotating-oidc", auth, NULL, out) == 401);
    sign_bearer(f->signing_key, "at+jwt", "fixture", f->issuer, "knurl",
                "machine", "jwt-service", expiry, auth);
    CHECK(exchange(f, 1, "GET", "/rotating-oidc", auth, NULL, out) == 401);
}

static void oauth_client(struct fixture *f)
{
    knurl_oauth2_config config;
    knurl_oauth2_client *client;
    char endpoint[160], value[64];
    memset(&config, 0, sizeof(config));
    snprintf(endpoint, sizeof(endpoint), "%s/post-token", f->issuer);
    config.token_endpoint = endpoint;
    config.client_id = "post:client";
    config.client_secret = "s&ecret";
    config.client_secret_post = 1;
    config.ca_file = f->cert;
    config.scope = "machine read";
    CHECK(knurl_oauth2_client_new(&config, &client, NULL) == KNURL_OK);
    CHECK(knurl_oauth2_client_token(client, value, 3, NULL) == KNURL_LIMIT);
    CHECK(knurl_oauth2_client_token(client, value, sizeof(value), NULL) ==
          KNURL_OK);
    CHECK(strcmp(value, "form-credentials") == 0);
    knurl_oauth2_client_close(client);

    snprintf(endpoint, sizeof(endpoint), "%s/short-token", f->issuer);
    CHECK(knurl_oauth2_client_new(&config, &client, NULL) == KNURL_OK);
    CHECK(knurl_oauth2_client_token(client, value, sizeof(value), NULL) ==
              KNURL_OK &&
          strcmp(value, "just-issued") == 0);
    knurl_oauth2_client_close(client);

    config.token_endpoint = NULL;
    config.issuer = f->issuer;
    CHECK(knurl_oauth2_client_new(&config, &client, NULL) == KNURL_OK);
    CHECK(knurl_oauth2_client_token(client, value, sizeof(value), NULL) ==
              KNURL_OK &&
          strcmp(value, "before-move") == 0);
    CHECK(sleep(2) == 0);
    CHECK(knurl_oauth2_client_token(client, value, sizeof(value), NULL) ==
              KNURL_OK &&
          strcmp(value, "after-move") == 0);
    CHECK(knurl_oauth2_client_token(client, value, sizeof(value), NULL) ==
              KNURL_OK &&
          strcmp(value, "after-move") == 0);
    knurl_oauth2_client_close(client);
}

static void oauth_saturation(struct fixture *f)
{
    static const char hold[] =
        "POST /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
        "Authorization: Bearer fixture-hold\r\nContent-Length: 1\r\n\r\na";
    struct connection queued[40];
    char out[4096], release[sizeof(queued) / sizeof(queued[0])];
    size_t i;
    int status;
    for (i = 0; i < sizeof(queued) / sizeof(queued[0]); ++i) {
        queued[i] = connect_to(f, 1);
        send_request(&queued[i], hold);
    }
    CHECK(note(f->entered[0], 1000) == 'e');
    CHECK(exchange(f, 1, "POST", "/slow",
                   "Authorization: Bearer fixture-hold\r\n", "a", out) == 503);
    CHECK(exchange(f, 1, "GET", "/oauth-proxy", NULL, NULL, out) == 200 &&
          strstr(out, "Bearer after-revocation") != NULL);
    CHECK(exchange(f, 1, "GET", "/fast", NULL, NULL, out) == 200);
    memset(release, 'r', sizeof(release));
    CHECK(write(f->resume[1], release, sizeof(release)) ==
          (ssize_t)sizeof(release));
    for (i = 0; i < sizeof(queued) / sizeof(queued[0]); ++i) {
        status = read_response(&queued[i], out);
        CHECK(status == 200 || status == 503);
    }
}

static void oidc(struct fixture *f)
{
    char out[4096], auth[2048], subject[256];
    long now = (long)time(NULL);
    memset(subject, 's', sizeof(subject) - 1);
    memcpy(subject, "service:deployment/", 19);
    subject[sizeof(subject) - 1] = 0;
    sign_bearer(f->signing_key, "application/at+jwt", "fixture",
                "https://issuer.example", "knurl", "machine", subject, now + 60,
                auth);
    CHECK(exchange(f, 1, "GET", "/oidc", auth, NULL, out) == 200 &&
          strstr(out, subject) != NULL);
    bearer(f, "at+jwt", "knurl", "machine", now + 60, auth);
    CHECK(exchange(f, 1, "GET", "/oidc", auth, NULL, out) == 200 &&
          strstr(out, "jwt-service") != NULL);
    {
        char *signature = strrchr(auth, '.') + 1;
        signature[0] = signature[0] == 'A' ? 'B' : 'A';
    }
    CHECK(exchange(f, 1, "GET", "/oidc", auth, NULL, out) == 401);
    CHECK(strstr(out, "error=\"invalid_token\"") != NULL);
    bearer(f, "JWT", "knurl", "machine", now + 60, auth);
    CHECK(exchange(f, 1, "GET", "/oidc", auth, NULL, out) == 401);
    bearer(f, "at+jwt", "wrong", "machine", now + 60, auth);
    CHECK(exchange(f, 1, "GET", "/oidc", auth, NULL, out) == 401);
    bearer(f, "at+jwt", "knurl", "wrong", now + 60, auth);
    CHECK(exchange(f, 1, "GET", "/oidc", auth, NULL, out) == 401);
    bearer(f, "at+jwt", "knurl", "machine", now - 60, auth);
    CHECK(exchange(f, 1, "GET", "/oidc", auth, NULL, out) == 401);
}

static void totp_code_offset(const char *uri, char form[160], int offset)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    const char *secret = strstr(uri, "?secret="), *digit;
    unsigned char step[8], digest[20], key[20];
    unsigned int length, i, off, bits = 0, count = 0, used = 0;
    unsigned long value;
    time_t now = time(NULL) / 30 + offset;
    CHECK(secret != NULL && strncmp(uri, "otpauth://totp/Knurl:", 21) == 0);
    secret += 8;
    CHECK(strcspn(secret, "&") == 32);
    for (i = 0; i < 32; ++i) {
        digit = strchr(alphabet, secret[i]);
        CHECK(digit != NULL);
        bits = (bits << 5) | (unsigned int)(digit - alphabet);
        count += 5;
        if (count >= 8) {
            count -= 8;
            key[used++] = (unsigned char)(bits >> count);
        }
    }
    CHECK(used == sizeof(key));
    for (i = 8; i != 0; --i) {
        step[i - 1] = (unsigned char)now;
        now >>= 8;
    }
    CHECK(HMAC(EVP_sha1(), key, 20, step, sizeof(step), digest, &length) !=
          NULL);
    off = digest[19] & 15;
    value = ((unsigned long)(digest[off] & 127) << 24) |
            ((unsigned long)digest[off + 1] << 16) |
            ((unsigned long)digest[off + 2] << 8) | digest[off + 3];
    snprintf(form, 160, "%s&code=%06lu", password_form, value % 1000000UL);
}

static void totp_code(const char *uri, char form[160])
{
    totp_code_offset(uri, form, 0);
}

static void read_enrollment(const char *response, char uri[512])
{
    const char *start = strstr(response, "{\"uri\":\"");
    const char *end;
    CHECK(start != NULL);
    start += 8;
    end = strchr(start, '"');
    CHECK(end != NULL && end > start && end - start < 512 &&
          strstr(response, "no-store") != NULL);
    memcpy(uri, start, (size_t)(end - start));
    uri[end - start] = 0;
}

static void verify_totp_rollback(struct fixture *f, const char *form)
{
    char out[4096], path[1080], saved[1080];
    unsigned int i;
    int fd;
    long before, after;
    FILE *file;
    snprintf(path, sizeof(path), "%s/totp.json", f->store_path);
    snprintf(saved, sizeof(saved), "%s/totp.saved", f->store_path);
    CHECK(rename(path, saved) == 0);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && (file = fdopen(fd, "w")) != NULL);
    CHECK(fputs("{\"records\":[", file) >= 0);
    for (i = 0; i < 8191; ++i)
        CHECK(fprintf(file,
                      "%s{\"key\":\"t-%064u\",\"subject\":\"\",\"policy\":\"\","
                      "\"email\":\"\",\"secret\":\"\",\"seed\":\"\","
                      "\"revision\":\"\",\"enrollment\":\"\"}",
                      i == 0 ? "" : ",", i) > 0);
    CHECK(fputs("]}", file) >= 0 && (before = ftell(file)) > 0 &&
          fclose(file) == 0);
    CHECK(exchange(f, 1, "POST", "/otp", NULL, form, out) == 503);
    /* Capacity must fail at the session write. A stranded replay record
     * would fill the final row in the file. */
    file = fopen(path, "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    after = ftell(file);
    CHECK(before == after && fclose(file) == 0);
    CHECK(rename(saved, path) == 0);
}

static void totp_enrollment(struct fixture *f)
{
    static const char bob[] =
        "username=bob&password=a+test+password+with+spaces";
    char out[4096], browser[256], first[512], rotated[512];
    char code[160], form[160];
    CHECK(exchange(f, 1, "POST", "/otp", NULL, bob, out) == 428 &&
          strstr(out, "Set-Cookie:") == NULL);
    CHECK(exchange(f, 1, "POST", "/totp-enroll", NULL, "", out) == 401);
    CHECK(exchange(f, 1, "POST", "/login", NULL, bob, out) == 204);
    cookie(out, browser);
    CHECK(exchange(f, 1, "POST", "/totp-enroll", browser, "", out) == 202);
    CHECK(exchange(f, 1, "POST", "/totp-enroll", browser, "", out) == 202);
    read_enrollment(out, first);
    totp_code_offset(first, code, -1);
    snprintf(form, sizeof(form), "confirm=%s", strrchr(code, '=') + 1);
    CHECK(exchange(f, 1, "POST", "/totp-enroll", browser, form, out) == 204);
    CHECK(strstr(out, "Set-Cookie:") == NULL);
    CHECK(exchange(f, 1, "POST", "/totp-enroll", browser, "", out) == 401);
    CHECK(exchange(f, 1, "POST", "/login", NULL, bob, out) == 204);
    cookie(out, browser);
    totp_code(first, code);
    CHECK(exchange(f, 1, "POST", "/totp-enroll", browser,
                   strrchr(code, '&') + 1, out) == 202);
    read_enrollment(out, rotated);
    CHECK(strcmp(first, rotated) != 0);
    totp_code_offset(first, code, 1);
    snprintf(form, sizeof(form), "%s%s", bob, strrchr(code, '&'));
    CHECK(exchange(f, 1, "POST", "/otp", NULL, form, out) == 204);
    totp_code_offset(rotated, code, -1);
    snprintf(form, sizeof(form), "confirm=%s", strrchr(code, '=') + 1);
    CHECK(exchange(f, 1, "POST", "/totp-enroll", browser, form, out) == 204);
    CHECK(exchange(f, 1, "GET", "/app/me", browser, NULL, out) == 401);
    totp_code(rotated, code);
    snprintf(form, sizeof(form), "%s%s", bob, strrchr(code, '&'));
    verify_totp_rollback(f, form);
    CHECK(exchange(f, 1, "POST", "/otp", NULL, form, out) == 204);
    CHECK(knurl_auth_json_prune(f->store, NULL) == KNURL_OK);
    CHECK(exchange(f, 1, "POST", "/otp", NULL, form, out) == 401);
}

static void response_field(const char *wire, const char *field, char *out,
                           size_t capacity)
{
    char name[32];
    const char *start, *end;
    snprintf(name, sizeof(name), "\"%s\":\"", field);
    start = strstr(wire, name);
    CHECK(start != NULL);
    start += strlen(name);
    end = strchr(start, '"');
    CHECK(end != NULL && (size_t)(end - start) < capacity);
    memcpy(out, start, (size_t)(end - start));
    out[end - start] = 0;
}

static void issued_basic(const char *wire, char id[41], char header[320])
{
    char secret[65], pair[128], encoded[192];
    CHECK(strstr(wire, "Set-Cookie:") == NULL &&
          strstr(wire, "no-store") != NULL);
    response_field(wire, "client_id", id, 41);
    response_field(wire, "client_secret", secret, sizeof(secret));
    CHECK(strlen(id) == 40 && strlen(secret) == 64);
    snprintf(pair, sizeof(pair), "%s:%s", id, secret);
    CHECK(EVP_EncodeBlock((unsigned char *)encoded, (const unsigned char *)pair,
                          (int)strlen(pair)) > 0);
    snprintf(header, 320, "Authorization: Basic %s\r\n", encoded);
}

static void ordered_browser(struct fixture *f)
{
    static const char password[] =
        "username=carol&password=a+test+password+with+spaces";
    char out[4096], flow[65], form[128], code[160], session[256];
    CHECK(exchange(f, 1, "POST", "/ordered-browser", NULL,
                   "username=carol&password=test&code=000000", out) == 400);
    CHECK(exchange(f, 1, "POST", "/ordered-browser", NULL, password, out) ==
          202);
    CHECK(strstr(out, "\"step\":\"totp\"") != NULL &&
          strstr(out, "Set-Cookie:") == NULL);
    response_field(out, "workflow", flow, sizeof(flow));
    snprintf(form, sizeof(form), "workflow=%s&password=wrong", flow);
    CHECK(exchange(f, 1, "POST", "/ordered-browser", NULL, form, out) == 401);
    CHECK(exchange(f, 1, "GET", "/ordered-subject", NULL, NULL, out) == 401);
    totp_code(f->flow_enrollment[0], code);
    snprintf(form, sizeof(form), "workflow=%s%s", flow, strrchr(code, '&'));
    CHECK(exchange(f, 1, "POST", "/ordered-alternate", NULL, form, out) == 401);
    CHECK(exchange(f, 1, "POST", "/ordered-browser", NULL, form, out) == 204);
    cookie(out, session);
    CHECK(exchange(f, 1, "POST", "/ordered-browser", NULL, form, out) == 401);
    CHECK(exchange(f, 1, "GET", "/ordered-subject", session, NULL, out) ==
              200 &&
          strstr(out, "\r\n\r\ncarol") != NULL);
    snprintf(form, sizeof(form), "%s%s", password, strrchr(code, '&'));
    CHECK(exchange(f, 1, "POST", "/otp", NULL, form, out) == 401);
}

static void ordered_machine(struct fixture *f)
{
    char out[4096], flow[65], form[160], email_form[96], code[160];
    char machine_id[41], auth[320], email_code[7];
    char path[1200];
    struct stat active, pruned, revoked;
    size_t i;
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, "username=unknown",
                   out) == 202);
    CHECK(strstr(out, "\"step\":\"email_code\"") != NULL);
    response_field(out, "workflow", flow, sizeof(flow));
    snprintf(form, sizeof(form), "workflow=%s&password=wrong", flow);
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, form, out) == 401);
    snprintf(form, sizeof(form), "workflow=%s&code=000000", flow);
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, form, out) == 401);
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, "username=dave",
                   out) == 202);
    CHECK(strstr(out, "\"step\":\"email_code\"") != NULL);
    response_field(out, "workflow", flow, sizeof(flow));
    for (i = 0; i < 6; ++i)
        CHECK((email_code[i] = note(f->mail[0], 3000)) != 0);
    email_code[6] = 0;
    snprintf(form, sizeof(form), "workflow=%s&password=wrong", flow);
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, form, out) == 401);
    snprintf(form, sizeof(form), "workflow=%s&code=%c%s", flow,
             email_code[0] == '0' ? '1' : '0', email_code + 1);
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, form, out) == 401);
    snprintf(email_form, sizeof(email_form), "workflow=%s&code=%s", flow,
             email_code);
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, email_form, out) ==
          202);
    CHECK(strstr(out, "\"step\":\"password\"") != NULL);
    response_field(out, "workflow", flow, sizeof(flow));
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, email_form, out) ==
          401);
    snprintf(form, sizeof(form), "workflow=%s&password=wrong", flow);
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, form, out) == 401);
    snprintf(form, sizeof(form),
             "workflow=%s&password=a+test+password+with+spaces", flow);
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, form, out) == 202);
    CHECK(strstr(out, "\"step\":\"totp\"") != NULL);
    response_field(out, "workflow", flow, sizeof(flow));
    totp_code(f->flow_enrollment[1], code);
    snprintf(form, sizeof(form), "workflow=%s%s", flow, strrchr(code, '&'));
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, form, out) == 201);
    issued_basic(out, machine_id, auth);
    CHECK(exchange(f, 1, "POST", "/ordered-machine", NULL, form, out) == 401);
    CHECK(exchange(f, 1, "GET", "/ordered-api", auth, NULL, out) == 200 &&
          strstr(out, "\r\n\r\ndave") != NULL);
    CHECK(exchange(f, 1, "GET", "/machine", auth, NULL, out) == 401);
    CHECK(knurl_auth_json_prune(f->store, NULL) == KNURL_OK);
    CHECK(exchange(f, 1, "GET", "/ordered-api", auth, NULL, out) == 200);
    snprintf(path, sizeof(path), "%s/totp.json", f->store_path);
    CHECK(stat(path, &active) == 0);
    CHECK(knurl_auth_json_admin(f->store, KNURL_AUTH_USER_DISABLE, NULL, "dave",
                                NULL, NULL) == KNURL_OK);
    CHECK(exchange(f, 1, "GET", "/ordered-api", auth, NULL, out) == 401);
    CHECK(knurl_auth_json_prune(f->store, NULL) == KNURL_OK);
    CHECK(stat(path, &pruned) == 0 && pruned.st_size < active.st_size);
    CHECK(knurl_auth_json_revoke(f->store, "ordered-api", machine_id, NULL) ==
          KNURL_OK);
    CHECK(stat(path, &revoked) == 0 && revoked.st_size == pruned.st_size);
}

static void totp_race(struct fixture *f, unsigned short other_port)
{
    char form[160], out[4096];
    pid_t peer;
    int first, second;
    totp_code(f->enrollment, form);
    peer = fork();
    CHECK(peer >= 0);
    if (peer == 0) {
        secondary_runner = runner_pid = 0;
        f->tls = other_port;
        first = exchange(f, 1, "POST", "/otp", NULL, form, out);
        _exit(first == 204 ? 0 : first == 401 ? 1 : 2);
    }
    first = exchange(f, 1, "POST", "/otp", NULL, form, out);
    CHECK(waitpid(peer, &second, 0) == peer && WIFEXITED(second));
    CHECK((first == 204 && WEXITSTATUS(second) == 1) ||
          (first == 401 && WEXITSTATUS(second) == 0));
    CHECK(exchange(f, 1, "POST", "/otp", NULL, form, out) == 401);
}

static void enroll_concurrently(struct fixture *f)
{
    char uri[KNURL_AUTH_TOTP_URI_CAPACITY];
    int result[2], child_status;
    knurl_status status;
    ssize_t length;
    pid_t child;
    CHECK(pipe(result) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(result[0]);
        status =
            knurl_auth_json_enroll_totp(f->store, NULL, "alice", 0, uri, NULL);
        if (status == KNURL_OK)
            CHECK(write(result[1], uri, strlen(uri) + 1) ==
                  (ssize_t)(strlen(uri) + 1));
        _exit(status == KNURL_OK ? 0 : status == KNURL_CONFLICT ? 1 : 2);
    }
    close(result[1]);
    status = knurl_auth_json_enroll_totp(f->store, NULL, "alice", 0,
                                         f->enrollment, NULL);
    CHECK(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status));
    CHECK((status == KNURL_OK && WEXITSTATUS(child_status) == 1) ||
          (status == KNURL_CONFLICT && WEXITSTATUS(child_status) == 0));
    if (status == KNURL_CONFLICT) {
        length = read(result[0], f->enrollment, sizeof(f->enrollment));
        CHECK(length > 0 && f->enrollment[length - 1] == 0);
    }
    close(result[0]);
}

static void verify_state_files(const char *path)
{
    static const char *const names[] = {
        "key",           "lock",       "accounts.json", "attempts.json",
        "sessions.json", "email.json", "totp.json"};
    DIR *dir = opendir(path);
    struct dirent *entry;
    size_t count = 0, i;
    CHECK(dir != NULL);
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (strcmp(entry->d_name, names[i]) == 0)
                break;
        CHECK(i < sizeof(names) / sizeof(names[0]));
        ++count;
    }
    CHECK(count == sizeof(names) / sizeof(names[0]));
    closedir(dir);
}

static void large_store(const char *work)
{
    char store[1024], accounts[1040], secret[65];
    knurl_auth_json *json;
    FILE *file;
    unsigned int i;
    snprintf(store, sizeof(store), "%s/large-store", work);
    snprintf(accounts, sizeof(accounts), "%s/accounts.json", store);
    CHECK(knurl_auth_json_init(store, NULL) == KNURL_OK);
    file = fopen(accounts, "wb");
    CHECK(file != NULL && fputs("{\"records\":[", file) >= 0);
    /* Enough mapped records to exceed LoneJSON's general-purpose parse budget.
     */
    for (i = 0; i < 1500; ++i)
        CHECK(fprintf(file,
                      "%s{\"key\":\"c-%064u\",\"subject\":\"seeded\","
                      "\"policy\":\"seeded\",\"email\":\"\",\"secret\":\"\","
                      "\"seed\":\"\",\"revision\":\"\",\"enrollment\":\"\"}",
                      i == 0 ? "" : ",", i) > 0);
    CHECK(fputs("]}", file) >= 0 && fclose(file) == 0);
    CHECK(knurl_auth_json_open(store, &json, NULL) == KNURL_OK);
    CHECK(knurl_auth_json_issue(json, "machine", "test-client", secret, NULL) ==
          KNURL_OK);
    CHECK(knurl_auth_json_revoke(json, "machine", "test-client", NULL) ==
          KNURL_OK);
    knurl_auth_json_close(json);
}

static knurl_status session_check(const knurl_auth_identity *identity,
                                  void *userdata, knurl_error *error)
{
    struct fixture *f = userdata;
    FILE *file;
    int status;
    (void)error;
    CHECK((identity->method == KNURL_AUTH_SESSION ||
           identity->method == KNURL_AUTH_MACHINE) &&
          strcmp(identity->users_id, "staff") == 0 &&
          strcmp(identity->subject, "directory-user") == 0);
    file = fopen(f->directory_status, "rb");
    CHECK(file != NULL);
    status = fgetc(file);
    fclose(file);
    return status == 'a' ? KNURL_OK : status == 'd' ? KNURL_EOF : KNURL_IO;
}

static knurl_status partner_check(const knurl_auth_identity *identity,
                                  void *userdata, knurl_error *error)
{
    (void)userdata;
    (void)error;
    CHECK(strcmp(identity->users_id, "partner") == 0 &&
          strcmp(identity->subject, "directory-user") == 0);
    return KNURL_OK;
}

static void directory_status(struct fixture *f, int status)
{
    FILE *file = fopen(f->directory_status, "wb");
    CHECK(file != NULL && fputc(status, file) == status && fclose(file) == 0);
}

static void directory_totp(struct fixture *f, char session[256])
{
    static const char credentials[] =
        "username=remote&password=a+test+password+with+spaces";
    char out[4096], other[256], uri[512], code[160], form[256], email_code[7];
    char partner[256], partner_uri[KNURL_AUTH_TOTP_URI_CAPACITY];
    size_t i;
    CHECK(exchange(f, 1, "POST", "/external", NULL, credentials, out) == 204);
    cookie(out, session);
    CHECK(exchange(f, 1, "POST", "/external-email-code", NULL,
                   "username=remote", out) == 202);
    for (i = 0; i < 6; ++i)
        CHECK((email_code[i] = note(f->mail[0], 3000)) != 0);
    email_code[6] = 0;
    snprintf(form, sizeof(form), "username=remote&code=%s", email_code);
    CHECK(exchange(f, 1, "POST", "/external-email", NULL, form, out) == 204);
    cookie(out, other);
    CHECK(exchange(f, 1, "GET", "/external", session, NULL, out) == 200 &&
          strstr(out, "directory-user") != NULL);
    CHECK(exchange(f, 1, "GET", "/external-email", other, NULL, out) == 200);
    CHECK(exchange(f, 1, "POST", "/external-totp", session, "", out) == 202);
    read_enrollment(out, uri);
    CHECK(strstr(uri, "Knurl:staff%2Fdirectory-user") != NULL);
    CHECK(exchange(f, 1, "POST", "/partner", NULL, partner_credentials, out) ==
          204);
    cookie(out, partner);
    CHECK(exchange(f, 1, "POST", "/partner-totp", partner, "", out) == 202);
    read_enrollment(out, partner_uri);
    CHECK(strstr(partner_uri, "Knurl:partner%2Fdirectory-user") != NULL);
    totp_code_offset(uri, code, -1);
    snprintf(form, sizeof(form), "confirm=%s", strrchr(code, '=') + 1);
    CHECK(exchange(f, 1, "POST", "/external-totp", session, form, out) == 204);
    CHECK(exchange(f, 1, "GET", "/external", session, NULL, out) == 401);
    CHECK(exchange(f, 1, "GET", "/external-email", other, NULL, out) == 401);
    CHECK(exchange(f, 1, "GET", "/partner", partner, NULL, out) == 200);
    totp_code_offset(partner_uri, code, -1);
    snprintf(form, sizeof(form), "confirm=%s", strrchr(code, '=') + 1);
    CHECK(exchange(f, 1, "POST", "/partner-totp", partner, form, out) == 204);
    CHECK(exchange(f, 1, "GET", "/partner", partner, NULL, out) == 401);
    totp_code(partner_uri, code);
    snprintf(form, sizeof(form), "%s%s", partner_credentials,
             strrchr(code, '&'));
    CHECK(exchange(f, 1, "POST", "/partner-otp", NULL, form, out) == 204);
    cookie(out, partner);

    CHECK(exchange(f, 1, "POST", "/external", NULL, credentials, out) == 204);
    cookie(out, session);
    CHECK(exchange(f, 1, "GET", "/external", session, NULL, out) == 200);
    totp_code(uri, code);
    CHECK(exchange(f, 1, "POST", "/external-totp", session,
                   strrchr(code, '&') + 1, out) == 202);
    read_enrollment(out, uri);
    totp_code_offset(uri, code, -1);
    snprintf(form, sizeof(form), "confirm=%s", strrchr(code, '=') + 1);
    CHECK(exchange(f, 1, "POST", "/external-totp", session, form, out) == 204);
    CHECK(exchange(f, 1, "GET", "/external", session, NULL, out) == 401);
    CHECK(exchange(f, 1, "GET", "/partner-otp", partner, NULL, out) == 200);
    totp_code(uri, code);
    snprintf(form, sizeof(form), "%s%s", credentials, strrchr(code, '&'));
    CHECK(exchange(f, 1, "POST", "/external-otp", NULL, form, out) == 204);
    CHECK(exchange(f, 1, "POST", "/external", NULL, credentials, out) == 204);
    cookie(out, session);
}

static void start_mail(struct fixture *f, const char *path, const char *form,
                       char out[4096])
{
    int attempt;
    for (attempt = 0; attempt < 6; ++attempt) {
        CHECK(exchange(f, 1, "POST", path, NULL, form, out) == 202);
        if (note(f->entered[0], 200) == 'm')
            return;
    }
    CHECK(0);
}

/* The isolated mail fixture has no competing requests here. Backdate the
 * published challenge's resend timestamp while leaving the code unexpired. */
static void age_resend_window(struct fixture *f)
{
    static const struct timespec wait = {0, 10000000L};
    char path[1200], json[4096], *updated, *digits;
    size_t used;
    int attempt;
    FILE *file;
    snprintf(path, sizeof(path), "%s/email.json", f->mail_store_path);
    for (attempt = 0; attempt < 100; ++attempt) {
        file = fopen(path, "rb");
        CHECK(file != NULL);
        used = fread(json, 1, sizeof(json) - 1, file);
        CHECK(feof(file) && fclose(file) == 0);
        json[used] = 0;
        if (strstr(json, "\"e-") != NULL && strstr(json, "\"l-") == NULL)
            break;
        (void)nanosleep(&wait, NULL);
    }
    CHECK(attempt < 100);
    updated = strstr(json, "\"updated\":");
    CHECK(updated != NULL);
    digits = updated + strlen("\"updated\":");
    digits += strspn(digits, " \t");
    used = strspn(digits, "0123456789");
    CHECK(used > 0);
    *digits++ = '0';
    memset(digits, ' ', used - 1);
    file = fopen(path, "wb");
    CHECK(file != NULL && fputs(json, file) >= 0 && fclose(file) == 0);
}

static void mail_delivery(struct fixture *f)
{
    char out[4096], code[7], form[160], flow[65], previous[65];
    size_t i;
    struct connection login;
    start_mail(f, "/mail-start", "username=alice", out);
    start_mail(f, "/mail-start", "username=dave", out);
    login = connect_to(f, 1);
    snprintf(form, sizeof(form), "%lu\r\n\r\n%s",
             (unsigned long)strlen(password_form), password_form);
    {
        char wire[512];
        snprintf(wire, sizeof(wire),
                 "POST /mail-password HTTP/1.1\r\nHost: localhost\r\n"
                 "Connection: close\r\nContent-Type: "
                 "application/x-www-form-urlencoded\r\n"
                 "Content-Length: %s",
                 form);
        send_request(&login, wire);
    }
    check_response_ready(&login);
    CHECK(read_response(&login, out) == 204);
    CHECK(write(f->resume[1], "ff", 2) == 2);

    start_mail(f, "/mail-start", "username=alice", out);
    CHECK(write(f->resume[1], "r", 1) == 1);
    for (i = 0; i < 6; ++i)
        CHECK((code[i] = note(f->mail[0], 3000)) != 0);
    code[6] = 0;
    age_resend_window(f);
    start_mail(f, "/mail-start", "username=alice", out);
    CHECK(write(f->resume[1], "f", 1) == 1);
    snprintf(form, sizeof(form), "username=alice&code=%s", code);
    CHECK(exchange(f, 1, "POST", "/mail-login", NULL, form, out) == 204);
    CHECK(exchange(f, 1, "POST", "/mail-login", NULL, form, out) == 401);
    start_mail(f, "/mail-start", "username=alice", out);
    CHECK(write(f->resume[1], "f", 1) == 1);

    start_mail(f, "/mail-workflow", "username=dave", out);
    response_field(out, "workflow", previous, sizeof(previous));
    CHECK(write(f->resume[1], "f", 1) == 1);
    start_mail(f, "/mail-workflow", "username=dave", out);
    response_field(out, "workflow", flow, sizeof(flow));
    CHECK(strcmp(flow, previous) != 0);
    CHECK(write(f->resume[1], "r", 1) == 1);
    for (i = 0; i < 6; ++i)
        CHECK((code[i] = note(f->mail[0], 3000)) != 0);
    snprintf(form, sizeof(form), "workflow=%s&code=%s", previous, code);
    CHECK(exchange(f, 1, "POST", "/mail-workflow", NULL, form, out) == 401);
    snprintf(form, sizeof(form), "workflow=%s&code=%s", flow, code);
    CHECK(exchange(f, 1, "POST", "/mail-workflow", NULL, form, out) == 202);
    response_field(out, "workflow", flow, sizeof(flow));
    snprintf(form, sizeof(form),
             "workflow=%s&password=a+test+password+with+spaces", flow);
    CHECK(exchange(f, 1, "POST", "/mail-workflow", NULL, form, out) == 204);
}

static void exercise(struct fixture *f)
{
    char out[4096], session[256], form[160], code[7], encoded[256], auth[320];
    size_t i;
    CHECK(exchange(f, 0, "POST", "/login", NULL, password_form, out) == 403);
    CHECK(exchange(f, 1, "POST", "/login", "Origin: https://other.test\r\n",
                   password_form, out) == 403);
    CHECK(exchange(f, 1, "GET", "/app/me", NULL, NULL, out) == 401);
    CHECK(exchange(f, 1, "GET", "/static", NULL, NULL, out) == 401);
    CHECK(exchange(f, 1, "GET", "/proxy", NULL, NULL, out) == 401);
    CHECK(exchange(f, 1, "POST", "/login", NULL, "username=alice&password=bad",
                   out) == 401);
    CHECK(exchange(f, 1, "POST", "/login", NULL, password_form, out) == 204);
    cookie(out, session);
    CHECK(exchange(f, 1, "GET", "/app/me", session, NULL, out) == 200 &&
          strstr(out, "\r\n\r\nalice") != NULL);
    CHECK(exchange(f, 1, "GET", "/static", session, NULL, out) == 200 &&
          strstr(out, "protected fixture") != NULL &&
          strstr(out, "private, no-store") != NULL);
    snprintf(auth, sizeof(auth), "%.*s; theme=light\r\n",
             (int)strlen(session) - 2, session);
    CHECK(exchange(f, 1, "GET", "/proxy", auth, NULL, out) == 200);
    CHECK(strstr(out, "\r\nproxy") != NULL);
    CHECK(exchange(f, 1, "GET", "/different-policy", session, NULL, out) ==
          401);
    CHECK(exchange(f, 1, "POST", "/logout", session, NULL, out) == 204 &&
          strstr(out, "Max-Age=0") != NULL);
    CHECK(exchange(f, 1, "GET", "/app/me", session, NULL, out) == 401);
    directory_totp(f, session);
    directory_status(f, 'd');
    CHECK(exchange(f, 1, "GET", "/external", session, NULL, out) == 401);
    directory_status(f, 'e');
    CHECK(exchange(f, 1, "GET", "/external", session, NULL, out) == 503);
    {
        char client_id[41], basic[320], partner[320];
        directory_status(f, 'a');
        CHECK(exchange(f, 1, "POST", "/external-provision", NULL,
                       "username=remote&password=a+test+password+with+spaces",
                       out) == 201);
        issued_basic(out, client_id, basic);
        CHECK(exchange(f, 1, "GET", "/external-machine", basic, NULL, out) ==
              200);
        CHECK(exchange(f, 1, "POST", "/partner-provision", NULL,
                       partner_credentials, out) == 201);
        issued_basic(out, client_id, partner);
        CHECK(knurl_auth_json_prune(f->users_store, NULL) == KNURL_OK);
        CHECK(exchange(f, 1, "GET", "/external-machine", basic, NULL, out) ==
              200);
        CHECK(exchange(f, 1, "GET", "/partner-machine", partner, NULL, out) ==
              200);
        CHECK(exchange(f, 1, "GET", "/partner-machine", basic, NULL, out) ==
              401);
        CHECK(exchange(f, 1, "GET", "/external-machine", partner, NULL, out) ==
              401);
        directory_status(f, 'd');
        CHECK(exchange(f, 1, "GET", "/external-machine", basic, NULL, out) ==
              401);
        directory_status(f, 'e');
        CHECK(exchange(f, 1, "GET", "/external-machine", basic, NULL, out) ==
              503);
        directory_status(f, 'a');
        CHECK(knurl_auth_json_admin(f->users_store, KNURL_AUTH_USER_DISABLE,
                                    "staff", "directory-user", NULL,
                                    NULL) == KNURL_OK);
        CHECK(knurl_auth_json_prune(f->users_store, NULL) == KNURL_OK);
        CHECK(exchange(f, 1, "GET", "/external", session, NULL, out) == 401);
        CHECK(exchange(f, 1, "GET", "/external-machine", basic, NULL, out) ==
              401);
        CHECK(exchange(f, 1, "GET", "/partner-machine", partner, NULL, out) ==
              200);
    }
    totp_code(f->enrollment, form);
    CHECK(exchange(f, 1, "POST", "/otp", NULL, form, out) == 204);
    totp_enrollment(f);
    CHECK(exchange(f, 1, "POST", "/otp", NULL, form, out) == 401);
    CHECK(knurl_auth_json_enroll_totp(f->store, NULL, "alice", 1, f->enrollment,
                                      NULL) == KNURL_OK);
    totp_code(f->enrollment, form);
    CHECK(exchange(f, 1, "POST", "/otp", NULL, form, out) == 204);
    CHECK(exchange(f, 1, "POST", "/email-code", NULL, "username=unknown",
                   out) == 202);
    CHECK(exchange(f, 1, "POST", "/email-code", NULL, "username=alice", out) ==
          202);
    CHECK(exchange(f, 1, "POST", "/smtp-code", NULL, "username=alice", out) ==
          202);
    for (i = 0; i < 6; ++i)
        CHECK((code[i] = note(f->mail[0], 3000)) != 0);
    code[6] = 0;
    snprintf(form, sizeof(form), "username=alice&code=%s", code);
    CHECK(exchange(f, 1, "POST", "/email", NULL, form, out) == 204);
    cookie(out, session);
    CHECK(exchange(f, 1, "POST", "/email", NULL, form, out) == 401);
    CHECK(exchange(f, 1, "GET", "/different-policy", session, NULL, out) ==
          200);
    CHECK(exchange(f, 1, "POST", "/totp-enroll-email", session, "", out) ==
          409);
    CHECK(knurl_auth_json_admin(f->store, KNURL_AUTH_USER_DISABLE, NULL,
                                "alice", NULL, NULL) == KNURL_OK);
    CHECK(exchange(f, 1, "GET", "/different-policy", session, NULL, out) ==
          401);
    snprintf(form, sizeof(form), "pipeline:%s", f->client_secret);
    CHECK(EVP_EncodeBlock((unsigned char *)encoded, (const unsigned char *)form,
                          (int)strlen(form)) > 0);
    snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n", encoded);
    CHECK(exchange(f, 1, "GET", "/machine", auth, NULL, out) == 200 &&
          strstr(out, "pipeline") != NULL);
    CHECK(exchange(f, 1, "GET", "/machine-other", auth, NULL, out) == 401);
    CHECK(exchange(f, 1, "GET", "/machine-proxy", auth, NULL, out) == 200);
    CHECK(strstr(out, "\r\nproxy") != NULL);
    CHECK(knurl_auth_json_revoke(f->store, "machine-other", "pipeline", NULL) ==
          KNURL_OK);
    CHECK(exchange(f, 1, "GET", "/machine", auth, NULL, out) == 200);
    CHECK(knurl_auth_json_revoke(f->store, "machine", "pipeline", NULL) ==
          KNURL_OK);
    CHECK(exchange(f, 1, "GET", "/machine", auth, NULL, out) == 401);
    oidc(f);
    oauth_lifecycle(f);
    oauth_client(f);
    ordered_browser(f);
    ordered_machine(f);
    concurrency(f);
    for (i = 0; i < 12; ++i)
        CHECK(exchange(f, 1, "POST", "/login", NULL,
                       "username=limited&password=bad", out) == 401);
    CHECK(exchange(f, 1, "POST", "/login", NULL,
                   "username=limited&password=bad", out) == 429);
    CHECK(knurl_auth_json_prune(f->store, NULL) == KNURL_OK);
    mail_delivery(f);
    oauth_saturation(f);
}

static knurl_status ready(knurl_server *s, const knurl_process_context *context,
                          void *data, knurl_error *error)
{
    struct fixture *f = data;
    knurl_bound_listener info;
    pid_t parent = getpid();
    (void)context;
    CHECK(knurl_server_listener_at(s, 0, &info, error) == KNURL_OK);
    f->plain = info.port;
    CHECK(knurl_server_listener_at(s, 1, &info, error) == KNURL_OK);
    f->tls = info.port;
    CHECK(knurl_server_set_serving(s, 1, error) == KNURL_OK);
    f->client = fork();
    CHECK(f->client >= 0);
    if (f->client == 0) {
        (void)signal(SIGPIPE, SIG_IGN);
        f->tls_client = SSL_CTX_new(TLS_client_method());
        CHECK(f->tls_client != NULL &&
              SSL_CTX_load_verify_locations(f->tls_client, f->cert, NULL) == 1);
        SSL_CTX_set_verify(f->tls_client, SSL_VERIFY_PEER, NULL);
        exercise(f);
        SSL_CTX_free(f->tls_client);
        CHECK(kill(parent, SIGTERM) == 0);
        _exit(0);
    }
    return KNURL_OK;
}

static void plain_route(knurl_router *router, const char *path,
                        knurl_auth *auth)
{
    knurl_route_config route;
    knurl_route *handle;
    knurl_route_config_init(&route);
    route.path = path;
    route.method = KNURL_HTTP_GET;
    route.handler = text;
    CHECK(knurl_router_route(router, &route, &handle, NULL) == KNURL_OK);
    CHECK(knurl_route_set_auth(handle, auth, NULL) == KNURL_OK);
}

static void routes(knurl_router *router, struct fixture *f)
{
    knurl_auth_config config;
    knurl_auth *browser, *otp, *email, *smtp, *external, *machine, *other,
        *slow, *issuer, *rotating, *ordered, *ordered_api, *external_machine,
        *external_email;
    knurl_auth *mail_probe;
    knurl_route_group_config group_config;
    knurl_route_group *group;
    knurl_blocking_route_config app;
    knurl_static_config files;
    knurl_upload_stream_config upload;
    knurl_proxy_config proxy;
    knurl_oauth2_config oauth;
    knurl_oauth2_client *client;
    knurl_route *handle;
    char mail_url[128], proxy_url[128];
    knurl_auth_config_init(&config);
    config.json = f->mail_store;
    config.name = "mail-password";
    CHECK(knurl_auth_new(&config, &mail_probe, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/mail-password", mail_probe,
                           KNURL_AUTH_LOGIN, &handle, NULL) == KNURL_OK);
    knurl_auth_close(mail_probe);
    config.name = "mail-delivery";
    config.login = KNURL_AUTH_LOGIN_EMAIL;
    config.send_email = gated_mail;
    config.email_userdata = f;
    CHECK(knurl_auth_new(&config, &mail_probe, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/mail-start", mail_probe,
                           KNURL_AUTH_EMAIL_START, &handle, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/mail-login", mail_probe, KNURL_AUTH_LOGIN,
                           &handle, NULL) == KNURL_OK);
    knurl_auth_close(mail_probe);
    config.name = "mail-workflow";
    config.login = KNURL_AUTH_LOGIN_PASSWORD;
    config.factors = KNURL_AUTH_FACTOR_EMAIL | KNURL_AUTH_FACTOR_PASSWORD;
    CHECK(knurl_auth_new(&config, &mail_probe, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/mail-workflow", mail_probe,
                           KNURL_AUTH_WORKFLOW, &handle, NULL) == KNURL_OK);
    knurl_auth_close(mail_probe);
    knurl_auth_config_init(&config);
    config.name = "browser";
    config.json = f->store;
    CHECK(knurl_auth_new(&config, &browser, NULL) == KNURL_OK);
    config.name = "totp";
    config.login = KNURL_AUTH_LOGIN_TOTP;
    CHECK(knurl_auth_new(&config, &otp, NULL) == KNURL_OK);
    config.name = "email";
    config.login = KNURL_AUTH_LOGIN_EMAIL;
    config.send_email = send_code;
    config.email_userdata = f;
    CHECK(knurl_auth_new(&config, &email, NULL) == KNURL_OK);
    config.name = "smtp";
    config.send_email = NULL;
    snprintf(mail_url, sizeof(mail_url), "smtps://localhost:%u",
             (unsigned int)f->smtp);
    config.smtp.url = mail_url;
    config.smtp.from = "knurl@example.test";
    config.smtp.ca_file = f->cert;
    CHECK(knurl_auth_new(&config, &smtp, NULL) == KNURL_OK);
    config.name = "machine";
    config.method = KNURL_AUTH_MACHINE;
    CHECK(knurl_auth_new(&config, &machine, NULL) == KNURL_OK);
    config.name = "machine-other";
    CHECK(knurl_auth_new(&config, &other, NULL) == KNURL_OK);
    knurl_auth_config_init(&config);
    config.name = "slow";
    config.method = KNURL_AUTH_MACHINE;
    config.backend = provider;
    config.userdata = f;
    CHECK(knurl_auth_new(&config, &slow, NULL) == KNURL_OK);
    knurl_auth_config_init(&config);
    config.name = "oidc";
    config.method = KNURL_AUTH_MACHINE;
    config.oidc.issuer = "https://issuer.example";
    config.oidc.audience = "knurl";
    config.oidc.jwks_uri = "https://issuer.example/jwks";
    config.oidc.jwks_json = f->jwks;
    config.oidc.pin_jwks = 1;
    config.oidc.scope = "machine";
    CHECK(knurl_auth_new(&config, &issuer, NULL) == KNURL_OK);
    config.name = "rotating";
    config.oidc.issuer = f->issuer;
    config.oidc.jwks_uri = NULL;
    config.oidc.jwks_json = NULL;
    config.oidc.pin_jwks = 0;
    config.oidc.ca_file = f->cert;
    CHECK(knurl_auth_new(&config, &rotating, NULL) == KNURL_OK);
    knurl_auth_config_init(&config);
    config.name = "external";
    config.json = f->users_store;
    config.users = provider;
    config.users_userdata = f;
    config.session_check = session_check;
    config.session_check_userdata = f;
    CHECK(knurl_auth_new(&config, &external, NULL) == KNURL_INVALID);
    config.users_id = "staff";
    CHECK(knurl_auth_new(&config, &external, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/external-totp", external,
                           KNURL_AUTH_TOTP_ENROLL_ENDPOINT, &handle,
                           NULL) == KNURL_OK);
    {
        knurl_auth_config partner = config;
        knurl_auth *policy;
        config.name = "external-otp";
        config.login = KNURL_AUTH_LOGIN_TOTP;
        CHECK(knurl_auth_new(&config, &policy, NULL) == KNURL_OK);
        CHECK(knurl_auth_route(router, "/external-otp", policy,
                               KNURL_AUTH_LOGIN, &handle, NULL) == KNURL_OK);
        knurl_auth_close(policy);
        partner.name = "partner";
        partner.users = partner_users;
        partner.users_id = "partner";
        partner.session_check = partner_check;
        CHECK(knurl_auth_new(&partner, &policy, NULL) == KNURL_OK);
        CHECK(knurl_auth_route(router, "/partner", policy, KNURL_AUTH_LOGIN,
                               &handle, NULL) == KNURL_OK);
        CHECK(knurl_auth_route(router, "/partner-totp", policy,
                               KNURL_AUTH_TOTP_ENROLL_ENDPOINT, &handle,
                               NULL) == KNURL_OK);
        plain_route(router, "/partner", policy);
        knurl_auth_close(policy);
        partner.name = "partner-otp";
        partner.login = KNURL_AUTH_LOGIN_TOTP;
        CHECK(knurl_auth_new(&partner, &policy, NULL) == KNURL_OK);
        CHECK(knurl_auth_route(router, "/partner-otp", policy, KNURL_AUTH_LOGIN,
                               &handle, NULL) == KNURL_OK);
        plain_route(router, "/partner-otp", policy);
        knurl_auth_close(policy);
        partner.name = "external-machine";
        partner.login = KNURL_AUTH_LOGIN_PASSWORD;
        partner.method = KNURL_AUTH_MACHINE;
        partner.factors = KNURL_AUTH_FACTOR_PASSWORD;
        partner.session_check = NULL;
        partner.machine_check = partner_check;
        CHECK(knurl_auth_new(&partner, &policy, NULL) == KNURL_OK);
        CHECK(knurl_auth_route(router, "/partner-provision", policy,
                               KNURL_AUTH_WORKFLOW, &handle, NULL) == KNURL_OK);
        plain_route(router, "/partner-machine", policy);
        knurl_auth_close(policy);
    }
    config.name = "external-email";
    config.login = KNURL_AUTH_LOGIN_EMAIL;
    config.send_email = send_code;
    config.email_userdata = f;
    CHECK(knurl_auth_new(&config, &external_email, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/external-email", external_email,
                           KNURL_AUTH_LOGIN, &handle, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/external-email-code", external_email,
                           KNURL_AUTH_EMAIL_START, &handle, NULL) == KNURL_OK);
    config.name = "external-machine";
    config.method = KNURL_AUTH_MACHINE;
    config.login = KNURL_AUTH_LOGIN_PASSWORD;
    config.send_email = NULL;
    config.email_userdata = NULL;
    config.factors = KNURL_AUTH_FACTOR_PASSWORD;
    config.session_check = NULL;
    CHECK(knurl_auth_new(&config, &external_machine, NULL) == KNURL_INVALID);
    config.machine_check = session_check;
    config.machine_check_userdata = f;
    CHECK(knurl_auth_new(&config, &external_machine, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/external-provision", external_machine,
                           KNURL_AUTH_WORKFLOW, &handle, NULL) == KNURL_OK);
    knurl_auth_config_init(&config);
    config.name = "ordered";
    config.json = f->store;
    config.factors = KNURL_AUTH_FACTOR_PASSWORD | KNURL_AUTH_FACTOR_TOTP;
    CHECK(knurl_auth_new(&config, &ordered, NULL) == KNURL_OK);
    config.name = "ordered-api";
    config.method = KNURL_AUTH_MACHINE;
    config.factors |= KNURL_AUTH_FACTOR_EMAIL;
    config.send_email = send_code;
    config.email_userdata = f;
    CHECK(knurl_auth_new(&config, &ordered_api, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/ordered-browser", ordered,
                           KNURL_AUTH_WORKFLOW, &handle, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/ordered-alternate", ordered,
                           KNURL_AUTH_WORKFLOW, &handle, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/ordered-machine", ordered_api,
                           KNURL_AUTH_WORKFLOW, &handle, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/login", browser, KNURL_AUTH_LOGIN, &handle,
                           NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/logout", browser, KNURL_AUTH_LOGOUT,
                           &handle, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/otp", otp, KNURL_AUTH_LOGIN, &handle,
                           NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/totp-enroll", browser,
                           KNURL_AUTH_TOTP_ENROLL_ENDPOINT, &handle,
                           NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/totp-enroll-email", email,
                           KNURL_AUTH_TOTP_ENROLL_ENDPOINT, &handle,
                           NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/email", email, KNURL_AUTH_LOGIN, &handle,
                           NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/email-code", email, KNURL_AUTH_EMAIL_START,
                           &handle, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/smtp-code", smtp, KNURL_AUTH_EMAIL_START,
                           &handle, NULL) == KNURL_OK);
    CHECK(knurl_auth_route(router, "/external", external, KNURL_AUTH_LOGIN,
                           &handle, NULL) == KNURL_OK);
    knurl_route_group_config_init(&group_config);
    group_config.prefix = "/app";
    CHECK(knurl_router_group(router, &group_config, &group, NULL) == KNURL_OK);
    CHECK(knurl_route_group_set_auth(group, browser, NULL) == KNURL_OK);
    knurl_blocking_route_config_init(&app);
    app.path = "/me";
    app.handler = native;
    CHECK(knurl_route_group_blocking_route(group, &app, &handle, NULL) ==
          KNURL_OK);
    knurl_static_config_init(&files);
    files.path = "/static";
    files.file_path = f->static_file;
    CHECK(knurl_router_static_file(router, &files, &handle, NULL) == KNURL_OK);
    CHECK(knurl_route_set_auth(handle, browser, NULL) == KNURL_OK);
    knurl_proxy_config_init(&proxy);
    proxy.path = "/proxy";
    snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%u",
             (unsigned int)f->proxy);
    proxy.target = proxy_url;
    CHECK(knurl_router_proxy(router, &proxy, &handle, NULL) == KNURL_OK);
    CHECK(knurl_route_set_auth(handle, browser, NULL) == KNURL_OK);
    proxy.path = "/machine-proxy";
    CHECK(knurl_router_proxy(router, &proxy, &handle, NULL) == KNURL_OK);
    CHECK(knurl_route_set_auth(handle, machine, NULL) == KNURL_OK);
    memset(&oauth, 0, sizeof(oauth));
    oauth.issuer = f->issuer;
    oauth.ca_file = f->cert;
    oauth.client_id = "proxy:client";
    oauth.client_secret = "s&ecret";
    oauth.scope = "machine";
    CHECK(knurl_oauth2_client_new(&oauth, &client, NULL) == KNURL_OK);
    CHECK(knurl_route_set_proxy_oauth2(handle, client, NULL) == KNURL_INVALID);
    proxy.path = "/oauth-proxy";
    proxy.target = f->issuer;
    proxy.tls_ca_pem = f->ca_pem;
    CHECK(knurl_router_proxy(router, &proxy, &handle, NULL) == KNURL_OK);
    CHECK(knurl_route_set_proxy_oauth2(handle, client, NULL) == KNURL_OK);
    knurl_oauth2_client_close(client);
    knurl_upload_stream_config_init(&upload);
    upload.path = "/slow";
    upload.open = upload_open;
    upload.write = upload_write;
    upload.finish = upload_finish;
    upload.userdata = f;
    CHECK(knurl_router_upload_stream(router, &upload, &handle, NULL) ==
          KNURL_OK);
    CHECK(knurl_route_set_auth(handle, slow, NULL) == KNURL_OK);
    plain_route(router, "/fast", NULL);
    plain_route(router, "/different-policy", email);
    plain_route(router, "/machine", machine);
    plain_route(router, "/machine-other", other);
    plain_route(router, "/oidc", issuer);
    plain_route(router, "/rotating-oidc", rotating);
    plain_route(router, "/external", external);
    plain_route(router, "/external-email", external_email);
    plain_route(router, "/external-machine", external_machine);
    plain_route(router, "/ordered-subject", ordered);
    plain_route(router, "/ordered-api", ordered_api);
    knurl_auth_close(browser);
    knurl_auth_close(otp);
    knurl_auth_close(email);
    knurl_auth_close(smtp);
    knurl_auth_close(external);
    knurl_auth_close(external_email);
    knurl_auth_close(external_machine);
    knurl_auth_close(machine);
    knurl_auth_close(other);
    knurl_auth_close(slow);
    knurl_auth_close(issuer);
    knurl_auth_close(rotating);
    knurl_auth_close(ordered);
    knurl_auth_close(ordered_api);
}

static void cli(const char *runner, const char *store, const char *operation,
                const char *user, const char *value, const char *input,
                char *output, size_t capacity, const char *users_id)
{
    int pipes[2], result[2], status;
    pid_t child;
    size_t length = 0;
    ssize_t count;
    CHECK(pipe(pipes) == 0 && pipe(result) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        const char *arguments[9];
        size_t count = 0;
        close(pipes[1]);
        CHECK(dup2(pipes[0], STDIN_FILENO) >= 0);
        close(pipes[0]);
        close(result[0]);
        if (output != NULL)
            CHECK(dup2(result[1], STDOUT_FILENO) >= 0);
        close(result[1]);
        arguments[count++] = runner;
        arguments[count++] = "auth";
        arguments[count++] = operation;
        arguments[count++] = store;
        if (user != NULL)
            arguments[count++] = user;
        if (value != NULL)
            arguments[count++] = value;
        if (users_id != NULL) {
            arguments[count++] = "--users-id";
            arguments[count++] = users_id;
        }
        arguments[count] = NULL;
        execv(runner, (char *const *)arguments);
        _exit(126);
    }
    close(pipes[0]);
    if (input != NULL)
        CHECK(write(pipes[1], input, strlen(input)) == (ssize_t)strlen(input));
    close(pipes[1]);
    close(result[1]);
    if (output != NULL) {
        while (length + 1 < capacity &&
               (count = read(result[0], output + length,
                             capacity - 1 - length)) > 0)
            length += (size_t)count;
        CHECK(length > 0 && output[length - 1] == '\n');
        output[length - 1] = 0;
    }
    close(result[0]);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
}

static void start_runner(struct fixture *f, const char *work,
                         const char *runner, const char *script)
{
    struct timespec retry = {0, 10000000L};
    char port[16], plain[16], out[4096], location[128];
    struct sockaddr_in address;
    int status, probe, i;
    do {
        close(bound_socket(&f->plain));
    } while (f->plain == f->tls);
    snprintf(port, sizeof(port), "%u", (unsigned int)f->tls);
    snprintf(plain, sizeof(plain), "%u", (unsigned int)f->plain);
    runner_pid = fork();
    CHECK(runner_pid >= 0);
    if (runner_pid == 0) {
        execl(runner, runner, script, work, port, plain, (char *)NULL);
        _exit(126);
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(f->tls);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (i = 0; i < 300; ++i) {
        probe = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(probe >= 0);
        status = connect(probe, (struct sockaddr *)&address, sizeof(address));
        close(probe);
        if (status == 0 &&
            exchange(f, 1, "GET", "/health", NULL, NULL, out) == 200)
            break;
        CHECK(waitpid(runner_pid, &status, WNOHANG) == 0);
        nanosleep(&retry, NULL);
    }
    CHECK(i < 300);
    CHECK(exchange(f, 0, "GET", "/health?probe=1", NULL, NULL, out) == 308);
    snprintf(location, sizeof(location), "https://localhost:%u/health?probe=1",
             (unsigned int)f->tls);
    CHECK(strstr(out, location) != NULL);
}

static void stop_runner(void)
{
    int status;
    pid_t pid = runner_pid;
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(waitpid(pid, &status, 0) == pid);
    runner_pid = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static int runner_test(struct fixture *f, const char *work, const char *store,
                       const char *runner, const char *script)
{
    const char credential[] =
        "pipeline:a local test secret with plenty of characters";
    char out[4096], session[256], b64[128], auth[180], form[160];
    char issued[320], client_id[41];
    char staff[KNURL_AUTH_TOTP_URI_CAPACITY],
        partner[KNURL_AUTH_TOTP_URI_CAPACITY];
    unsigned short first_port, second_port;
    cli(runner, store, "init", NULL, NULL, NULL, NULL, 0, NULL);
    cli(runner, store, "prune", NULL, NULL, NULL, NULL, 0, NULL);
    cli(runner, store, "user-password", "alice", NULL, password, NULL, 0, NULL);
    cli(runner, store, "user-email", "alice", "alice@example.test", NULL, NULL,
        0, NULL);
    cli(runner, store, "user-totp", "alice", NULL, NULL, f->enrollment,
        sizeof(f->enrollment), NULL);
    cli(runner, store, "user-totp", "alice", NULL, NULL, staff, sizeof(staff),
        "staff");
    cli(runner, store, "user-totp", "alice", NULL, NULL, partner,
        sizeof(partner), "partner");
    CHECK(strstr(staff, "Knurl:staff%2Falice") != NULL &&
          strstr(partner, "Knurl:partner%2Falice") != NULL);
    cli(runner, store, "user-totp", "alice", "--rotate", NULL, out, sizeof(out),
        "staff");
    CHECK(strstr(out, "Knurl:staff%2Falice") != NULL &&
          strcmp(out, staff) != 0);
    cli(runner, store, "client-issue", "machine", "pipeline", NULL,
        f->client_secret, sizeof(f->client_secret), NULL);
    close(bound_socket(&f->tls));
    f->tls_client = SSL_CTX_new(TLS_client_method());
    CHECK(f->tls_client != NULL &&
          SSL_CTX_load_verify_locations(f->tls_client, f->cert, NULL) == 1);
    SSL_CTX_set_verify(f->tls_client, SSL_VERIFY_PEER, NULL);
    start_runner(f, work, runner, script);
    CHECK(exchange(f, 1, "GET", "/service", NULL, NULL, out) == 401);
    CHECK(EVP_EncodeBlock((unsigned char *)b64,
                          (const unsigned char *)credential,
                          sizeof(credential) - 1) > 0);
    snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n", b64);
    CHECK(exchange(f, 1, "GET", "/service", auth, NULL, out) == 200);
    CHECK(exchange(f, 1, "GET", "/unavailable-token", NULL, NULL, out) == 502);
    CHECK(exchange(f, 1, "POST", "/provision", NULL, password_form, out) ==
          201);
    issued_basic(out, client_id, issued);
    CHECK(exchange(f, 1, "GET", "/issued", issued, NULL, out) == 200);
    snprintf(form, sizeof(form), "pipeline:%s", f->client_secret);
    CHECK(EVP_EncodeBlock((unsigned char *)b64, (const unsigned char *)form,
                          (int)strlen(form)) > 0);
    snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n", b64);
    CHECK(exchange(f, 1, "GET", "/machine", auth, NULL, out) == 200);
    CHECK(exchange(f, 1, "GET", "/machine-other", auth, NULL, out) == 401);
    cli(runner, store, "client-revoke", "machine", "pipeline", NULL, NULL, 0,
        NULL);
    CHECK(exchange(f, 1, "GET", "/machine", auth, NULL, out) == 401);
    first_port = f->tls;
    secondary_runner = runner_pid;
    close(bound_socket(&f->tls));
    start_runner(f, work, runner, script);
    second_port = f->tls;
    f->tls = first_port;
    totp_race(f, second_port);
    stop_runner();
    runner_pid = secondary_runner;
    secondary_runner = 0;
    CHECK(exchange(f, 1, "POST", "/login", NULL, password_form, out) == 204);
    cookie(out, session);
    CHECK(exchange(f, 1, "GET", "/app/settings", session, NULL, out) == 200 &&
          strstr(out, "\r\n\r\nalice:settings") != NULL);
    stop_runner();
    start_runner(f, work, runner, script);
    CHECK(exchange(f, 1, "GET", "/app/settings", session, NULL, out) == 200);
    cli(runner, store, "user-disable", "alice", NULL, NULL, NULL, 0, "staff");
    CHECK(exchange(f, 1, "GET", "/app/settings", session, NULL, out) == 200);
    cli(runner, store, "user-enable", "alice", NULL, NULL, NULL, 0, "staff");
    cli(runner, store, "user-disable", "alice", NULL, NULL, NULL, 0, NULL);
    CHECK(exchange(f, 1, "GET", "/app/settings", session, NULL, out) == 401);
    CHECK(exchange(f, 1, "GET", "/issued", issued, NULL, out) == 401);
    stop_runner();
    SSL_CTX_free(f->tls_client);
    return 0;
}

int main(int argc, char **argv)
{
    struct fixture f;
    knurl_server_config config;
    knurl_listener_config listeners[2];
    knurl_tls_identity identity;
    knurl_server *server;
    knurl_error error;
    knurl_status status;
    const char *host = "localhost";
    char store[1024], users_store[1024], uri[KNURL_AUTH_TOTP_URI_CAPACITY];
    FILE *file;
    int child;
    pid_t smtp_pid, proxy_pid, oauth_pid;
    CHECK(argc == 2 || argc == 4);
    server_pid = getpid();
    memset(&f, 0, sizeof(f));
    snprintf(store, sizeof(store), "%s/state", argv[1]);
    snprintf(f.store_path, sizeof(f.store_path), "%s", store);
    snprintf(f.cert, sizeof(f.cert), "%s/cert.pem", argv[1]);
    snprintf(f.key, sizeof(f.key), "%s/key.pem", argv[1]);
    if (argc == 4)
        return runner_test(&f, argv[1], store, argv[2], argv[3]);
    snprintf(users_store, sizeof(users_store), "%s/users-state", argv[1]);
    CHECK(knurl_auth_json_init(users_store, &error) == KNURL_OK);
    CHECK(knurl_auth_json_open(users_store, &f.users_store, &error) ==
          KNURL_OK);
    snprintf(f.mail_store_path, sizeof(f.mail_store_path), "%s/mail", argv[1]);
    CHECK(knurl_auth_json_init(f.mail_store_path, &error) == KNURL_OK);
    CHECK(knurl_auth_json_open(f.mail_store_path, &f.mail_store, &error) ==
          KNURL_OK);
    CHECK(knurl_auth_json_admin(f.mail_store, KNURL_AUTH_USER_PASSWORD, NULL,
                                "alice", password, &error) == KNURL_OK);
    CHECK(knurl_auth_json_admin(f.mail_store, KNURL_AUTH_USER_EMAIL, NULL,
                                "alice", "alice@example.test",
                                &error) == KNURL_OK);
    CHECK(knurl_auth_json_admin(f.mail_store, KNURL_AUTH_USER_PASSWORD, NULL,
                                "dave", password, &error) == KNURL_OK);
    CHECK(knurl_auth_json_admin(f.mail_store, KNURL_AUTH_USER_EMAIL, NULL,
                                "dave", "dave@example.test",
                                &error) == KNURL_OK);
    large_store(argv[1]);
    snprintf(f.static_file, sizeof(f.static_file), "%s/file.txt", argv[1]);
    snprintf(f.directory_status, sizeof(f.directory_status),
             "%s/directory-status", argv[1]);
    directory_status(&f, 'a');
    file = fopen(f.static_file, "wb");
    CHECK(file != NULL && fputs("protected fixture", file) >= 0 &&
          fclose(file) == 0);
    CHECK(knurl_auth_json_init(store, &error) == KNURL_OK);
    CHECK(knurl_auth_json_init(store, &error) == KNURL_OK);
    CHECK(knurl_auth_json_open(store, &f.store, &error) == KNURL_OK);
    CHECK(knurl_auth_json_admin(f.store, KNURL_AUTH_USER_PASSWORD, NULL,
                                "alice", password, &error) == KNURL_OK);
    CHECK(knurl_auth_json_admin(f.store, KNURL_AUTH_USER_PASSWORD, NULL, "bob",
                                password, &error) == KNURL_OK);
    CHECK(knurl_auth_json_admin(f.store, KNURL_AUTH_USER_PASSWORD, NULL,
                                "carol", password, &error) == KNURL_OK);
    CHECK(knurl_auth_json_admin(f.store, KNURL_AUTH_USER_PASSWORD, NULL, "dave",
                                password, &error) == KNURL_OK);
    CHECK(knurl_auth_json_admin(f.store, KNURL_AUTH_USER_EMAIL, NULL, "dave",
                                "dave@example.test", &error) == KNURL_OK);
    CHECK(knurl_auth_json_enroll_totp(f.store, NULL, "carol", 0,
                                      f.flow_enrollment[0],
                                      &error) == KNURL_OK);
    CHECK(knurl_auth_json_enroll_totp(f.store, NULL, "dave", 0,
                                      f.flow_enrollment[1],
                                      &error) == KNURL_OK);
    enroll_concurrently(&f);
    CHECK(knurl_auth_json_enroll_totp(f.store, NULL, "alice", 0, uri, &error) ==
              KNURL_CONFLICT &&
          uri[0] == 0);
    CHECK(knurl_auth_json_admin(f.store, KNURL_AUTH_USER_EMAIL, NULL, "alice",
                                "alice@example.test", &error) == KNURL_OK);
    CHECK(knurl_auth_json_issue(f.store, "machine", "pipeline", f.client_secret,
                                &error) == KNURL_OK);
    {
        char other[65];
        CHECK(knurl_auth_json_issue(f.store, "machine-other", "pipeline", other,
                                    &error) == KNURL_OK);
    }
    CHECK(pipe(f.entered) == 0 && pipe(f.resume) == 0 && pipe(f.headers) == 0 &&
          pipe(f.mail) == 0 && pipe(f.token_request) == 0 &&
          pipe(f.token_resume) == 0);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listeners[0]);
    knurl_listener_config_init(&listeners[1]);
    listeners[0].bind = listeners[1].bind = "127.0.0.1";
    listeners[1].kind = KNURL_LISTENER_HTTPS;
    config.listeners = listeners;
    config.listener_count = 2;
    config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.userdata = &f;
    knurl_tls_identity_init(&identity);
    identity.mode = KNURL_TLS_MANUAL;
    identity.domains = &host;
    identity.domain_count = 1;
    identity.certificate.kind = identity.private_key.kind = KNURL_MATERIAL_FILE;
    identity.certificate.path = f.cert;
    identity.private_key.path = f.key;
    config.tls.identities = &identity;
    config.tls.identity_count = 1;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    file = fopen(f.cert, "rb");
    CHECK(file != NULL && fread(f.ca_pem, 1, sizeof(f.ca_pem) - 1, file) > 0 &&
          feof(file) && fclose(file) == 0);
    f.signing_key = fixture_key(&f);
    f.rotated_key = EVP_RSA_gen(2048);
    CHECK(f.rotated_key != NULL);
    jwks(f.signing_key, "fixture", f.jwks);
    jwks(f.rotated_key, "rotated", f.next_jwks);
    smtp_pid = smtp_fixture(&f);
    proxy_pid = proxy_fixture(&f);
    oauth_pid = oauth_fixture(&f);
    routes(knurl_server_router(server), &f);
    status = knurl_server_run(server, NULL, &error);
    if (status != KNURL_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(status == KNURL_OK);
    if (waitpid(f.client, &child, 0) != f.client || !WIFEXITED(child) ||
        WEXITSTATUS(child) != 0) {
        (void)kill(smtp_pid, SIGTERM);
        (void)kill(proxy_pid, SIGTERM);
        (void)kill(oauth_pid, SIGTERM);
        (void)waitpid(smtp_pid, NULL, 0);
        (void)waitpid(proxy_pid, NULL, 0);
        (void)waitpid(oauth_pid, NULL, 0);
        return 1;
    }
    CHECK(waitpid(smtp_pid, &child, 0) == smtp_pid && WIFEXITED(child) &&
          WEXITSTATUS(child) == 0);
    CHECK(waitpid(proxy_pid, &child, 0) == proxy_pid && WIFEXITED(child) &&
          WEXITSTATUS(child) == 0);
    CHECK(waitpid(oauth_pid, &child, 0) == oauth_pid && WIFEXITED(child) &&
          WEXITSTATUS(child) == 0);
    CHECK(knurl_server_destroy(server, NULL) == KNURL_OK);
    EVP_PKEY_free(f.rotated_key);
    EVP_PKEY_free(f.signing_key);
    verify_state_files(store);
    knurl_auth_json_close(f.store);
    knurl_auth_json_close(f.mail_store);
    knurl_auth_json_close(f.users_store);
    return 0;
}
