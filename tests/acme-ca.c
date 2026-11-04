#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "acme-ca.h"

#include <lonejson.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

struct jws {
    char *protected_data;
    char *payload;
    char *signature;
};
struct jwk {
    char *e;
    char *n;
    char *kty;
};
struct protected_doc {
    char *url;
    char *nonce;
    char *alg;
    char *kid;
    struct jwk jwk;
};
struct identifier {
    char *type;
    char *value;
};
struct payload_doc {
    int existing;
    char *csr;
    lonejson_object_array identifiers;
};
static const lonejson_field jws_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct jws, protected_data, "protected"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct jws, payload, "payload"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct jws, signature, "signature")};
LONEJSON_MAP_DEFINE(jws_map, struct jws, jws_fields);
static const lonejson_field jwk_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct jwk, e, "e"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct jwk, n, "n"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct jwk, kty, "kty")};
LONEJSON_MAP_DEFINE(jwk_map, struct jwk, jwk_fields);
static const lonejson_field protected_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct protected_doc, url, "url"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct protected_doc, nonce, "nonce"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct protected_doc, alg, "alg"),
    LONEJSON_FIELD_STRING_ALLOC(struct protected_doc, kid, "kid"),
    LONEJSON_FIELD_OBJECT(struct protected_doc, jwk, "jwk", &jwk_map)};
