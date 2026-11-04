#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
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

#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            (void)fprintf(stderr, "TLS line %d: %s\n", __LINE__, #test);       \
            return 1;                                                          \
        }                                                                      \
    } while (0)

struct fixture {
    EVP_PKEY *key;
    EVP_PKEY *other_key;
    X509 *ca;
    X509 *certificate;
    X509 *untrusted;
    char *cert_pem;
    char *key_pem;
    char *ca_pem;
    char *bundle;
    char path[32];
    pid_t client;
    unsigned short ports[7];
    int opens;
    int closes;
    int version;
    int tunnel_closed;
    int failure;
    int material_again;
    rotmos_status material_outcome;
};

struct material {
    struct fixture *fixture;
    size_t position;
};

struct tunnel {
    struct fixture *fixture;
    rotmos_request *request;
    unsigned char pending[128];
    size_t used;
    size_t offset;
    int eof;
};

static int extension(X509 *certificate, X509 *issuer, int nid, const char *text)
{
    X509V3_CTX context;
    X509_EXTENSION *item;
    int result;
    X509V3_set_ctx(&context, issuer, certificate, NULL, NULL, 0);
    item = X509V3_EXT_conf_nid(NULL, &context, nid, text);
    if (item == NULL)
        return 0;
    result = X509_add_ext(certificate, item, -1);
    X509_EXTENSION_free(item);
    return result;
}

static X509 *certificate(EVP_PKEY *key, X509 *ca, EVP_PKEY *signing)
{
    X509 *result;
    X509_NAME *name;
    result = X509_new();
    if (result == NULL)
        return NULL;
    name = X509_get_subject_name(result);
    if (!X509_set_version(result, 2) ||
        !ASN1_INTEGER_set(X509_get_serialNumber(result), ca == NULL ? 1 : 2) ||
        X509_gmtime_adj(X509_getm_notBefore(result), -60) == NULL ||
        X509_gmtime_adj(X509_getm_notAfter(result), 3600) == NULL ||
        !X509_set_pubkey(result, key) ||
        !X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            (const unsigned char *)(ca == NULL ? "fixture CA" : "localhost"),
            -1, -1, 0) ||
        !X509_set_issuer_name(result,
                              ca == NULL ? name : X509_get_subject_name(ca)) ||
        !extension(result, ca == NULL ? result : ca, NID_basic_constraints,
                   ca == NULL ? "critical,CA:TRUE" : "critical,CA:FALSE") ||
        !extension(result, ca == NULL ? result : ca, NID_key_usage,
                   ca == NULL ? "critical,keyCertSign,cRLSign"
                              : "critical,digitalSignature") ||
        (ca != NULL &&
         (!extension(result, ca, NID_ext_key_usage, "serverAuth,clientAuth") ||
          !extension(result, ca, NID_subject_alt_name,
                     "DNS:localhost,DNS:alias.local,DNS:secure.local,DNS:"
                     "optional.local"))) ||
        !X509_sign(result, signing, EVP_sha256())) {
        X509_free(result);
        return NULL;
    }
    return result;
}

static char *pem(X509 *cert, EVP_PKEY *key)
{
    BIO *bio;
    char *data;
    char *copy;
    long length;
    bio = BIO_new(BIO_s_mem());
    if (bio == NULL)
        return NULL;
    if (!(cert != NULL ? PEM_write_bio_X509(bio, cert)
                       : PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL,
                                                  NULL))) {
        BIO_free(bio);
        return NULL;
    }
    length = BIO_get_mem_data(bio, &data);
    copy = malloc((size_t)length + 1);
    if (copy != NULL) {
        memcpy(copy, data, (size_t)length);
        copy[length] = '\0';
    }
    BIO_free(bio);
    return copy;
}

