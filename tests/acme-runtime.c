#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include "acme-backend.h"

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            (void)fprintf(stderr, "ACME runtime line %d: %s\n", __LINE__,      \
                          #test);                                              \
            return 1;                                                          \
        }                                                                      \
    } while (0)

struct fixture {
    rotmos_server *server;
    EVP_PKEY *key;
    X509 *certificates[2];
    char *key_pem;
    char *pem[2];
    char *root;
    pid_t parent;
    pid_t client;
    unsigned short ports[3];
    int command[2];
    int notes[2];
    int mode;
    int phase;
    int restores;
    int persisted;
    int errors;
    int restarts;
    int escalated;
    int observed_failure;
    int observed_pending;
    int observed_usable;
    int failure;
};

static char *path(const char *directory, const char *leaf)
{
    char *result;
    size_t size;
    size = strlen(directory) + strlen(leaf) + 2;
    result = malloc(size);
    if (result != NULL)
        (void)snprintf(result, size, "%s/%s", directory, leaf);
    return result;
}

static int text(const char *directory, const char *leaf, const char *value)
{
    char *name;
    FILE *file;
    int okay;
    name = path(directory, leaf);
    if (name == NULL)
        return 0;
    file = fopen(name, "w");
    free(name);
    okay = file != NULL && fputs(value, file) >= 0;
    if (file != NULL && fclose(file) != 0)
        okay = 0;
    return okay;
}

static int remove_tree(const char *directory)
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
        name = path(directory, entry->d_name);
        if (name == NULL || lstat(name, &info) != 0)
            okay = 0;
        else if (S_ISDIR(info.st_mode)) {
            if (!remove_tree(name))
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
    char *data;
    char *result;
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

static X509 *certificate(EVP_PKEY *key, long serial)
{
    X509 *result;
    X509_NAME *name;
    X509_EXTENSION *extension;
    result = X509_new();
    if (result == NULL)
        return NULL;
    name = X509_get_subject_name(result);
    extension = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name,
                                    "DNS:localhost,DNS:manual.local");
    if (extension == NULL || !X509_set_version(result, 2) ||
        !ASN1_INTEGER_set(X509_get_serialNumber(result), serial) ||
        X509_gmtime_adj(X509_getm_notBefore(result), -60) == NULL ||
        X509_gmtime_adj(X509_getm_notAfter(result), 86400 * 90L) == NULL ||
        !X509_set_pubkey(result, key) ||
        !X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    (const unsigned char *)"localhost", -1, -1,
                                    0) ||
        !X509_set_issuer_name(result, name) ||
        !X509_add_ext(result, extension, -1) ||
        !X509_sign(result, key, EVP_sha256())) {
        X509_free(result);
        result = NULL;
    }
    X509_EXTENSION_free(extension);
    return result;
}

static void note(struct fixture *fixture, char byte)
{
    if (write(fixture->notes[1], &byte, 1) != 1)
        _exit(82);
}

static rotmos_status state(rotmos_server *server,
                           const rotmos_process_context *process,
                           rotmos_acme_event event, rotmos_bytes domain,
                           rotmos_bytes directory, void *userdata,
                           rotmos_error *error)
{
    struct fixture *fixture;
    char *name;
    struct timespec delay;
    (void)server;
    fixture = userdata;
    if (process->process_id != (long)getpid() || directory.length == 0)
        return ROTMOS_INVALID;
    if (event == ROTMOS_ACME_ERROR) {
        if (process->role != ROTMOS_PROCESS_KEY_MANAGER ||
            error->status != ROTMOS_IO || domain.length != 9) {
            (void)fprintf(stderr, "unexpected ACME error role %d: %d %s: %s\n",
                          process->role, error->status, error->operation,
                          error->message);
            _exit(83);
        }
        note(fixture, 'E');
        return ROTMOS_OK;
    }
    if (event == ROTMOS_ACME_RESTORE) {
        if (process->role != ROTMOS_PROCESS_PARENT ||
            process->process_id != (long)fixture->parent)
            return ROTMOS_INVALID;
        ++fixture->restores;
        name = path(directory.data, "certificates");
        if (name == NULL || (mkdir(name, 0700) != 0 && errno != EEXIST))
            return ROTMOS_IO;
        free(name);
        name = path(directory.data, "certificates/localhost");
        if (name == NULL || (mkdir(name, 0700) != 0 && errno != EEXIST))
            return ROTMOS_IO;
        free(name);
        if (!text(directory.data, "account-key.pem", fixture->key_pem) ||
            !text(directory.data, "certificates/localhost/key.pem",
                  fixture->key_pem) ||
            (fixture->mode != 3 &&
             !text(directory.data, "certificates/localhost/fullchain.pem",
                   fixture->pem[0])))
            return ROTMOS_IO;
        return ROTMOS_OK;
    }
    if (process->role != ROTMOS_PROCESS_KEY_MANAGER ||
        process->process_id == (long)fixture->parent)
        return ROTMOS_INVALID;
    if (domain.length != 0 && fixture->mode == 1) {
        rotmos_error_clear(error);
        error->status = ROTMOS_IO;
        (void)strcpy(error->operation, "fixture_persist");
        (void)strcpy(error->message, "fixture refused candidate");
        return ROTMOS_IO;
    }
    if (domain.length != 0 && fixture->mode == 2) {
        delay.tv_sec = 2;
        delay.tv_nsec = 0;
        (void)nanosleep(&delay, NULL);
        _exit(84);
    }
    note(fixture, domain.length != 0 ? 'P' : 'S');
    return ROTMOS_OK;
}

