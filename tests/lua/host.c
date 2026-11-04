#define _POSIX_C_SOURCE 200809L
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#if defined(ROTMOS_PRELOAD_TEST)
#include <rotmos/lua.h>
#endif
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static pid_t child;
struct allocator {
    size_t after;
    int failures;
};
static struct allocator allocation;
static void *allocate(void *data, void *pointer, size_t old_size,
                      size_t new_size)
{
    struct allocator *a = data;
    if (new_size == 0) {
        free(pointer);
        return NULL;
    }
    if (a->failures && (pointer == NULL || new_size > old_size)) {
        if (a->after)
            --a->after;
        else {
            --a->failures;
            return NULL;
        }
    }
    return realloc(pointer, new_size);
}
static int fail_next(lua_State *L)
{
    (void)L;
    allocation.after = 0;
    allocation.failures = 2; /* Lua retries after its emergency collection. */
    return 0;
}
static int fail_call(lua_State *L)
{
    int top = lua_gettop(L), status;
    lua_Integer after = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    allocation.after = (size_t)after;
    allocation.failures = 2;
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
    allocation.failures = 0;
    lua_pushboolean(L, status == LUA_OK);
    lua_insert(L, top + 1);
    return lua_gettop(L) - top;
}
static int encrypted;
static const char *required_header;
static const char *forwarded_address = "198.51.100.7";
static SSL_CTX *client_context;
static int exchange(unsigned short port, const char *method, const char *path,
                    const void *body, size_t length, int status,
                    const void *expected, size_t expected_length)
{
    int fd = -1, failed = 1;
    SSL *ssl = NULL;
    struct sockaddr_in address;
    struct timeval timeout;
    char head[1024], prefix[32], *reply = NULL, *payload, *cursor, *end,
                                 *decoded = NULL;
    size_t used = 0, n = 0, chunk;
    ssize_t count;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
        goto done;
    if (encrypted) {
        ssl = SSL_new(client_context);
        if (ssl == NULL || !SSL_set_fd(ssl, fd) ||
            !SSL_set_tlsext_host_name(ssl, "localhost") ||
            !SSL_set1_host(ssl, "localhost") || SSL_connect(ssl) != 1)
            goto done;
    }
    snprintf(head, sizeof(head),
             "%s %s HTTP/1.1\r\nHost: localhost\r\nX-Test: value\r\n"
             "Authorization: Bearer fixture-token\r\nX-Forwarded-For: %s\r\n"
             "Content-Length: %lu\r\nConnection: close\r\n\r\n",
             method, path, forwarded_address, (unsigned long)length);
    count = ssl ? SSL_write(ssl, head, (int)strlen(head))
                : write(fd, head, strlen(head));
    if (count != (ssize_t)strlen(head))
        goto done;
    if (length) {
        count =
            ssl ? SSL_write(ssl, body, (int)length) : write(fd, body, length);
        if (count != (ssize_t)length)
            goto done;
    }
    reply = malloc(65536);
    decoded = malloc(65536);
    if (reply == NULL || decoded == NULL)
        goto done;
    while (used < 65535) {
        count = ssl ? SSL_read(ssl, reply + used, (int)(65535 - used))
                    : read(fd, reply + used, 65535 - used);
        if (count <= 0)
            break;
        used += (size_t)count;
    }
    reply[used] = 0;
    snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d ", status);
    payload = strstr(reply, "\r\n\r\n");
    if (strncmp(reply, prefix, strlen(prefix)) || payload == NULL ||
        (required_header && strstr(reply, required_header) == NULL)) {
        fprintf(stderr, "%s: %s\n", path, reply);
        goto done;
    }
    payload += 4;
    if (strstr(reply, "transfer-encoding: chunked")) {
        cursor = payload;
        while (cursor < reply + used) {
            chunk = strtoul(cursor, &end, 16);
            if (end == cursor || strncmp(end, "\r\n", 2))
                goto done;
            cursor = end + 2;
            if (chunk == 0)
                break;
            if (chunk > 65535 - n || cursor + chunk + 2 > reply + used)
                goto done;
            memcpy(decoded + n, cursor, chunk);
            n += chunk;
            cursor += chunk + 2;
        }
    } else {
        n = used - (size_t)(payload - reply);
        memcpy(decoded, payload, n);
    }
    if (n != expected_length || memcmp(decoded, expected, expected_length)) {
        fprintf(stderr, "%s body length %lu expected %lu\n", path,
                (unsigned long)n, (unsigned long)expected_length);
        goto done;
    }
    failed = 0;
done:
    SSL_free(ssl);
    if (fd >= 0)
        close(fd);
    free(reply);
    free(decoded);
    return failed;
}
static int drive(unsigned short port)
{
    char *stream = malloc(1003);
    char *spooled = malloc(20004);
    int failed = 0;
    if (stream == NULL || spooled == NULL) {
        free(stream);
        free(spooled);
        return 1;
    }
    memset(stream, 's', 1000);
    memcpy(stream + 1000, "123", 3);
    memset(spooled, 'p', 20000);
    spooled[20000] = 0;
    memcpy(spooled + 20001, "123", 3);
#define REQ(m, p, b, l, s, e, n)                                               \
    do {                                                                       \
        if (exchange(port, m, p, b, l, s, e, n)) {                             \
            failed = 1;                                                        \
            goto done;                                                         \
        }                                                                      \
    } while (0)
    REQ("GET", "/text", "", 0, 200, "hello", 5);
    REQ("GET", "/nil", "", 0, 204, "", 0);
    REQ("POST", "/echo", "A\0B", 3, 201, "A\0B", 3);
    REQ("GET", "/group/value/42?x=&x=two", "", 0, 200, "42", 2);
    REQ("GET", "/before", "", 0, 403, "blocked", 7);
    REQ("GET", "/after", "", 0, 202, "changed", 7);
    REQ("GET", "/throw", "", 0, 500, "", 0);
    REQ("GET", "/stream", "", 0, 200, stream, 1003);
    REQ("GET", "/source-error", "", 0, 500, "", 0);
    required_header = "content-length: 20004\r\n";
    REQ("GET", "/spooled", "", 0, 200, spooled, 20004);
    REQ("HEAD", "/spooled", "", 0, 200, "", 0);
    required_header = NULL;
    REQ("GET", "/spooled-error", "", 0, 500, "", 0);
    REQ("GET", "/spooled-oom", "", 0, 500, "", 0);
    REQ("GET", "/spooled-close-error", "", 0, 200, "", 0);
    REQ("GET", "/spooled-strict", "", 0, 200, "strict", 6);
    REQ("GET", "/spooled-paused", "", 0, 501, "", 0);
    REQ("GET", "/spooled-discard", "", 0, 200, "discarded", 9);
    REQ("GET", "/getter-error", "", 0, 500, "", 0);
    REQ("GET", "/oom-reply", "", 0, 500, "", 0);
    REQ("GET", "/oom-source", "", 0, 500, "", 0);
    REQ("GET", "/discard", "", 0, 203, "replacement", 11);
    REQ("GET", "/table-group/entry", "", 0, 200, "default", 7);
    REQ("POST", "/table-group/other", "", 0, 200, "override", 8);
    REQ("GET", "/verify", "", 0, 200, "verified", 8);
    REQ("GET", "/stop", "", 0, 200, "stopped", 7);
done:
    free(stream);
    free(spooled);
    return failed;
}
static int spawn(lua_State *L)
{
    int status;
    unsigned short port = (unsigned short)lua_tointeger(L, 1);
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        status = drive(port);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int join(lua_State *L)
{
    int status;
    if (child <= 0 || waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return luaL_error(L, "HTTP driver failed");
    child = 0;
    lua_pushboolean(L, 1);
    return 1;
}
static int drive_policy(unsigned short http, unsigned short tls,
                        unsigned short redirect)
{
    int failed;
    encrypted = 1;
    required_header = NULL;
    failed = exchange(tls, "GET", "/tls", "", 0, 200, "tls", 3);
    encrypted = 0;
    required_header = "location: https://localhost:9443/raw%2Fpath?x=%2F\r\n";
    if (!failed)
        failed =
            exchange(redirect, "GET", "/raw%2Fpath?x=%2F", "", 0, 308, "", 0);
    required_header = "cache-control: no-store\r\n";
    if (!failed)
        failed = exchange(http, "GET", "/allow", "", 0, 200, "allowed", 7);
    if (!failed)
        failed =
            exchange(http, "GET", "/principal", "", 0, 200, "principal", 9);
    required_header = "www-authenticate: Basic realm=\"fixture\"\r\n";
    if (!failed)
        failed = exchange(http, "GET", "/required", "", 0, 401,
                          "authentication required\n", 24);
    required_header = "location: /login\r\n";
    if (!failed)
        failed = exchange(http, "GET", "/redirect", "", 0, 302,
                          "authentication required\n", 24);
    required_header = NULL;
    if (!failed)
        failed = exchange(http, "GET", "/deny", "", 0, 403, "A\0B", 3);
    if (!failed)
        failed = exchange(http, "GET", "/auth-error", "", 0, 500, "", 0);
    if (!failed)
        failed = exchange(http, "GET", "/auth-invalid", "", 0, 400, "", 0);
    if (!failed)
        failed = exchange(http, "GET", "/auth-getter", "", 0, 500, "", 0);
    if (!failed)
        failed = exchange(http, "GET", "/auth-oom", "", 0, 500, "", 0);
    if (!failed)
        failed = exchange(http, "GET", "/guard/entry", "", 0, 200, "group", 5);
    if (!failed)
        failed = exchange(http, "GET", "/event", "", 0, 200, "event", 5);
    if (!failed)
        failed = exchange(http, "GET", "/event", "", 0, 200, "event", 5);
    if (!failed)
        failed = exchange(http, "GET", "/blocked", "", 0, 403, "", 0);
    forwarded_address = "203.0.113.9";
    if (!failed)
        failed = exchange(http, "GET", "/stop", "", 0, 200, "stopped", 7);
    return failed;
}
static int spawn_policy(lua_State *L)
{
    unsigned short http = (unsigned short)lua_tointeger(L, 1);
    unsigned short tls = (unsigned short)lua_tointeger(L, 2);
    unsigned short redirect = (unsigned short)lua_tointeger(L, 3);
    int status;
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        status = drive_policy(http, tls, redirect);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int extension(X509 *certificate, X509 *issuer, int nid, const char *text)
{
    X509V3_CTX context;
    X509_EXTENSION *item;
    int ok;
    X509V3_set_ctx(&context, issuer, certificate, NULL, NULL, 0);
    item = X509V3_EXT_conf_nid(NULL, &context, nid, text);
    if (item == NULL)
        return 0;
    ok = X509_add_ext(certificate, item, -1);
    X509_EXTENSION_free(item);
    return ok;
}
static X509 *certificate(EVP_PKEY *key, X509 *ca, EVP_PKEY *signer)
{
    X509 *out = X509_new();
    X509_NAME *name;
    if (out == NULL)
        return NULL;
    name = X509_get_subject_name(out);
    if (!X509_set_version(out, 2) ||
        !ASN1_INTEGER_set(X509_get_serialNumber(out), ca ? 2 : 1) ||
        X509_gmtime_adj(X509_getm_notBefore(out), -60) == NULL ||
        X509_gmtime_adj(X509_getm_notAfter(out), 3600) == NULL ||
        !X509_set_pubkey(out, key) ||
        !X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            (const unsigned char *)(ca ? "localhost" : "Lua fixture CA"), -1,
            -1, 0) ||
        !X509_set_issuer_name(out, ca ? X509_get_subject_name(ca) : name) ||
        !extension(out, ca ? ca : out, NID_basic_constraints,
                   ca ? "critical,CA:FALSE" : "critical,CA:TRUE") ||
        !extension(out, ca ? ca : out, NID_key_usage,
                   ca ? "critical,digitalSignature"
                      : "critical,keyCertSign,cRLSign") ||
        (ca &&
         (!extension(out, ca, NID_ext_key_usage, "serverAuth,clientAuth") ||
          !extension(out, ca, NID_subject_alt_name,
                     "DNS:localhost,DNS:bundle.local"))) ||
        !X509_sign(out, signer, NULL)) {
        X509_free(out);
        return NULL;
    }
    return out;
}
struct tls_material {
    BIO *certificate, *private_key, *ca;
    const char *path;
};
static void pem_field(lua_State *L, const char *name, BIO *pem)
{
    char *bytes;
    long length = BIO_get_mem_data(pem, &bytes);
    lua_pushlstring(L, bytes, (size_t)length);
    lua_setfield(L, -2, name);
}
static int publish_material(lua_State *L)
{
    struct tls_material *material = lua_touserdata(L, 1);
    lua_newtable(L);
    pem_field(L, "certificate", material->certificate);
    pem_field(L, "private_key", material->private_key);
    pem_field(L, "ca", material->ca);
    lua_pushstring(L, material->path);
    lua_setfield(L, -2, "private_key_path");
    return 1;
}
static int tls_setup(lua_State *L)
{
    struct tls_material material;
    EVP_PKEY *key = NULL, *signer = NULL;
    X509 *ca = NULL, *cert = NULL;
    SSL_CTX *context = NULL;
    FILE *file = NULL;
    int fd = -1, ok = 0, status = LUA_OK;
    memset(&material, 0, sizeof(material));
    material.path = luaL_checkstring(L, 1);
    signer = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
    key = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
    if (signer == NULL || key == NULL ||
        (ca = certificate(signer, NULL, signer)) == NULL ||
        (cert = certificate(key, ca, signer)) == NULL)
        goto done;
    material.certificate = BIO_new(BIO_s_mem());
    material.private_key = BIO_new(BIO_s_mem());
    material.ca = BIO_new(BIO_s_mem());
    context = SSL_CTX_new(TLS_client_method());
    if (material.certificate == NULL || material.private_key == NULL ||
        material.ca == NULL || context == NULL ||
        !PEM_write_bio_X509(material.certificate, cert) ||
        !PEM_write_bio_X509(material.ca, ca) ||
        !PEM_write_bio_PrivateKey(material.private_key, key, NULL, NULL, 0,
                                  NULL, NULL) ||
        !SSL_CTX_use_certificate(context, cert) ||
        !SSL_CTX_use_PrivateKey(context, key) ||
        !X509_STORE_add_cert(SSL_CTX_get_cert_store(context), ca))
        goto done;
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
    fd = open(material.path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        goto done;
    file = fdopen(fd, "w");
    if (file == NULL)
        goto done;
    fd = -1;
    if (!PEM_write_PrivateKey(file, key, NULL, NULL, 0, NULL, NULL))
        goto done;
    if (fclose(file) != 0) {
        file = NULL;
        goto done;
    }
    file = NULL;
    SSL_CTX_free(client_context);
    client_context = context;
    context = NULL;
    lua_pushcfunction(L, publish_material);
    lua_pushlightuserdata(L, &material);
    status = lua_pcall(L, 1, 1, 0);
    ok = 1;
done:
    if (file != NULL)
        fclose(file);
    if (fd >= 0)
        close(fd);
    SSL_CTX_free(context);
    X509_free(cert);
    X509_free(ca);
    EVP_PKEY_free(key);
    EVP_PKEY_free(signer);
    BIO_free(material.certificate);
    BIO_free(material.private_key);
    BIO_free(material.ca);
    if (!ok)
        return luaL_error(L, "TLS fixture preparation failed");
    if (status != LUA_OK)
        return lua_error(L);
    return 1;
}
#if defined(ROTMOS_PRELOAD_TEST)
static int conflict(lua_State *L)
{
    (void)L;
    return 0;
}
#endif
int main(int argc, char **argv)
{
    lua_State *L;
    int status;
#if defined(ROTMOS_PRELOAD_TEST)
    rotmos_error error;
#endif
    if (argc != 3)
        return 2;
    L = lua_newstate(allocate, &allocation, 0);
    if (L == NULL)
        return 2;
#if defined(ROTMOS_PRELOAD_TEST)
    if (rotmos_lua_preload(L, &error) != ROTMOS_STATE)
        return 3;
#endif
    luaL_openlibs(L);
#if defined(ROTMOS_PRELOAD_TEST)
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setglobal(L, "string");
    if (rotmos_lua_preload(L, &error) != ROTMOS_STATE)
        return 6;
    lua_setglobal(L, "string");
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, conflict);
    lua_setfield(L, -2, "rotmos");
    lua_settop(L, 0);
    if (rotmos_lua_preload(L, &error) != ROTMOS_CONFLICT)
        return 4;
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushnil(L);
    lua_setfield(L, -2, "rotmos");
    lua_settop(L, 0);
    if (rotmos_lua_preload(L, &error) != ROTMOS_OK ||
        rotmos_lua_preload(L, &error) != ROTMOS_OK)
        return 5;
#else
    lua_getglobal(L, "package");
    lua_pushfstring(L, "%s/share/lua/5.5/?.lua", argv[2]);
    lua_setfield(L, -2, "path");
    lua_pushfstring(L, "%s/lib/lua/5.5/?.so", argv[2]);
    lua_setfield(L, -2, "cpath");
    lua_pop(L, 1);
#endif
    lua_newtable(L);
    lua_pushcfunction(L, spawn);
    lua_setfield(L, -2, "spawn");
    lua_pushcfunction(L, join);
    lua_setfield(L, -2, "join");
    lua_pushcfunction(L, fail_next);
    lua_setfield(L, -2, "fail_next");
    lua_pushcfunction(L, fail_call);
    lua_setfield(L, -2, "fail_call");
    lua_pushcfunction(L, tls_setup);
    lua_setfield(L, -2, "tls_setup");
    lua_pushcfunction(L, spawn_policy);
    lua_setfield(L, -2, "spawn_policy");
    lua_pushstring(L, argv[2]);
    lua_setfield(L, -2, "root");
    lua_setglobal(L, "fixture");
    lua_pushstring(L, "preserved");
    lua_setglobal(L, "host_marker");
    status = luaL_loadfilex(L, argv[1], "t");
    if (status == LUA_OK)
        status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK)
        fprintf(stderr, "Lua fixture: %s\n", lua_tostring(L, -1));
    lua_close(L);
    SSL_CTX_free(client_context);
    return status != LUA_OK;
}