static rotmos_status material_read(void *buffer, size_t capacity,
                                   size_t *produced, void *userdata,
                                   rotmos_error *error)
{
    struct material *state;
    size_t count;
    state = userdata;
    if (state->fixture->material_again) {
        *produced = 0;
        return ROTMOS_AGAIN;
    }
    count = strlen(state->fixture->key_pem) - state->position;
    if (count > capacity)
        count = capacity;
    if (count > 17)
        count = 17;
    memcpy(buffer, state->fixture->key_pem + state->position, count);
    state->position += count;
    *produced = count;
    rotmos_error_clear(error);
    return count == 0 ? ROTMOS_EOF : ROTMOS_OK;
}

static void material_close(void *userdata, rotmos_status outcome)
{
    struct material *state;
    state = userdata;
    if (outcome != state->fixture->material_outcome)
        state->fixture->failure = 1;
    ++state->fixture->closes;
    free(state);
}

static rotmos_status material_open(rotmos_source **out, void *userdata,
                                   rotmos_error *error)
{
    struct fixture *fixture;
    struct material *state;
    rotmos_source_config config;
    rotmos_status status;
    fixture = userdata;
    ++fixture->opens;
    state = calloc(1, sizeof(*state));
    if (state == NULL)
        return ROTMOS_NOMEM;
    state->fixture = fixture;
    rotmos_source_config_init(&config);
    config.read = material_read;
    config.close = material_close;
    config.userdata = state;
    status = rotmos_source_new(&config, out, error);
    if (status != ROTMOS_OK)
        free(state);
    return status;
}

static void tunnel_closed(rotmos_channel *channel,
                          const rotmos_channel_outcome *outcome, void *userdata)
{
    struct tunnel *state;
    (void)channel;
    state = userdata;
    if (outcome->cause != ROTMOS_OK || !outcome->read_eof ||
        !outcome->write_shutdown || outcome->sent_bytes.high ||
        outcome->sent_bytes.low != 65536 ||
        !rotmos_request_is_tls(state->request))
        state->fixture->failure = 1;
    ++state->fixture->tunnel_closed;
}

static void request_closed(rotmos_request *request,
                           const rotmos_request_outcome *outcome,
                           void *userdata)
{
    (void)request;
    (void)outcome;
    free(userdata);
}

static void tunnel_event(rotmos_channel *channel, unsigned int events,
                         void *userdata)
{
    static const char *names[] = {"Upgrade", "Connection",
                                  "Sec-WebSocket-Accept"};
    static const char *values[] = {"websocket", "Upgrade",
                                   "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="};
    rotmos_header headers[3];
    struct tunnel *state;
    rotmos_status status;
    size_t count;
    size_t i;
    state = userdata;
    if (rotmos_channel_state(channel) == ROTMOS_CHANNEL_HTTP_INPUT) {
        for (i = 0; i < 3; ++i) {
            headers[i].name.data = names[i];
            headers[i].name.length = strlen(names[i]);
            headers[i].value.data = values[i];
            headers[i].value.length = strlen(values[i]);
        }
        if (rotmos_channel_upgrade(channel, headers, 3, NULL) != ROTMOS_OK)
            goto failed;
    }
    if (events & ROTMOS_WRITE_CLOSED) {
        if (rotmos_channel_close(channel, ROTMOS_OK, NULL) != ROTMOS_OK)
            goto failed;
        return;
    }
    for (;;) {
        if (state->offset < state->used) {
            status =
                rotmos_channel_write(channel, state->pending + state->offset,
                                     state->used - state->offset, &count, NULL);
            if (status == ROTMOS_AGAIN)
                break;
            if (status != ROTMOS_OK)
                goto failed;
            state->offset += count;
            continue;
        }
        if (state->eof)
            break;
        status = rotmos_channel_read(channel, state->pending,
                                     sizeof(state->pending), &count, NULL);
        if (status == ROTMOS_AGAIN)
            break;
        if (status == ROTMOS_EOF) {
            state->eof = 1;
            if (rotmos_channel_shutdown_write(channel, NULL) != ROTMOS_OK)
                goto failed;
            break;
        }
        if (status != ROTMOS_OK)
            goto failed;
        state->used = count;
        state->offset = 0;
    }
    (void)rotmos_channel_interest(
        channel,
        (state->eof ? 0 : ROTMOS_READABLE) |
            (state->offset < state->used ? ROTMOS_WRITABLE : 0),
        NULL);
    return;
failed:
    state->fixture->failure = 1;
    (void)rotmos_channel_close(channel, ROTMOS_INVALID, NULL);
}