LONEJSON_MAP_DEFINE(protected_map, struct protected_doc, protected_fields);
static const lonejson_field identifier_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct identifier, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct identifier, value, "value")};
LONEJSON_MAP_DEFINE(identifier_map, struct identifier, identifier_fields);
static const lonejson_field payload_fields[] = {
    LONEJSON_FIELD_BOOL(struct payload_doc, existing, "onlyReturnExisting"),
    LONEJSON_FIELD_STRING_ALLOC(struct payload_doc, csr, "csr"),
    LONEJSON_FIELD_OBJECT_ARRAY(struct payload_doc, identifiers, "identifiers",
                                struct identifier, &identifier_map,
                                LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(payload_map, struct payload_doc, payload_fields);

static int parse(const lonejson_map *map, void *out, const char *input)
{
    lonejson *runtime;
    lonejson_error error;
    lonejson_status status;
    memset(out, 0, map->struct_size);
    runtime = lonejson_new(NULL, &error);
    if (runtime == NULL)
        return 0;
    status = lonejson_parse_cstr(runtime, map, out, input, &error);
    lonejson_free(runtime);
    return status == LONEJSON_STATUS_OK;
}

static char *join(const char *directory, const char *leaf)
{
    char *result;
    size_t size;
    size = strlen(directory) + strlen(leaf) + 2;
    result = malloc(size);
    if (result != NULL)
        (void)snprintf(result, size, "%s/%s", directory, leaf);
    return result;
}

int test_ca_write(const char *directory, const char *leaf, const char *value)
{
    char *name;
    FILE *file;
    int okay;
    name = join(directory, leaf);
    if (name == NULL)
        return 0;
    file = fopen(name, "w");
    free(name);
    okay = file != NULL && fputs(value, file) >= 0;
    if (file != NULL && fclose(file) != 0)
        okay = 0;
    return okay;
}

int test_ca_remove(const char *directory)
{
    DIR *stream;
    struct dirent *entry;
    struct stat info;
    char *name;
    int okay;
    stream = opendir(directory);
    if (stream == NULL)
        return 0;
    okay = 1;
    while ((entry = readdir(stream)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        name = join(directory, entry->d_name);
        if (name == NULL || lstat(name, &info) != 0)
            okay = 0;
        else if (S_ISDIR(info.st_mode)) {
            if (!test_ca_remove(name))
                okay = 0;
        } else if (unlink(name) != 0)
            okay = 0;
        free(name);
    }
    if (closedir(stream) != 0 || rmdir(directory) != 0)
        okay = 0;
    return okay;
}

static char *pem(X509 *certificate, EVP_PKEY *key)
{
    BIO *bio;
    char *result;
    char *data;
    long length;
    bio = BIO_new(BIO_s_mem());
    if (bio == NULL)
        return NULL;
    result = NULL;
    if (certificate != NULL
            ? PEM_write_bio_X509(bio, certificate)
            : PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL)) {
        length = BIO_get_mem_data(bio, &data);
        result = malloc((size_t)length + 1);
        if (result != NULL) {
            memcpy(result, data, (size_t)length);
            result[length] = '\0';
        }
    }
    BIO_free(bio);
    return result;
}

static int extension(X509 *certificate, X509 *issuer, int nid, const char *text)
{
    X509V3_CTX context;
    X509_EXTENSION *item;
    int okay;
    X509V3_set_ctx(&context, issuer, certificate, NULL, NULL, 0);
    item = X509V3_EXT_conf_nid(NULL, &context, nid, text);
    okay = item != NULL && X509_add_ext(certificate, item, -1);
    X509_EXTENSION_free(item);
    return okay;
}

static X509 *certificate(struct test_ca *ca, EVP_PKEY *key, int round)
{
    X509 *result;
    X509_NAME *name;
    X509 *issuer;
    result = X509_new();
    if (result == NULL)
        return NULL;
    issuer = round == 0 ? result : ca->root;
    name = X509_get_subject_name(result);
    if (!X509_set_version(result, 2) ||
        !ASN1_INTEGER_set(X509_get_serialNumber(result), round) ||
        X509_gmtime_adj(X509_getm_notBefore(result), -60) == NULL ||
        X509_gmtime_adj(X509_getm_notAfter(result),
                        (round == 1 ? 2L : 90L) * 86400L) == NULL ||
        !X509_set_pubkey(result, key) ||
        !X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            (const unsigned char *)(round == 0 ? "rotmos fixture CA"
                                               : "localhost"),
            -1, -1, 0) ||
        !X509_set_issuer_name(result, X509_get_subject_name(issuer)) ||
        !extension(result, issuer, NID_basic_constraints,
                   round == 0 ? "critical,CA:TRUE" : "critical,CA:FALSE") ||
        !extension(result, issuer, NID_key_usage,
                   round == 0 ? "critical,keyCertSign,digitalSignature"
                              : "critical,digitalSignature") ||
        !extension(result, issuer, NID_subject_alt_name,
                   round == 0 ? "DNS:localhost,IP:127.0.0.1"
                              : "DNS:localhost") ||
        !X509_sign(result, ca->signing, EVP_sha256())) {
        X509_free(result);
        return NULL;
    }
    return result;
}

static char *encode(const unsigned char *data, size_t length)
{
    char *out;
    size_t i;
    size_t size;
    size = 4 * ((length + 2) / 3) + 1;
    out = malloc(size);
    if (out == NULL)
        return NULL;
    (void)EVP_EncodeBlock((unsigned char *)out, data, (int)length);
    for (i = 0; out[i] != '\0'; ++i) {
        if (out[i] == '+')
            out[i] = '-';
        if (out[i] == '/')
            out[i] = '_';
        if (out[i] == '=') {
            out[i] = '\0';
            break;
        }
    }
    return out;
}

static unsigned char *decode(const char *input, size_t *length)
{
    unsigned char *data;
    char *copy;
    size_t size;
    size_t padded;
    size_t i;
    int count;
    size = strlen(input);
    if (size % 4 == 1)
        return NULL;
    padded = (size + 3) / 4 * 4;
    copy = malloc(padded + 1);
    data = malloc(padded + 1);
    if (copy == NULL || data == NULL) {
        free(copy);
        free(data);
        return NULL;
    }
    for (i = 0; i < size; ++i)
        copy[i] = input[i] == '-' ? '+' : (input[i] == '_' ? '/' : input[i]);
    while (i < padded)
        copy[i++] = '=';
    copy[padded] = '\0';
    count = size != 0
                ? EVP_DecodeBlock(data, (unsigned char *)copy, (int)padded)
                : 0;
    free(copy);
    if (count < 0) {
        free(data);
        return NULL;
    }
    *length = (size_t)count - (padded - size);
    data[*length] = 0;
    return data;
}

static char *number(EVP_PKEY *key, const char *parameter)
{
    BIGNUM *value;
    unsigned char *bytes;
    char *result;
    int size;
    value = NULL;
    if (!EVP_PKEY_get_bn_param(key, parameter, &value))
        return NULL;
    size = BN_num_bytes(value);
    bytes = malloc((size_t)size);
    result = NULL;
    if (bytes != NULL) {
        (void)BN_bn2bin(value, bytes);
        result = encode(bytes, (size_t)size);
    }
    free(bytes);
    BN_free(value);
    return result;
}

struct test_ca *test_ca_new(const char *directory, int mode)
{
    struct test_ca *ca;
    struct sockaddr_in address;
    socklen_t size;
    char hash[32];
    ca = calloc(1, sizeof(*ca));
    if (ca == NULL)
        return NULL;
    ca->mode = mode;
    ca->listener = -1;
    ca->report = mmap(NULL, sizeof(*ca->report), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ca->report == MAP_FAILED) {
        ca->report = NULL;
        goto failed;
    }
    ca->account = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", 2048);
    ca->domain = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", 2048);
    ca->signing = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", 2048);
    if (ca->account == NULL || ca->domain == NULL || ca->signing == NULL)
        goto failed;
    ca->root = certificate(ca, ca->signing, 0);
    if (ca->root == NULL)
        goto failed;
    ca->root_pem = pem(ca->root, NULL);
    ca->account_pem = pem(NULL, ca->account);
    ca->domain_pem = pem(NULL, ca->domain);
    ca->exponent = number(ca->account, OSSL_PKEY_PARAM_RSA_E);
    ca->modulus = number(ca->account, OSSL_PKEY_PARAM_RSA_N);
    if (ca->root_pem == NULL || ca->account_pem == NULL ||
        ca->domain_pem == NULL || ca->exponent == NULL || ca->modulus == NULL)
        goto failed;
    (void)snprintf(hash, sizeof(hash), "%08lx.0",
                   X509_subject_name_hash(ca->root));
    if (!test_ca_write(directory, "ca.pem", ca->root_pem) ||
        !test_ca_write(directory, hash, ca->root_pem))
        goto failed;
    ca->context = SSL_CTX_new(TLS_server_method());
    if (ca->context == NULL ||
        !SSL_CTX_use_certificate(ca->context, ca->root) ||
        !SSL_CTX_use_PrivateKey(ca->context, ca->signing))
        goto failed;
    ca->listener = socket(AF_INET, SOCK_STREAM, 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    (void)inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (ca->listener < 0 ||
        bind(ca->listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(ca->listener, 8) != 0)
        goto failed;
    size = sizeof(address);
    if (getsockname(ca->listener, (struct sockaddr *)&address, &size) != 0)
        goto failed;
    (void)snprintf(ca->base, sizeof(ca->base), "https://127.0.0.1:%u",
                   (unsigned int)ntohs(address.sin_port));
    return ca;
failed:
    test_ca_free(ca);
    return NULL;
}

void test_ca_free(struct test_ca *ca)
{
    if (ca == NULL)
        return;
    if (ca->pid > 0) {
        (void)kill(ca->pid, SIGTERM);
        (void)waitpid(ca->pid, NULL, 0);
    }
    if (ca->listener >= 0)
        (void)close(ca->listener);
    if (ca->report != NULL)
        (void)munmap(ca->report, sizeof(*ca->report));
    SSL_CTX_free(ca->context);
    EVP_PKEY_free(ca->account);
    EVP_PKEY_free(ca->domain);
    EVP_PKEY_free(ca->signing);
    X509_free(ca->root);
    free(ca->account_pem);
    free(ca->domain_pem);
    free(ca->root_pem);
    free(ca->exponent);
    free(ca->modulus);
    free(ca->issued_pem);
    free(ca);
}

static int connect_to(unsigned short port)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    (void)inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
        return fd;
    (void)close(fd);
    return -1;
}

int test_ca_client(struct test_ca *ca, unsigned short port, int serial,
                   int status, int challenge)
{
    static int reported;
    static const unsigned char alpn[] = {10,  'a', 'c', 'm', 'e', '-',
                                         't', 'l', 's', '/', '1'};
    static const char get[] =
        "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    SSL_CTX *ctx;
    SSL *ssl;
    X509 *peer;
    ASN1_OBJECT *oid;
    X509_EXTENSION *item;
    ASN1_OCTET_STRING *value;
    const unsigned char *selected;
    unsigned int selected_length;
    char response[2048];
    char code[16];
    size_t used;
    int okay;
    int fd;
    int count;
    int index;
    fd = connect_to(port);
    if (fd < 0)
        return 0;
    ctx = SSL_CTX_new(TLS_client_method());
    ssl = ctx != NULL ? SSL_new(ctx) : NULL;
    okay = ssl != NULL;
    peer = NULL;
    response[0] = '\0';
    if (okay) {
        (void)SSL_set_fd(ssl, fd);
        (void)SSL_set_tlsext_host_name(ssl, "localhost");
        if (challenge)
            (void)SSL_set_alpn_protos(ssl, alpn, sizeof(alpn));
        else {
            (void)X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), ca->root);
            SSL_set_verify(ssl, SSL_VERIFY_PEER, NULL);
            (void)SSL_set1_host(ssl, "localhost");
        }
        okay = SSL_connect(ssl) == 1;
        peer = okay ? SSL_get1_peer_certificate(ssl) : NULL;
        okay = okay && peer != NULL;
    }
    if (okay && challenge) {
        SSL_get0_alpn_selected(ssl, &selected, &selected_length);
        oid = OBJ_txt2obj("1.3.6.1.5.5.7.1.31", 1);
        index = X509_get_ext_by_OBJ(peer, oid, -1);
        ASN1_OBJECT_free(oid);
        item = index >= 0 ? X509_get_ext(peer, index) : NULL;
        value = item != NULL ? X509_EXTENSION_get_data(item) : NULL;
        okay = selected_length == 10 && !memcmp(selected, alpn + 1, 10) &&
               item != NULL && X509_EXTENSION_get_critical(item) &&
               ASN1_STRING_length(value) == 34 &&
               !memcmp(ASN1_STRING_get0_data(value) + 2, ca->digest, 32) &&
               X509_check_host(peer, "localhost", 0, 0, NULL) == 1 &&
               X509_verify(peer, ca->domain) == 1;
    } else if (okay) {
        okay =
            ASN1_INTEGER_get(X509_get_serialNumber(peer)) == serial &&
            SSL_get_verify_result(ssl) == X509_V_OK &&
            SSL_write(ssl, get, (int)sizeof(get) - 1) == (int)sizeof(get) - 1;
        used = 0;
        while (okay && used < sizeof(response) - 1) {
            count = SSL_read(ssl, response + used,
                             (int)(sizeof(response) - used - 1));
            if (count <= 0)
                break;
            used += (size_t)count;
        }
        response[used] = '\0';
        (void)snprintf(code, sizeof(code), " %d ", status);
        okay = okay && strstr(response, code) != NULL;
    }
    if (!okay && !challenge && !reported) {
        reported = 1;
        (void)fprintf(
            stderr, "CA application client verify=%ld serial=%ld response=%s\n",
            ssl != NULL ? SSL_get_verify_result(ssl) : -1L,
            peer != NULL ? ASN1_INTEGER_get(X509_get_serialNumber(peer)) : -1L,
            response);
        ERR_print_errors_fp(stderr);
    }
    X509_free(peer);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    (void)close(fd);
    return okay;
}