static void command(rotmos_watch *watch, unsigned int events, void *userdata)
{
    struct fixture *fixture;
    char byte;
    unsigned char digest[32];
    (void)watch;
    (void)events;
    fixture = userdata;
    memset(digest, 42, sizeof(digest));
    if (read(fixture->command[0], &byte, 1) != 1 ||
        !(byte == 'C' ? rotmos_test_acme_send(digest, sizeof(digest), 1)
                      : rotmos_test_acme_send(fixture->pem[1],
                                              strlen(fixture->pem[1]), 0)))
        _exit(85);
}

static rotmos_status process_start(rotmos_server *server,
                                   const rotmos_process_context *process,
                                   void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    rotmos_watch *watch;
    fixture = userdata;
    if (process->role != ROTMOS_PROCESS_ACME)
        return ROTMOS_OK;
    return rotmos_loop_watch(rotmos_server_loop(server), fixture->command[0],
                             ROTMOS_READABLE, command, NULL, fixture, &watch,
                             error);
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

static int exchange(struct fixture *fixture, size_t listener, int stage)
{
    static const unsigned char alpn[] = {10,  'a', 'c', 'm', 'e', '-',
                                         't', 'l', 's', '/', '1'};
    static const char *get =
        "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    SSL_CTX *ctx;
    SSL *ssl;
    X509 *peer;
    ASN1_OBJECT *oid;
    X509_EXTENSION *extension;
    ASN1_OCTET_STRING *value;
    const unsigned char *selected;
    unsigned int selected_length;
    int fd;
    int okay;
    int count;
    int index;
    size_t used;
    long serial;
    const char *sni;
    char response[1024];
    fd = connect_to(fixture->ports[listener]);
    if (fd < 0)
        return 0;
    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        (void)close(fd);
        return 0;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    if (stage != 2 && stage != 4) {
        (void)SSL_CTX_use_certificate(ctx, fixture->certificates[0]);
        (void)SSL_CTX_use_PrivateKey(ctx, fixture->key);
    }
    (void)SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    (void)SSL_CTX_set_max_proto_version(ctx, listener == 1 ? TLS1_3_VERSION
                                                           : TLS1_2_VERSION);
    ssl = SSL_new(ctx);
    okay = ssl != NULL;
    if (okay) {
        (void)SSL_set_fd(ssl, fd);
        sni = listener == 2 ? "manual.local" : "localhost";
        (void)SSL_set_tlsext_host_name(ssl, sni);
        if (stage == 2)
            (void)SSL_set_alpn_protos(ssl, alpn, sizeof(alpn));
        okay = SSL_connect(ssl) == 1;
    }
    peer = okay ? SSL_get1_peer_certificate(ssl) : NULL;
    okay = okay && peer != NULL;
    if (stage == 4)
        goto done;
    okay = okay && SSL_version(ssl) ==
                       (listener == 1 ? TLS1_3_VERSION : TLS1_2_VERSION);
    if (okay && stage == 2) {
        SSL_get0_alpn_selected(ssl, &selected, &selected_length);
        oid = OBJ_txt2obj("1.3.6.1.5.5.7.1.31", 1);
        index = X509_get_ext_by_OBJ(peer, oid, -1);
        ASN1_OBJECT_free(oid);
        extension = index >= 0 ? X509_get_ext(peer, index) : NULL;
        value = extension != NULL ? X509_EXTENSION_get_data(extension) : NULL;
        okay = selected_length == 10 && !memcmp(selected, alpn + 1, 10) &&
               extension != NULL && X509_EXTENSION_get_critical(extension) &&
               ASN1_STRING_length(value) == 34 &&
               ASN1_STRING_get0_data(value)[2] == 42;
    } else if (okay) {
        serial = stage == 3 && fixture->mode != 1 && listener != 2 ? 2 : 1;
        okay = ASN1_INTEGER_get(X509_get_serialNumber(peer)) == serial;
        get = listener == 2 ? "GET / HTTP/1.1\r\nHost: "
                              "manual.local\r\nConnection: close\r\n\r\n"
                            : "GET / HTTP/1.1\r\nHost: "
                              "localhost\r\nConnection: close\r\n\r\n";
        if (okay)
            okay = SSL_write(ssl, get, (int)strlen(get)) == (int)strlen(get);
        used = 0;
        while (okay && used < sizeof(response) - 1) {
            count = SSL_read(ssl, response + used,
                             (int)(sizeof(response) - used - 1));
            if (count <= 0)
                break;
            used += (size_t)count;
        }
        response[used] = '\0';
        okay = okay && strstr(response, stage == 0 ? "503 " : "200 ") != NULL;
    }
done:
    X509_free(peer);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    (void)close(fd);
    return okay;
}

static void client(struct fixture *fixture, int stage)
{
    size_t i;
    int attempt;
    int okay;
    struct timespec delay;
    fixture->client = fork();
    if (fixture->client != 0)
        return;
    (void)signal(SIGPIPE, SIG_IGN);
    delay.tv_sec = 0;
    delay.tv_nsec = 2000000;
    okay = 1;
    if (stage == 1 && exchange(fixture, 0, 4))
        okay = 0;
    for (i = 0; i < (stage == 2 ? 2UL : 3UL); ++i) {
        for (attempt = 0; attempt < 150; ++attempt) {
            if (exchange(fixture, i, stage))
                break;
            (void)nanosleep(&delay, NULL);
        }
        if (attempt == 150)
            okay = 0;
    }
    note(fixture, okay ? (char)('A' + stage) : 'F');
    _exit(okay ? 0 : 86);
}

static void timed_out(rotmos_timer *timer, void *userdata)
{
    struct fixture *fixture;
    (void)timer;
    fixture = userdata;
    fixture->failure = 1;
    (void)rotmos_server_request_stop(fixture->server, NULL);
}

static void completed(rotmos_watch *watch, unsigned int events, void *userdata)
{
    struct fixture *fixture;
    unsigned char bytes[32];
    ssize_t count;
    size_t i;
    char command_byte;
    int status;
    (void)watch;
    (void)events;
    fixture = userdata;
    count = read(fixture->notes[0], bytes, sizeof(bytes));
    for (i = 0; count > 0 && i < (size_t)count; ++i) {
        switch (bytes[i]) {
        case 'S':
            break;
        case 'P':
            ++fixture->persisted;
            if (fixture->phase == 3 && fixture->mode == 0) {
                fixture->phase = 4;
                client(fixture, 3);
            }
            break;
        case 'E':
            ++fixture->errors;
            break;
        case 'A':
        case 'B':
        case 'C':
        case 'D':
            if (waitpid(fixture->client, &status, 0) != fixture->client ||
                !WIFEXITED(status) || WEXITSTATUS(status))
                fixture->failure = 1;
            fixture->client = 0;
            if (bytes[i] == 'A') {
                (void)rotmos_server_set_serving(fixture->server, 1, NULL);
                client(fixture, 1);
            } else if (bytes[i] == 'B') {
                fixture->phase = 2;
                command_byte = 'C';
                (void)write(fixture->command[1], &command_byte, 1);
                client(fixture, 2);
            } else if (bytes[i] == 'C') {
                fixture->phase = 3;
                (void)rotmos_server_set_serving(fixture->server, 1, NULL);
                command_byte = 'I';
                (void)write(fixture->command[1], &command_byte, 1);
            } else
                (void)rotmos_server_request_stop(fixture->server, NULL);
            break;
        default:
            fixture->failure = 1;
            (void)rotmos_server_request_stop(fixture->server, NULL);
            break;
        }
    }
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process,
                           void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    rotmos_bound_listener listener;
    rotmos_timer *timer;
    rotmos_watch *watch;
    char byte;
    size_t i;
    (void)process;
    fixture = userdata;
    for (i = 0; i < 3; ++i) {
        if (rotmos_server_listener_at(server, i, &listener, error) !=
                ROTMOS_OK ||
            !listener.tls)
            return ROTMOS_INVALID;
        fixture->ports[i] = listener.port;
    }
    if (rotmos_loop_timer(rotmos_server_loop(server), 4000, 0, timed_out, NULL,
                          fixture, &timer, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    if (rotmos_loop_watch(rotmos_server_loop(server), fixture->notes[0],
                          ROTMOS_READABLE, completed, NULL, fixture, &watch,
                          error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    if (fixture->mode == 3) {
        fixture->phase = 2;
        byte = 'C';
        (void)write(fixture->command[1], &byte, 1);
        client(fixture, 2);
    }
    return ROTMOS_OK;
}

static rotmos_status unbound_ready(rotmos_server *server,
                                   const rotmos_process_context *process,
                                   void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    rotmos_tls_identity_state current;
    (void)process;
    fixture = userdata;
    if (rotmos_server_tls_identity_state(server, 0, &current, error) !=
            ROTMOS_OK ||
        current != ROTMOS_TLS_PENDING)
        return ROTMOS_INVALID;
    fixture->phase = 4;
    return rotmos_server_request_stop(server, error);
}

static void identity_changed(rotmos_server *server,
                             const rotmos_tls_identity_event *event,
                             void *userdata)
{
    struct fixture *fixture;
    rotmos_tls_identity_state current;
    fixture = userdata;
    if (rotmos_server_tls_identity_state(server, event->identity_index,
                                         &current, NULL) != ROTMOS_OK ||
        current != event->state)
        fixture->failure = 1;
    if (event->error.status != ROTMOS_OK) {
        if (event->identity_index != 0 || event->state != ROTMOS_TLS_USABLE ||
            event->domain.length != 9 ||
            event->error.status !=
                (fixture->mode == 1 ? ROTMOS_IO : ROTMOS_TIMEOUT))
            fixture->failure = 1;
        ++fixture->observed_failure;
    }
    if (event->identity_index == 0) {
        if (event->state == ROTMOS_TLS_PENDING)
            ++fixture->observed_pending;
        else if (event->error.status == ROTMOS_OK)
            ++fixture->observed_usable;
    }
    if (event->identity_index == 0 && event->state == ROTMOS_TLS_USABLE &&
        fixture->phase == 0) {
        fixture->phase = 1;
        client(fixture, 0);
    }
    if (fixture->mode == 3 && fixture->phase == 3 &&
        event->identity_index == 0 && event->state == ROTMOS_TLS_USABLE) {
        fixture->phase = 4;
        client(fixture, 3);
    }
}

static void worker_event(rotmos_server *server,
                         const rotmos_worker_event *event, void *userdata)
{
    struct fixture *fixture;
    (void)server;
    fixture = userdata;
    if (event->process.role != ROTMOS_PROCESS_KEY_MANAGER)
        return;
    if (event->kind == ROTMOS_WORKER_ESCALATED)
        ++fixture->escalated;
    if (event->kind == ROTMOS_WORKER_RESTARTED) {
        ++fixture->restarts;
        if (fixture->phase != 3)
            fixture->failure = 1;
        fixture->phase = 4;
        client(fixture, 3);
    }
}

static rotmos_status handler(rotmos_request *request, rotmos_reply *reply,
                             void *userdata, rotmos_error *error)
{
    (void)request;
    (void)userdata;
    return rotmos_reply_text(reply, 200, "text/plain", "ACME runtime", error);
}

int main(int argc, char **argv)
{
    const char *domains[3] = {"localhost", "manual.local", "localhost"};
    struct fixture *fixture;
    rotmos_server_config config;
    rotmos_listener_config listeners[3];
    rotmos_tls_identity identities[2];
    rotmos_route_config route;
    rotmos_run_result result;
    rotmos_error error;
    rotmos_tls_identity_state current;
    rotmos_status status;
    rotmos_server *invalid;
    char *directory;
    char *private_directory;
    char *ca_file;
    int descriptor;
    size_t i;
    fixture = calloc(1, sizeof(*fixture));
    CHECK(fixture != NULL);
    fixture->mode = argc <= 1
                        ? 0
                        : (!strcmp(argv[1], "fail")
                               ? 1
                               : (!strcmp(argv[1], "timeout")
                                      ? 2
                                      : (!strcmp(argv[1], "pending") ? 3 : 4)));
    fixture->parent = getpid();
    directory = malloc(64);
    CHECK(directory != NULL);
    (void)strcpy(directory, "rotmos-acme-runtime-XXXXXX");
    CHECK(mkdtemp(directory) != NULL);
    fixture->root = realpath(directory, NULL);
    CHECK(fixture->root != NULL);
    fixture->key = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", 2048);
    CHECK(fixture->key != NULL);
    fixture->key_pem = pem(NULL, fixture->key);
    for (i = 0; i < 2; ++i) {
        fixture->certificates[i] = certificate(fixture->key, (long)i + 1);
        CHECK(fixture->certificates[i] != NULL);
        fixture->pem[i] = pem(fixture->certificates[i], NULL);
        CHECK(fixture->pem[i] != NULL);
    }
    CHECK(fixture->key_pem != NULL && pipe(fixture->notes) == 0 &&
          pipe(fixture->command) == 0);
    CHECK(fcntl(fixture->notes[0], F_SETFL, O_NONBLOCK) == 0 &&
          fcntl(fixture->command[0], F_SETFL, O_NONBLOCK) == 0);
    rotmos_server_config_init(&config);
    for (i = 0; i < 3; ++i) {
        rotmos_listener_config_init(&listeners[i]);
        listeners[i].bind = "127.0.0.1";
        listeners[i].kind = ROTMOS_LISTENER_HTTPS;
        listeners[i].identity_index = i == 2 ? 1 : 0;
    }
    for (i = 0; i < 2; ++i) {
        rotmos_tls_identity_init(&identities[i]);
        identities[i].domains = &domains[i];
        identities[i].domain_count = 1;
    }
    identities[0].mode = ROTMOS_TLS_ACME;
    identities[0].client_ca.kind = ROTMOS_MATERIAL_MEMORY;
    identities[0].client_ca.bytes.data = fixture->pem[0];
    identities[0].client_ca.bytes.length = strlen(fixture->pem[0]);
    identities[0].require_client_certificate = 1;
    identities[1].mode = ROTMOS_TLS_MANUAL;
    identities[1].domain_count = 2;
    identities[1].certificate.kind = ROTMOS_MATERIAL_MEMORY;
    identities[1].certificate.bytes.data = fixture->pem[0];
    identities[1].certificate.bytes.length = strlen(fixture->pem[0]);
    identities[1].private_key.kind = ROTMOS_MATERIAL_MEMORY;
    identities[1].private_key.bytes.data = fixture->key_pem;
    identities[1].private_key.bytes.length = strlen(fixture->key_pem);
    config.listeners = listeners;
    config.listener_count = 3;
    config.workers.count = 2;
    config.workers.affinity_disabled = 1;
    config.workers.shutdown_timeout_ms = 500;
    config.timeouts.startup.milliseconds = 1500;
    config.http.backend_quiet = 1;
    config.http.backend_curl_timeout_seconds = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.start_paused = 1;
    config.tls.identities = identities;
    config.tls.identity_count = 2;
    config.tls.acme.email = "fixture@example.test";
    config.tls.acme.directory_url = "http://127.0.0.1:1/directory";
    config.tls.acme.runtime_directory = fixture->root;
    config.tls.acme.persist_timeout_ms = fixture->mode == 2 ? 250 : 1000;
    config.tls.acme.state = state;
    config.tls.acme.userdata = fixture;
    config.callbacks.on_process_start = process_start;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_event = worker_event;
    config.callbacks.on_tls_identity = identity_changed;
    config.callbacks.userdata = fixture;
    config.tls.acme.ca_file = "/rotmos-missing-ca-file";
    CHECK(rotmos_server_new(&config, &invalid, &error) == ROTMOS_OK);
    CHECK(rotmos_server_validate(invalid, &error) == ROTMOS_IO);
    CHECK(rotmos_server_destroy(invalid, &error) == ROTMOS_OK);
    ca_file = path(fixture->root, "oversized-ca.pem");
    CHECK(ca_file != NULL);
    descriptor = open(ca_file, O_CREAT | O_WRONLY | O_EXCL, 0600);
    CHECK(descriptor >= 0 && ftruncate(descriptor, 1024L * 1024L + 1) == 0);
    CHECK(close(descriptor) == 0);
    config.tls.acme.ca_file = ca_file;
    CHECK(rotmos_server_new(&config, &invalid, &error) == ROTMOS_OK);
    CHECK(rotmos_server_validate(invalid, &error) == ROTMOS_LIMIT);
    CHECK(rotmos_server_destroy(invalid, &error) == ROTMOS_OK);
    CHECK(unlink(ca_file) == 0);
    free(ca_file);
    config.tls.acme.ca_file = NULL;
    config.tls.acme.directory_url = "file:///rotmos-directory";
    CHECK(rotmos_server_new(&config, &invalid, &error) == ROTMOS_OK);
    CHECK(rotmos_server_validate(invalid, &error) == ROTMOS_INVALID);
    CHECK(rotmos_server_destroy(invalid, &error) == ROTMOS_OK);
    config.tls.acme.directory_url = "http://127.0.0.1:1/directory";
    if (fixture->mode == 4) {
        listeners[0].kind = ROTMOS_LISTENER_HTTP;
        config.listener_count = 1;
        config.tls.identities = &identities[1];
        config.tls.identity_count = 1;
        config.callbacks.on_parent_ready = unbound_ready;
    }
    CHECK(rotmos_server_new(&config, &fixture->server, &error) == ROTMOS_OK);
    CHECK(fixture->restores == 0);
    private_directory = path(fixture->root, ".rotmos");
    CHECK(private_directory != NULL && access(private_directory, F_OK) != 0 &&
          errno == ENOENT);
    free(private_directory);
    CHECK(rotmos_server_validate(fixture->server, &error) == ROTMOS_OK);
    CHECK(fixture->restores == 0);
    CHECK(rotmos_server_tls_identity_state(fixture->server, 0, &current,
                                           &error) == ROTMOS_STATE);
    rotmos_route_config_init(&route);
    route.path = "/";
    route.handler = handler;
    CHECK(rotmos_router_route(rotmos_server_router(fixture->server), &route,
                              NULL, &error) == ROTMOS_OK);
    status = rotmos_server_run(fixture->server, &result, &error);
    if (status != ROTMOS_OK || fixture->failure)
        (void)fprintf(
            stderr,
            "ACME mode %d run %d %s: %s phase %d restarts %d failures %d\n",
            fixture->mode, status, error.operation, error.message,
            fixture->phase, fixture->restarts, fixture->observed_failure);
    if (fixture->client > 0) {
        (void)kill(fixture->client, SIGKILL);
        (void)waitpid(fixture->client, NULL, 0);
    }
    CHECK(status == ROTMOS_OK && !fixture->failure && fixture->phase == 4);
    CHECK(fixture->restores == (fixture->mode != 4) &&
          fixture->restarts == (fixture->mode == 1 || fixture->mode == 2));
    CHECK(fixture->errors == (fixture->mode == 1));
    CHECK(fixture->escalated == (fixture->mode == 2));
    CHECK(fixture->observed_failure ==
          (fixture->mode == 1 || fixture->mode == 2));
    CHECK(fixture->observed_pending >= 1 &&
          (fixture->mode == 4 ? fixture->observed_usable == 0
                              : fixture->observed_usable >= 1));
    if (result.has_failure)
        (void)fprintf(
            stderr,
            "last child role %d kind %d exit %d signal %d expected %d: %s %s\n",
            result.last_failure.process.role, result.last_failure.kind,
            result.last_failure.exit_status, result.last_failure.signal_number,
            result.last_failure.expected, result.last_failure.error.operation,
            result.last_failure.error.message);
    CHECK(result.has_failure == (fixture->mode == 1 || fixture->mode == 2) &&
          result.recovered_failures.high == 0 &&
          result.recovered_failures.low == (unsigned long)fixture->restarts);
    CHECK(result.reason == ROTMOS_RUN_REQUESTED_STOP);
    CHECK(rotmos_server_destroy(fixture->server, &error) == ROTMOS_OK);
    CHECK(remove_tree(directory));
    for (i = 0; i < 2; ++i) {
        X509_free(fixture->certificates[i]);
        free(fixture->pem[i]);
        (void)close(fixture->notes[i]);
        (void)close(fixture->command[i]);
    }
    EVP_PKEY_free(fixture->key);
    free(fixture->key_pem);
    free(fixture->root);
    free(directory);
    free(fixture);
    return 0;
}