static rotmos_status headers(rotmos_request *request, rotmos_request_plan *plan,
                             void *userdata, rotmos_error *error)
{
    rotmos_channel_config config;
    rotmos_channel *channel;
    struct tunnel *state;
    state = calloc(1, sizeof(*state));
    if (state == NULL)
        return ROTMOS_NOMEM;
    state->fixture = userdata;
    state->request = request;
    rotmos_channel_config_init(&config);
    config.interest = ROTMOS_READABLE;
    config.send_bytes = 64;
    config.on_event = tunnel_event;
    config.on_close = tunnel_closed;
    config.userdata = state;
    if (rotmos_request_takeover(request, &config, &channel, error) !=
        ROTMOS_OK) {
        free(state);
        return ROTMOS_INVALID;
    }
    plan->disposition = ROTMOS_REQUEST_TAKEOVER;
    plan->on_close = request_closed;
    plan->userdata = state;
    return ROTMOS_OK;
}

static rotmos_status handler(rotmos_request *request, rotmos_reply *reply,
                             void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    fixture = userdata;
    if (rotmos_request_listener_index(request) == 0 &&
        rotmos_request_is_tls(request))
        return ROTMOS_INVALID;
    if (rotmos_request_path(request).length == 5 &&
        memcmp(rotmos_request_path(request).data, "/stop", 5) == 0) {
        if (fixture->failure || fixture->tunnel_closed != 1)
            return ROTMOS_INVALID;
        (void)rotmos_server_request_stop(rotmos_request_server(request), NULL);
    }
    return rotmos_reply_text(
        reply, 200, "text/plain",
        rotmos_request_is_tls(request) ? "secure" : "clear", error);
}

static int connect_to(unsigned short port)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    (void)inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    address.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static SSL *connection(struct fixture *fixture, int index, const char *sni,
                       int client_cert, int version, int *fd, SSL_CTX **context)
{
    SSL *ssl;
    *fd = connect_to(fixture->ports[index]);
    if (*fd < 0)
        return NULL;
    *context = SSL_CTX_new(TLS_client_method());
    if (*context == NULL)
        return NULL;
    (void)SSL_CTX_set_min_proto_version(*context, version);
    (void)SSL_CTX_set_max_proto_version(*context, version);
    (void)X509_STORE_add_cert(SSL_CTX_get_cert_store(*context), fixture->ca);
    SSL_CTX_set_verify(*context, SSL_VERIFY_PEER, NULL);
    if (client_cert) {
        (void)SSL_CTX_use_certificate(*context, client_cert == 1
                                                    ? fixture->certificate
                                                    : fixture->untrusted);
        (void)SSL_CTX_use_PrivateKey(
            *context, client_cert == 1 ? fixture->key : fixture->other_key);
    }
    ssl = SSL_new(*context);
    if (ssl != NULL) {
        (void)SSL_set_fd(ssl, *fd);
        if (sni != NULL)
            (void)SSL_set_tlsext_host_name(ssl, sni);
        (void)SSL_set1_host(
            ssl, sni == NULL || strcmp(sni, "unknown.local") == 0 ? "localhost"
                                                                  : sni);
    }
    return ssl;
}