static int verify(struct test_ca *ca, const char *endpoint, const char *input,
                  struct payload_doc *payload)
{
    struct jws jws;
    struct protected_doc header;
    EVP_MD_CTX *verify_context;
    unsigned char *protected_data;
    unsigned char *body;
    unsigned char *signature;
    size_t size;
    size_t signature_size;
    char url[160];
    int okay;
    memset(&jws, 0, sizeof(jws));
    memset(&header, 0, sizeof(header));
    protected_data = body = signature = NULL;
    verify_context = NULL;
    okay = parse(&jws_map, &jws, input);
    if (!okay)
        goto done;
    protected_data = decode(jws.protected_data, &size);
    signature = decode(jws.signature, &signature_size);
    body = decode(jws.payload, &size);
    okay = protected_data != NULL && signature != NULL && body != NULL &&
           parse(&protected_map, &header, (char *)protected_data);
    if (!okay)
        goto done;
    (void)snprintf(url, sizeof(url), "%s%s", ca->base, endpoint);
    okay = !strcmp(header.url, url) && !strcmp(header.alg, "RS256") &&
           !strcmp(header.nonce, ca->nonce) && !ca->nonce_used;
    ca->nonce_used = 1;
    if (!strcmp(endpoint, "/account")) {
        okay = okay && header.kid == NULL && header.jwk.e != NULL &&
               header.jwk.n != NULL && !strcmp(header.jwk.kty, "RSA") &&
               !strcmp(header.jwk.e, ca->exponent) &&
               !strcmp(header.jwk.n, ca->modulus);
    } else {
        (void)snprintf(url, sizeof(url), "%s/account/1", ca->base);
        okay = okay && header.kid != NULL && !strcmp(header.kid, url);
    }
    verify_context = EVP_MD_CTX_new();
    okay =
        okay && verify_context != NULL &&
        EVP_DigestVerifyInit(verify_context, NULL, EVP_sha256(), NULL,
                             ca->account) &&
        EVP_DigestVerifyUpdate(verify_context, jws.protected_data,
                               strlen(jws.protected_data)) &&
        EVP_DigestVerifyUpdate(verify_context, ".", 1) &&
        EVP_DigestVerifyUpdate(verify_context, jws.payload,
                               strlen(jws.payload)) &&
        EVP_DigestVerifyFinal(verify_context, signature, signature_size) == 1;
    if (okay && size != 0)
        okay = parse(&payload_map, payload, (char *)body);
done:
    EVP_MD_CTX_free(verify_context);
    free(protected_data);
    free(body);
    free(signature);
    lonejson_cleanup(&jws_map, &jws);
    lonejson_cleanup(&protected_map, &header);
    return okay;
}

static int finalize(struct test_ca *ca, const char *encoded)
{
    unsigned char *bytes;
    const unsigned char *cursor;
    size_t size;
    X509_REQ *request;
    EVP_PKEY *key;
    X509 *issued;
    STACK_OF(X509_EXTENSION) * extensions;
    GENERAL_NAMES *names;
    GENERAL_NAME *name;
    X509_EXTENSION *item;
    int i;
    int domain_found;
    int okay;
    if (encoded == NULL)
        return 0;
    bytes = decode(encoded, &size);
    if (bytes == NULL)
        return 0;
    cursor = bytes;
    request = d2i_X509_REQ(NULL, &cursor, (long)size);
    key = request != NULL ? X509_REQ_get_pubkey(request) : NULL;
    okay = key != NULL && cursor == bytes + size &&
           X509_REQ_verify(request, key) == 1 &&
           EVP_PKEY_eq(key, ca->domain) == 1;
    extensions = request != NULL ? X509_REQ_get_extensions(request) : NULL;
    domain_found = 0;
    for (i = 0; extensions != NULL && i < sk_X509_EXTENSION_num(extensions);
         ++i) {
        item = sk_X509_EXTENSION_value(extensions, i);
        if (OBJ_obj2nid(X509_EXTENSION_get_object(item)) !=
            NID_subject_alt_name)
            continue;
        names = X509V3_EXT_d2i(item);
        if (names != NULL && sk_GENERAL_NAME_num(names) == 1) {
            name = sk_GENERAL_NAME_value(names, 0);
            if (name->type == GEN_DNS &&
                ASN1_STRING_length(name->d.dNSName) == 9 &&
                !memcmp(ASN1_STRING_get0_data(name->d.dNSName), "localhost", 9))
                domain_found = 1;
        }
        GENERAL_NAMES_free(names);
    }
    sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
    okay = okay && domain_found;
    issued = okay ? certificate(ca, key, ca->report->orders) : NULL;
    free(ca->issued_pem);
    ca->issued_pem = issued != NULL ? pem(issued, NULL) : NULL;
    X509_free(issued);
    EVP_PKEY_free(key);
    X509_REQ_free(request);
    free(bytes);
    return okay && ca->issued_pem != NULL;
}