static int exchange(struct fixture *fixture, int index, const char *sni,
                    int client_cert, int version, const char *host,
                    const char *target, const char *expected)
{
    SSL_CTX *ctx;
    SSL *ssl;
    char request[512];
    char reply[2048];
    int fd;
    int length;
    int count;
    size_t used;
    ssl = NULL;
    ctx = NULL;
    fd = -1;
    if (index > 0 && index < 4) {
        ssl = connection(fixture, index, sni, client_cert, version, &fd, &ctx);
        CHECK(ssl != NULL);
        count = SSL_connect(ssl);
        if (count != 1) {
            if (expected != NULL)
                ERR_print_errors_fp(stderr);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            (void)close(fd);
            return expected == NULL ? 0 : 1;
        }
        CHECK(SSL_version(ssl) == version &&
              SSL_get_verify_result(ssl) == X509_V_OK);
    } else {
        fd = connect_to(fixture->ports[index]);
        CHECK(fd >= 0);
    }
    length =
        snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                 target, host);
    count = ssl != NULL ? SSL_write(ssl, request, length)
                        : (int)write(fd, request, (size_t)length);
    if (expected != NULL)
        CHECK(count == length);
    used = 0;
    while (used < sizeof(reply) - 1) {
        count =
            ssl != NULL
                ? SSL_read(ssl, reply + used, (int)(sizeof(reply) - used - 1))
                : (int)read(fd, reply + used, sizeof(reply) - used - 1);
        if (count <= 0)
            break;
        used += (size_t)count;
    }
    reply[used] = '\0';
    if (expected != NULL && strstr(reply, expected) == NULL)
        (void)fprintf(stderr, "TLS expected %s, received %s\n", expected,
                      reply);
    CHECK(expected == NULL ? strstr(reply, "200 ") == NULL
                           : strstr(reply, expected) != NULL);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    CHECK(close(fd) == 0);
    return 0;
}

static int tunnel_client(struct fixture *fixture)
{
    SSL_CTX *ctx;
    SSL *ssl;
    unsigned char payload[1024];
    char reply[2048];
    char *end;
    int fd;
    int count;
    size_t used;
    size_t sent;
    size_t received;
    size_t i;
    const char *request =
        "GET /tunnel HTTP/1.1\r\nHost: localhost\r\nUpgrade: "
        "websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: "
        "13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\nabcde";
    ctx = NULL;
    fd = -1;
    ssl = connection(fixture, 1, "localhost", 0, fixture->version, &fd, &ctx);
    CHECK(ssl != NULL && SSL_connect(ssl) == 1);
    CHECK(SSL_write(ssl, request, (int)strlen(request)) ==
          (int)strlen(request));
    used = 0;
    do {
        count = SSL_read(ssl, reply + used, (int)(sizeof(reply) - used - 1));
        CHECK(count > 0);
        used += (size_t)count;
        reply[used] = '\0';
    } while ((end = strstr(reply, "\r\n\r\n")) == NULL);
    CHECK(strncmp(reply, "HTTP/1.1 101 ", 13) == 0);
    received = used - (size_t)(end + 4 - reply);
    for (i = 0; i < received; ++i)
        CHECK((unsigned char)end[4 + i] == (unsigned char)('a' + i % 26));
    sent = 5;
    while (sent < 65536) {
        used = 65536 - sent;
        if (used > sizeof(payload))
            used = sizeof(payload);
        for (i = 0; i < used; ++i)
            payload[i] = (unsigned char)('a' + (sent + i) % 26);
        CHECK(SSL_write(ssl, payload, (int)used) == (int)used);
        sent += used;
    }
    CHECK(SSL_shutdown(ssl) >= 0);
    while ((count = SSL_read(ssl, payload, sizeof(payload))) > 0) {
        CHECK((size_t)count <= 65536 - received);
        for (i = 0; i < (size_t)count; ++i)
            CHECK(payload[i] == (unsigned char)('a' + (received + i) % 26));
        received += (size_t)count;
    }
    CHECK(received == 65536 &&
          SSL_get_error(ssl, count) == SSL_ERROR_ZERO_RETURN);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    CHECK(close(fd) == 0);
    return 0;
}