static int write_all(SSL *ssl, const char *data, size_t size)
{
    int count;
    while (size != 0) {
        count = SSL_write(ssl, data, (int)size);
        if (count <= 0)
            return 0;
        data += count;
        size -= (size_t)count;
    }
    return 1;
}

static int respond(SSL *ssl, int status, const char *location,
                   const char *nonce, const char *body, int head)
{
    char header[512];
    int length;
    length = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d Test\r\nContent-Length: %lu\r\n"
                      "Content-Type: application/json\r\nConnection: close\r\n"
                      "Replay-Nonce: %s\r\nLocation: %s\r\n\r\n",
                      status, (unsigned long)strlen(body), nonce, location);
    return length > 0 && (size_t)length < sizeof(header) &&
           write_all(ssl, header, (size_t)length) &&
           (head || write_all(ssl, body, strlen(body)));
}

static int request(struct test_ca *ca, SSL *ssl, const char *endpoint,
                   const char *input)
{
    struct payload_doc payload;
    struct identifier *identifier;
    struct timespec delay;
    char *body;
    char *canonical;
    char *thumb;
    char *authorization;
    char location[160];
    unsigned char digest[32];
    int round;
    int okay;
    int status;
    size_t size;
    memset(&payload, 0, sizeof(payload));
    body = malloc(4096);
    if (body == NULL)
        return 0;
    location[0] = '\0';
    body[0] = '\0';
    status = 200;
    okay = 1;
    round = ca->report->orders;
    if (!strcmp(endpoint, "/directory")) {
        (void)__atomic_add_fetch(&ca->report->directories, 1, __ATOMIC_RELEASE);
        if (ca->mode == 4) {
            delay.tv_sec = 2;
            delay.tv_nsec = 0;
            (void)nanosleep(&delay, NULL);
        }
        (void)snprintf(
            body, 4096,
            "{\"newNonce\":\"%s/nonce\",\"newAccount\":\"%s/account\","
            "\"newOrder\":\"%s/new-order\",\"revokeCert\":\"%s/revoke\"}",
            ca->base, ca->base, ca->base, ca->base);
    } else if (!strcmp(endpoint, "/nonce")) {
        round = __atomic_add_fetch(&ca->report->nonces, 1, __ATOMIC_RELAXED);
        canonical = encode((unsigned char *)&round, sizeof(round));
        if (canonical == NULL) {
            okay = 0;
            goto done;
        }
        (void)snprintf(ca->nonce, sizeof(ca->nonce), "%s", canonical);
        free(canonical);
        ca->nonce_used = 0;
    } else {
        okay = verify(ca, endpoint, input, &payload);
        if (!okay)
            goto done;
        if (!strcmp(endpoint, "/account")) {
            if (payload.existing) {
                (void)__atomic_add_fetch(&ca->report->resolves, 1,
                                         __ATOMIC_RELEASE);
                if (!ca->registered) {
                    status = 400;
                    (void)snprintf(body, 4096,
                                   "{\"type\":\"urn:ietf:params:acme:error:%"
                                   "s\"}",
                                   ca->mode == 5 && ca->report->resolves == 1
                                       ? "badNonce"
                                       : "accountDoesNotExist");
                }
            } else {
                (void)__atomic_add_fetch(&ca->report->creates, 1,
                                         __ATOMIC_RELEASE);
                ca->registered = 1;
                status = 201;
            }
            (void)snprintf(location, sizeof(location), "%s/account/1",
                           ca->base);
        } else if (!strcmp(endpoint, "/new-order")) {
            identifier = payload.identifiers.items;
            okay = payload.identifiers.count == 1 &&
                   !strcmp(identifier->type, "dns") &&
                   !strcmp(identifier->value, "localhost");
            round =
                __atomic_add_fetch(&ca->report->orders, 1, __ATOMIC_RELEASE);
            if (!okay || round > 2) {
                okay = 0;
                goto done;
            }
            ca->authorized = 0;
            free(ca->issued_pem);
            ca->issued_pem = NULL;
            (void)snprintf(ca->token, sizeof(ca->token),
                           "Bhw9ciKqH6vNvQwZwciRj%u", (unsigned int)round);
            size = strlen(ca->exponent) + strlen(ca->modulus) + 48;
            canonical = malloc(size);
            if (canonical == NULL) {
                okay = 0;
                goto done;
            }
            (void)snprintf(canonical, size,
                           "{\"e\":\"%s\",\"kty\":\"RSA\",\"n\":\"%s\"}",
                           ca->exponent, ca->modulus);
            (void)SHA256((unsigned char *)canonical, strlen(canonical), digest);
            free(canonical);
            thumb = encode(digest, sizeof(digest));
            size = thumb != NULL ? strlen(thumb) + strlen(ca->token) + 2 : 0;
            authorization = size != 0 ? malloc(size) : NULL;
            if (authorization == NULL) {
                free(thumb);
                okay = 0;
                goto done;
            }
            (void)snprintf(authorization, size, "%s.%s", ca->token, thumb);
            (void)SHA256((unsigned char *)authorization, strlen(authorization),
                         ca->digest);
            free(thumb);
            free(authorization);
            status = 201;
            (void)snprintf(location, sizeof(location), "%s/order/%d", ca->base,
                           round);
            (void)snprintf(body, 4096,
                           "{\"status\":\"pending\",\"authorizations\":[\"%s/"
                           "auth/%d\"],\"finalize\":\"%s/finalize/%d\"}",
                           ca->base, round, ca->base, round);
        } else if (!strncmp(endpoint, "/challenge/", 11)) {
            okay =
                __atomic_load_n(&ca->report->ports_ready, __ATOMIC_ACQUIRE) &&
                test_ca_client(ca, ca->report->ports[0], 0, 0, 1) &&
                test_ca_client(ca, ca->report->ports[1], 0, 0, 1);
            if (!okay)
                goto done;
            ca->authorized = 1;
            (void)__atomic_add_fetch(&ca->report->challenges, 1,
                                     __ATOMIC_RELEASE);
            (void)strcpy(body, "{}");
        } else if (!strncmp(endpoint, "/auth/", 6)) {
            (void)snprintf(
                body, 4096,
                "{\"status\":\"%s\",\"challenges\":[{\"type\":\"tls-alpn-01\","
                "\"url\":\"%s/challenge/"
                "%d\",\"token\":\"%s\",\"status\":\"%s\"}]}",
                ca->authorized ? "valid" : "pending", ca->base, round,
                ca->token, ca->authorized ? "valid" : "pending");
        } else if (!strncmp(endpoint, "/finalize/", 10)) {
            okay = ca->authorized && finalize(ca, payload.csr);
            if (!okay)
                goto done;
            (void)__atomic_add_fetch(&ca->report->finalized, 1,
                                     __ATOMIC_RELEASE);
            (void)strcpy(body, "{}");
        } else if (!strncmp(endpoint, "/order/", 7)) {
            (void)snprintf(body, 4096,
                           "{\"status\":\"%s\",\"certificate\":\"%s/cert/%d\"}",
                           ca->issued_pem != NULL
                               ? "valid"
                               : (ca->authorized ? "ready" : "pending"),
                           ca->base, round);
        } else if (!strncmp(endpoint, "/cert/", 6)) {
            okay = ca->issued_pem != NULL;
            if (!okay)
                goto done;
            (void)snprintf(body, 4096, "%s", ca->issued_pem);
            (void)__atomic_add_fetch(&ca->report->issued, 1, __ATOMIC_RELEASE);
        } else
            okay = 0;
    }
    if (okay) {
        okay = respond(ssl, status, location, ca->nonce, body,
                       !strcmp(endpoint, "/nonce"));
        /* Bound/timeout cases deliberately close the transfer early. */
        if (!okay && (ca->mode == 3 || ca->mode == 4))
            okay = 1;
    }
done:
    lonejson_cleanup(&payload_map, &payload);
    free(body);
    return okay;
}