static int client(struct fixture *fixture)
{
    CHECK(exchange(fixture, 1, "localhost", 0, fixture->version, "localhost",
                   "/", "\r\n\r\nsecure") == 0);
    CHECK(exchange(fixture, 1, "alias.local", 0, fixture->version,
                   "alias.local", "/", "\r\n\r\nsecure") == 0);
    CHECK(exchange(fixture, 1, "localhost", 0, fixture->version, "alias.local",
                   "/", "421 ") == 0);
    CHECK(exchange(fixture, 1, "unknown.local", 0, fixture->version,
                   "unknown.local", "/", NULL) == 0);
    CHECK(exchange(fixture, 1, NULL, 0, fixture->version, "localhost", "/",
                   NULL) == 0);
    CHECK(exchange(fixture, 1, "localhost", 0,
                   fixture->version == TLS1_2_VERSION ? TLS1_3_VERSION
                                                      : TLS1_2_VERSION,
                   "localhost", "/", NULL) == 0);
    CHECK(exchange(fixture, 2, "secure.local", 0, fixture->version,
                   "secure.local", "/", NULL) == 0);
    CHECK(exchange(fixture, 2, "secure.local", 1, fixture->version,
                   "secure.local", "/", "\r\n\r\nsecure") == 0);
    CHECK(exchange(fixture, 2, "secure.local", 2, fixture->version,
                   "secure.local", "/", NULL) == 0);
    CHECK(exchange(fixture, 3, "optional.local", 0, fixture->version,
                   "optional.local", "/", "\r\n\r\nsecure") == 0);
    CHECK(exchange(fixture, 3, "optional.local", 1, fixture->version,
                   "optional.local", "/", "\r\n\r\nsecure") == 0);
    CHECK(exchange(
              fixture, 4, NULL, 0, 0, "LOCALHOST:8080", "/a%2fb?x=%2f&empty=",
              "location: https://LOCALHOST:8443/a%2fb?x=%2f&empty=\r\n") == 0);
    CHECK(exchange(fixture, 5, NULL, 0, 0, "localhost:80", "/?",
                   "location: https://localhost/?\r\n") == 0);
    CHECK(exchange(fixture, 5, NULL, 0, 0, "evil.local", "/", "400 ") == 0);
    CHECK(exchange(fixture, 6, NULL, 0, 0, "[::1]:8080", "/a?x=1",
                   "location: https://[::1]:8443/a?x=1\r\n") == 0);
    CHECK(exchange(fixture, 6, NULL, 0, 0, "evil.local@localhost", "/",
                   "400 ") == 0);
    CHECK(tunnel_client(fixture) == 0);
    CHECK(exchange(fixture, 0, NULL, 0, 0, "localhost", "/stop",
                   "\r\n\r\nclear") == 0);
    return 0;
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process,
                           void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    rotmos_bound_listener listener;
    size_t i;
    int result;
    (void)process;
    fixture = userdata;
    for (i = 0; i < 7; ++i) {
        if (rotmos_server_listener_at(server, i, &listener, error) != ROTMOS_OK)
            return ROTMOS_INVALID;
        fixture->ports[i] = listener.port;
    }
    fixture->client = fork();
    if (fixture->client < 0)
        return ROTMOS_IO;
    if (fixture->client == 0) {
        (void)signal(SIGPIPE, SIG_IGN);
        result = client(fixture);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    return ROTMOS_OK;
}

static int invalid(const rotmos_server_config *config, rotmos_status expected)
{
    rotmos_server *server;
    rotmos_error error;
    CHECK(rotmos_server_new(config, &server, &error) == ROTMOS_OK);
    CHECK(rotmos_server_validate(server, &error) == expected);
    CHECK(rotmos_server_destroy(server, NULL) == ROTMOS_OK);
    return 0;
}

static int file_bound(const rotmos_server_config *config)
{
    rotmos_server_config limited;
    rotmos_tls_identity identity;
    limited = *config;
    identity = config->tls.identities[0];
    rotmos_tls_material_init(&identity.extra_chain);
    limited.tls.identities = &identity;
    limited.tls.identity_count = 1;
    limited.tls.max_material_bytes = 128;
    limited.listener_count = 1;
    return invalid(&limited, ROTMOS_LIMIT);
}

static int run(struct fixture *fixture)
{
    rotmos_server_config config;
    rotmos_listener_config listeners[7];
    rotmos_tls_identity identities[4];
    rotmos_route_config route;
    const char *domains[] = {"localhost", "alias.local", "secure.local",
                             "optional.local", "*"};
    rotmos_server *server;
    rotmos_run_result result;
    rotmos_status status;
    rotmos_error error;
    int child_status;
    int old_opens;
    size_t i;
    char *bad_key;
    const char *ciphers;
    rotmos_server_config_init(&config);
    for (i = 0; i < 4; ++i) {
        rotmos_tls_identity_init(&identities[i]);
        identities[i].mode = ROTMOS_TLS_MANUAL;
        identities[i].domains = &domains[i == 0 ? 0 : i + 1];
        identities[i].domain_count = i == 0 ? 2 : 1;
        identities[i].certificate.kind = ROTMOS_MATERIAL_MEMORY;
        identities[i].certificate.bytes.data = fixture->cert_pem;
        identities[i].certificate.bytes.length = strlen(fixture->cert_pem);
        identities[i].private_key.kind = ROTMOS_MATERIAL_MEMORY;
        identities[i].private_key.bytes.data = fixture->key_pem;
        identities[i].private_key.bytes.length = strlen(fixture->key_pem);
        identities[i].extra_chain.kind = ROTMOS_MATERIAL_MEMORY;
        identities[i].extra_chain.bytes.data = fixture->ca_pem;
        identities[i].extra_chain.bytes.length = strlen(fixture->ca_pem);
    }
    identities[0].certificate.kind = ROTMOS_MATERIAL_FILE;
    identities[0].certificate.path = fixture->path;
    identities[0].certificate.bytes.data = NULL;
    identities[0].certificate.bytes.length = 0;
    rotmos_tls_material_init(&identities[0].private_key);
    identities[0].private_key.kind = ROTMOS_MATERIAL_CALLBACK;
    identities[0].private_key.open = material_open;
    identities[0].private_key.userdata = fixture;
    rotmos_tls_material_init(&identities[1].certificate);
    rotmos_tls_material_init(&identities[1].private_key);
    identities[1].bundle.kind = ROTMOS_MATERIAL_MEMORY;
    identities[1].bundle.bytes.data = fixture->bundle;
    identities[1].bundle.bytes.length = strlen(fixture->bundle);
    for (i = 1; i < 3; ++i) {
        identities[i].client_ca.kind = ROTMOS_MATERIAL_MEMORY;
        identities[i].client_ca.bytes.data = fixture->ca_pem;
        identities[i].client_ca.bytes.length = strlen(fixture->ca_pem);
    }
    identities[1].require_client_certificate = 1;
    for (i = 0; i < 7; ++i) {
        rotmos_listener_config_init(&listeners[i]);
        listeners[i].bind = "127.0.0.1";
        listeners[i].kind =
            i == 0 ? ROTMOS_LISTENER_HTTP
                   : (i < 4 ? ROTMOS_LISTENER_HTTPS : ROTMOS_LISTENER_REDIRECT);
        listeners[i].identity_index =
            i < 4 ? (i == 0 ? 0 : i - 1) : (i == 6 ? 3 : 0);
        listeners[i].redirect_https_port = i == 5 ? 443 : 8443;
    }
    config.listeners = listeners;
    config.listener_count = 7;
    config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.workers.shutdown_timeout_ms = 1000;
    config.tls.identities = identities;
    config.tls.identity_count = 4;
    config.tls.cipher_list = "ECDHE-ECDSA-AES128-GCM-SHA256";
    config.tls.version = fixture->version == TLS1_2_VERSION
                             ? ROTMOS_TLS_VERSION_1_2
                             : ROTMOS_TLS_VERSION_1_3;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.userdata = fixture;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    if (fixture->version == TLS1_2_VERSION) {
        CHECK(file_bound(&config) == 0);
        bad_key = pem(NULL, fixture->other_key);
        CHECK(bad_key != NULL);
        identities[2].private_key.bytes.data = bad_key;
        identities[2].private_key.bytes.length = strlen(bad_key);
        CHECK(invalid(&config, ROTMOS_INVALID) == 0);
        identities[2].private_key.bytes.data = fixture->key_pem;
        identities[2].private_key.bytes.length = strlen(fixture->key_pem);
        free(bad_key);
        ciphers = config.tls.cipher_list;
        config.tls.cipher_list = "no-such-cipher";
        CHECK(invalid(&config, ROTMOS_INVALID) == 0);
        config.tls.cipher_list = ciphers;
        identities[2].client_ca.bytes.data = "invalid";
        identities[2].client_ca.bytes.length = 7;
        CHECK(invalid(&config, ROTMOS_INVALID) == 0);
        identities[2].client_ca.bytes.data = fixture->ca_pem;
        identities[2].client_ca.bytes.length = strlen(fixture->ca_pem);
        fixture->material_again = 1;
        fixture->material_outcome = ROTMOS_STATE;
        CHECK(invalid(&config, ROTMOS_STATE) == 0);
        fixture->material_again = 0;
        fixture->material_outcome = ROTMOS_OK;
        CHECK(fixture->opens == fixture->closes && !fixture->failure);
    }
    old_opens = fixture->opens;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    CHECK(fixture->opens == old_opens);
    CHECK(rotmos_server_validate(server, &error) == ROTMOS_OK);
    CHECK(fixture->opens == old_opens + 1 && fixture->closes == fixture->opens);
    rotmos_route_config_init(&route);
    route.path = "/tunnel";
    route.on_headers = headers;
    route.userdata = fixture;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    rotmos_route_config_init(&route);
    route.path = "^/";
    route.path_kind = ROTMOS_PATH_REGEX;
    route.handler = handler;
    route.userdata = fixture;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    fixture->client = -1;
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        (void)fprintf(stderr, "TLS runtime %s: %s\n", error.operation,
                      error.message);
    CHECK(status == ROTMOS_OK && !result.escalated);
    CHECK(waitpid(fixture->client, &child_status, 0) == fixture->client);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(fixture->opens == old_opens + 2 &&
          fixture->closes == fixture->opens && !fixture->failure);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    return 0;
}

int main(void)
{
    struct fixture fixture;
    EVP_PKEY *ca_key;
    FILE *file;
    int fd;
    memset(&fixture, 0, sizeof(fixture));
    fixture.key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "prime256v1");
    fixture.other_key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "prime256v1");
    ca_key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "prime256v1");
    CHECK(fixture.key != NULL && fixture.other_key != NULL && ca_key != NULL);
    fixture.ca = certificate(ca_key, NULL, ca_key);
    fixture.certificate = certificate(fixture.key, fixture.ca, ca_key);
    fixture.untrusted =
        certificate(fixture.other_key, fixture.ca, fixture.other_key);
    CHECK(fixture.ca != NULL && fixture.certificate != NULL &&
          fixture.untrusted != NULL);
    fixture.cert_pem = pem(fixture.certificate, NULL);
    fixture.key_pem = pem(NULL, fixture.key);
    fixture.ca_pem = pem(fixture.ca, NULL);
    CHECK(fixture.cert_pem != NULL && fixture.key_pem != NULL &&
          fixture.ca_pem != NULL);
    fixture.bundle =
        malloc(strlen(fixture.cert_pem) + strlen(fixture.key_pem) + 1);
    CHECK(fixture.bundle != NULL);
    (void)strcpy(fixture.bundle, fixture.cert_pem);
    (void)strcat(fixture.bundle, fixture.key_pem);
    (void)strcpy(fixture.path, "rotmos-tls-XXXXXX");
    fd = mkstemp(fixture.path);
    CHECK(fd >= 0);
    file = fdopen(fd, "w");
    CHECK(file != NULL && fputs(fixture.cert_pem, file) >= 0 &&
          fclose(file) == 0);
    fixture.version = TLS1_2_VERSION;
    CHECK(run(&fixture) == 0);
    fixture.version = TLS1_3_VERSION;
    CHECK(run(&fixture) == 0);
    CHECK(unlink(fixture.path) == 0);
    free(fixture.cert_pem);
    free(fixture.key_pem);
    free(fixture.ca_pem);
    free(fixture.bundle);
    X509_free(fixture.certificate);
    X509_free(fixture.untrusted);
    X509_free(fixture.ca);
    EVP_PKEY_free(fixture.key);
    EVP_PKEY_free(fixture.other_key);
    EVP_PKEY_free(ca_key);
    return 0;
}