static void serve(struct test_ca *ca)
{
    SSL *ssl;
    char *buffer;
    char *end;
    char *line;
    char method[8];
    char endpoint[160];
    char *number_end;
    size_t used;
    size_t length;
    size_t body;
    long declared;
    int count;
    int fd;
    struct timeval timeout;
    (void)signal(SIGPIPE, SIG_IGN);
    buffer = malloc(32769);
    if (buffer == NULL)
        _exit(80);
    for (;;) {
        fd = accept(ca->listener, NULL, NULL);
        if (fd < 0 && errno == EINTR)
            continue;
        if (fd < 0)
            _exit(81);
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout));
        ssl = SSL_new(ca->context);
        if (ssl == NULL)
            _exit(82);
        (void)SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) != 1) {
            SSL_free(ssl);
            (void)close(fd);
            continue;
        }
        used = body = length = 0;
        while (used < 32768) {
            count = SSL_read(ssl, buffer + used, (int)(32768 - used));
            if (count <= 0)
                break;
            used += (size_t)count;
            buffer[used] = '\0';
            end = strstr(buffer, "\r\n\r\n");
            if (end == NULL)
                continue;
            body = (size_t)(end + 4 - buffer);
            line = buffer;
            while (line < end) {
                if (!strncasecmp(line, "content-length:", 15)) {
                    declared = strtol(line + 15, &number_end, 10);
                    if (declared < 0 || declared > 32768 ||
                        number_end == line + 15)
                        _exit(83);
                    length = (size_t)declared;
                }
                line = strstr(line, "\r\n");
                if (line == NULL)
                    break;
                line += 2;
            }
            if (used >= body + length)
                break;
        }
        buffer[used] = '\0';
        if (body == 0 || used != body + length ||
            sscanf(buffer, "%7s %159s", method, endpoint) != 2 ||
            (!strcmp(method, "POST") && length == 0) ||
            !request(ca, ssl, endpoint, buffer + body)) {
            (void)fprintf(stderr, "CA fixture rejected %s\n", buffer);
            __atomic_store_n(&ca->report->failed, 1, __ATOMIC_RELEASE);
        }
        (void)SSL_shutdown(ssl);
        SSL_free(ssl);
        (void)close(fd);
    }
}

int test_ca_start(struct test_ca *ca)
{
    ca->pid = fork();
    if (ca->pid == 0)
        serve(ca);
    if (ca->pid < 0)
        return 0;
    (void)close(ca->listener);
    ca->listener = -1;
    return 1;
}
