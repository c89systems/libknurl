#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "acme_internal.h"

#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
            (void)fprintf(stderr, "ACME state line %d: %s\n", __LINE__,        \
                          #test);                                              \
            return 1;                                                          \
        }                                                                      \
    } while (0)

struct fixture {
    struct rotmos_acme_store *store;
    char *keys[2];
    char *certificates[2];
    char *root;
    long parent;
    int notes[2];
    int mode;
    int current;
    int restores;
};

static int write_text(const char *directory, const char *leaf, const char *text)
{
    char *path;
    FILE *file;
    int okay;
    path = rotmos_acme_path(directory, leaf);
    if (path == NULL)
        return 0;
    file = fopen(path, "w");
    free(path);
    okay = file != NULL && fputs(text, file) >= 0;
    if (file != NULL && fclose(file) != 0)
        okay = 0;
    return okay;
}

static int matches(const char *directory, const char *leaf,
                   const char *expected)
{
    char *path;
    char *actual;
    size_t length;
    size_t count;
    FILE *file;
    int okay;
    path = rotmos_acme_path(directory, leaf);
    if (path == NULL)
        return 0;
    file = fopen(path, "r");
    free(path);
    if (file == NULL)
        return 0;
    length = strlen(expected);
    actual = malloc(length + 1);
    if (actual == NULL) {
        (void)fclose(file);
        return 0;
    }
    count = fread(actual, 1, length + 1, file);
    okay =
        count == length && !memcmp(actual, expected, length) && !ferror(file);
    (void)fclose(file);
    free(actual);
    return okay;
}

static int active_matches(struct fixture *fixture, int generation)
{
    char *active;
    int okay;
    if (rotmos_acme_store_active(fixture->store, &active, NULL) != ROTMOS_OK)
        return 0;
    okay = matches(active, "certificates/example.test/key.pem",
                   fixture->keys[generation]) &&
           matches(active, "certificates/example.test/fullchain.pem",
                   fixture->certificates[generation]) &&
           matches(active, "account-key.pem", fixture->keys[0]) &&
           matches(active, "certificates/other.example.test/fullchain.pem",
                   fixture->certificates[0]);
    free(active);
    return okay;
}

static rotmos_status callback(rotmos_server *server,
                              const rotmos_process_context *process,
                              rotmos_acme_event event, rotmos_bytes domain,
                              rotmos_bytes directory, void *userdata,
                              rotmos_error *error)
{
    struct fixture *fixture;
    struct stat info;
    fixture = userdata;
    if (server == NULL || process->process_id != (long)getpid() ||
        directory.data == NULL || directory.length != strlen(directory.data))
        return ROTMOS_STATE;
    if (event == ROTMOS_ACME_RESTORE) {
        ++fixture->restores;
        if (process->role != ROTMOS_PROCESS_PARENT ||
            process->process_id != fixture->parent || domain.length != 0 ||
            strcmp(directory.data, fixture->store->candidate))
            return ROTMOS_STATE;
        if (fixture->mode == 6 || fixture->mode == 7) {
            if (!active_matches(fixture, fixture->current) ||
                !write_text(directory.data, "certificates/example.test/key.pem",
                            fixture->keys[0]) ||
                !write_text(directory.data,
                            "certificates/example.test/fullchain.pem",
                            fixture->certificates[0]))
                return ROTMOS_STATE;
            if (fixture->mode == 6) {
                rotmos_error_clear(error);
                error->status = ROTMOS_IO;
                (void)strcpy(error->message, "expected restore failure");
                return ROTMOS_IO;
            }
        }
        return ROTMOS_OK;
    }
    if (event == ROTMOS_ACME_ERROR) {
        if (error->status != ROTMOS_IO && error->status != ROTMOS_TIMEOUT)
            return ROTMOS_STATE;
        (void)write(fixture->notes[1], "E", 1);
        error->status = ROTMOS_INVALID;
        (void)strcpy(error->message,
                     "observation deliberately changed its local copy");
        return ROTMOS_INVALID;
    }
    if (process->role != ROTMOS_PROCESS_KEY_MANAGER ||
        process->process_id == fixture->parent)
        return ROTMOS_STATE;
    if (event != ROTMOS_ACME_PERSIST ||
        (domain.length != 0 &&
         (domain.length != 12 || memcmp(domain.data, "example.test", 12))) ||
        !strcmp(directory.data, fixture->store->root) ||
        (fixture->store->work != NULL &&
         !strcmp(directory.data, fixture->store->work)) ||
        stat(directory.data, &info) != 0 || !S_ISDIR(info.st_mode) ||
        (info.st_mode & 077) != 0 ||
        !matches(directory.data, "account-key.pem", fixture->keys[0]) ||
        !matches(directory.data, "certificates/example.test/key.pem",
                 fixture->keys[1]) ||
        !matches(directory.data, "certificates/example.test/fullchain.pem",
                 fixture->certificates[1]) ||
        !matches(directory.data, "certificates/other.example.test/key.pem",
                 fixture->keys[0]) ||
        !active_matches(fixture, fixture->current))
        return ROTMOS_STATE;
    (void)write(fixture->notes[1], "P", 1);
    if (fixture->mode == 2)
        (void)raise(SIGKILL);
    if (fixture->mode == 5) {
        struct timespec pause;
        pause.tv_sec = 0;
        pause.tv_nsec = 5000000L;
        (void)nanosleep(&pause, NULL);
    }
    if (fixture->mode == 1) {
        rotmos_error_clear(error);
        error->status = ROTMOS_IO;
        error->source = ROTMOS_ERROR_SYSTEM;
        (void)strcpy(error->operation, "fixture_persist");
        (void)strcpy(error->message, "expected persistence failure");
        return ROTMOS_IO;
    }
    return ROTMOS_OK;
}

static int credential(struct fixture *fixture, int index)
{
    EVP_PKEY *key;
    X509 *certificate;
    X509_NAME *name;
    X509_EXTENSION *san;
    BIO *output;
    char *bytes;
    long length;
    key = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", 2048);
    CHECK(key != NULL);
    certificate = X509_new();
    CHECK(certificate != NULL);
    CHECK(X509_set_version(certificate, 2) == 1);
    CHECK(ASN1_INTEGER_set(X509_get_serialNumber(certificate), index + 1) == 1);
    CHECK(X509_gmtime_adj(X509_getm_notBefore(certificate), -60) != NULL);
    CHECK(X509_gmtime_adj(X509_getm_notAfter(certificate), 3600) != NULL);
    CHECK(X509_set_pubkey(certificate, key) == 1);
    name = X509_get_subject_name(certificate);
    CHECK(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                     (const unsigned char *)"example.test", -1,
                                     -1, 0) == 1);
    CHECK(X509_set_issuer_name(certificate, name) == 1);
    san = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name,
                              "DNS:example.test,DNS:other.example.test");
    CHECK(san != NULL && X509_add_ext(certificate, san, -1) == 1);
    X509_EXTENSION_free(san);
    CHECK(X509_sign(certificate, key, EVP_sha256()) > 0);
    output = BIO_new(BIO_s_mem());
    CHECK(output != NULL && PEM_write_bio_PrivateKey(output, key, NULL, NULL, 0,
                                                     NULL, NULL) == 1);
    length = BIO_get_mem_data(output, &bytes);
    fixture->keys[index] = malloc((size_t)length + 1);
    CHECK(fixture->keys[index] != NULL);
    memcpy(fixture->keys[index], bytes, (size_t)length);
    fixture->keys[index][length] = '\0';
    BIO_free(output);
    output = BIO_new(BIO_s_mem());
    CHECK(output != NULL && PEM_write_bio_X509(output, certificate) == 1);
    length = BIO_get_mem_data(output, &bytes);
    fixture->certificates[index] = malloc((size_t)length + 1);
    CHECK(fixture->certificates[index] != NULL);
    memcpy(fixture->certificates[index], bytes, (size_t)length);
    fixture->certificates[index][length] = '\0';
    BIO_free(output);
    X509_free(certificate);
    EVP_PKEY_free(key);
    return 0;
}

static int prepare_files(struct fixture *fixture)
{
    char *certs;
    char *domain;
    const char *names[] = {"example.test", "other.example.test"};
    size_t i;
    CHECK(write_text(fixture->root, "account-key.pem", fixture->keys[0]));
    certs = rotmos_acme_path(fixture->root, "certificates");
    CHECK(certs != NULL &&
          rotmos_acme_directory(certs, 1, 1, NULL) == ROTMOS_OK);
    for (i = 0; i < 2; ++i) {
        domain = rotmos_acme_path(certs, names[i]);
        CHECK(domain != NULL &&
              rotmos_acme_directory(domain, 1, 1, NULL) == ROTMOS_OK);
        CHECK(write_text(domain, "key.pem", fixture->keys[0]));
        CHECK(write_text(domain, "fullchain.pem", fixture->certificates[0]));
        free(domain);
    }
    free(certs);
    return 0;
}

static rotmos_process_context context(rotmos_process_role role)
{
    rotmos_process_context result;
    memset(&result, 0, sizeof(result));
    result.role = role;
    result.process_id = (long)getpid();
    return result;
}

static int mutate(struct fixture *fixture)
{
    CHECK(write_text(fixture->store->work, "certificates/example.test/key.pem",
                     fixture->keys[1]));
    CHECK(write_text(fixture->store->work,
                     "certificates/example.test/fullchain.pem",
                     fixture->certificates[1]));
    return 0;
}

static int child(struct fixture *fixture, rotmos_server *server)
{
    rotmos_process_context process;
    rotmos_error error;
    rotmos_status status;
    process = context(ROTMOS_PROCESS_KEY_MANAGER);
    CHECK(rotmos_acme_store_begin(fixture->store, &error) == ROTMOS_OK);
    CHECK(mutate(fixture) == 0);
    CHECK(rotmos_acme_store_stage(fixture->store, &error) == ROTMOS_OK);
    CHECK(rotmos_acme_store_publish(fixture->store, &error) == ROTMOS_STATE);
    status = rotmos_acme_store_persist(fixture->store, server, &process,
                                       "example.test", &error);
    CHECK(status == (fixture->mode == 1
                         ? ROTMOS_IO
                         : (fixture->mode == 5 ? ROTMOS_TIMEOUT : ROTMOS_OK)));
    if (fixture->mode == 1) {
        CHECK(error.status == ROTMOS_IO &&
              strcmp(error.message, "expected persistence failure") == 0);
        CHECK(active_matches(fixture, fixture->current));
    } else if (fixture->mode == 5) {
        CHECK(error.status == ROTMOS_TIMEOUT &&
              active_matches(fixture, fixture->current));
    } else if (fixture->mode != 3) {
        CHECK(rotmos_acme_store_publish(fixture->store, &error) == ROTMOS_OK);
        CHECK(active_matches(fixture, 1));
    }
    return 0;
}

static int publish(struct fixture *fixture, rotmos_server *server, int mode)
{
    pid_t pid;
    int status;
    fixture->mode = mode;
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0)
        _exit(child(fixture, server));
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(mode == 2 ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
                    : WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

static int recover(struct fixture *fixture, rotmos_server *server,
                   const rotmos_tls_config *config)
{
    rotmos_process_context process;
    struct rotmos_acme_store *store;
    rotmos_error error;
    pid_t pid;
    int status;
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        fixture->mode = 0;
        process = context(ROTMOS_PROCESS_KEY_MANAGER);
        status = rotmos_acme_store_new(config, "DEFAULT", &store, &error) !=
                 ROTMOS_OK;
        if (!status) {
            fixture->store = store;
            status = rotmos_acme_store_recover(store, server, &process,
                                               &error) != ROTMOS_OK;
            if (status)
                (void)fprintf(stderr, "recover: %s\n", error.message);
            rotmos_acme_store_free(store);
        }
        _exit(status);
    }
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

static int notes(struct fixture *fixture, const char *expected)
{
    char data[16];
    size_t used;
    ssize_t count;
    used = 0;
    while ((count = read(fixture->notes[0], data + used, sizeof(data) - used)) >
           0)
        used += (size_t)count;
    CHECK(used == strlen(expected) && !memcmp(data, expected, used));
    return 0;
}

static int invalid_candidate(struct fixture *fixture)
{
    char *large;
    char *path;
    rotmos_error error;
    CHECK(rotmos_acme_store_begin(fixture->store, &error) == ROTMOS_OK);
    CHECK(write_text(fixture->store->work, "certificates/example.test/key.pem",
                     fixture->keys[fixture->current ? 0 : 1]));
    CHECK(rotmos_acme_store_stage(fixture->store, &error) == ROTMOS_INVALID);
    CHECK(active_matches(fixture, fixture->current));
    large = malloc(8194);
    CHECK(large != NULL);
    memset(large, 'x', 8193);
    large[8193] = '\0';
    CHECK(write_text(fixture->store->work, "account-key.pem", large));
    free(large);
    CHECK(rotmos_acme_store_stage(fixture->store, &error) == ROTMOS_LIMIT);
    CHECK(
        write_text(fixture->store->work, "account-key.pem", fixture->keys[0]));
    path = rotmos_acme_path(fixture->store->work,
                            "certificates/example.test/key.pem");
    CHECK(path != NULL && unlink(path) == 0 &&
          symlink("/does-not-exist", path) == 0);
    free(path);
    CHECK(rotmos_acme_store_stage(fixture->store, &error) != ROTMOS_OK);
    CHECK(active_matches(fixture, fixture->current));
    CHECK(notes(fixture, "") == 0);
    return 0;
}

static int cleanup_root(struct fixture *fixture)
{
    char *active;
    char *path;
    const char *domains[] = {"example.test", "other.example.test"};
    size_t i;
    CHECK(rotmos_acme_store_active(fixture->store, &active, NULL) == ROTMOS_OK);
    CHECK(unlinkat(fixture->store->descriptor, "active", 0) == 0);
    CHECK(rotmos_acme_remove(fixture->store, active));
    free(active);
    CHECK(rotmos_acme_sweep(fixture->store, NULL) == ROTMOS_OK);
    path = rotmos_copy_text(fixture->store->directory);
    CHECK(path != NULL);
    rotmos_acme_store_free(fixture->store);
    CHECK(rmdir(path) == 0);
    free(path);
    for (i = 0; i < 2; ++i) {
        path = rotmos_acme_path(fixture->root, "certificates");
        active = rotmos_acme_path(path, domains[i]);
        free(path);
        path = rotmos_acme_path(active, "key.pem");
        CHECK(unlink(path) == 0);
        free(path);
        path = rotmos_acme_path(active, "fullchain.pem");
        CHECK(unlink(path) == 0 && rmdir(active) == 0);
        free(path);
        free(active);
    }
    path = rotmos_acme_path(fixture->root, "certificates");
    CHECK(rmdir(path) == 0);
    free(path);
    path = rotmos_acme_path(fixture->root, "account-key.pem");
    CHECK(unlink(path) == 0 && rmdir(fixture->root) == 0);
    free(path);
    return 0;
}

int main(void)
{
    struct fixture fixture;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_tls_identity identity;
    rotmos_process_context process;
    rotmos_server *server;
    rotmos_error error;
    struct stat info;
    const char *domains[] = {"example.test", "other.example.test"};
    char root[] = "rotmos-acme-state-XXXXXX";
    char *path;
    size_t i;
    memset(&fixture, 0, sizeof(fixture));
    fixture.parent = (long)getpid();
    CHECK(credential(&fixture, 0) == 0 && credential(&fixture, 1) == 0);
    CHECK(mkdtemp(root) != NULL);
    fixture.root = root;
    CHECK(pipe(fixture.notes) == 0 &&
          fcntl(fixture.notes[0], F_SETFL, O_NONBLOCK) == 0);
    CHECK(prepare_files(&fixture) == 0);
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    rotmos_tls_identity_init(&identity);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = 1;
    identity.mode = ROTMOS_TLS_ACME;
    identity.domains = domains;
    identity.domain_count = 2;
    config.tls.identities = &identity;
    config.tls.identity_count = 1;
    config.tls.max_material_bytes = 8192;
    config.tls.acme.runtime_directory = root;
    config.tls.acme.email = "fixture@example.test";
    config.tls.acme.directory_url = "http://127.0.0.1:1/directory";
    config.tls.acme.state = callback;
    config.tls.acme.userdata = &fixture;
    domains[0] = "a..example.test";
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_INVALID &&
          server == NULL);
    domains[0] = "example.test";
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    CHECK(chmod(root, 0755) == 0);
    CHECK(rotmos_acme_store_new(&config.tls, "DEFAULT", &fixture.store,
                                &error) == ROTMOS_INVALID &&
          fixture.store == NULL);
    CHECK(chmod(root, 0700) == 0);
    CHECK(rotmos_acme_store_new(&config.tls, "DEFAULT", &fixture.store,
                                &error) == ROTMOS_OK);
    path = rotmos_acme_path(root, ".rotmos");
    CHECK(path != NULL && lstat(path, &info) == -1 && errno == ENOENT &&
          fixture.restores == 0);
    free(path);
    process = context(ROTMOS_PROCESS_PARENT);
    CHECK(rotmos_acme_store_restore(fixture.store, server, &process, &error) ==
          ROTMOS_OK);
    CHECK(fixture.restores == 1 && active_matches(&fixture, 0));
    CHECK(publish(&fixture, server, 1) == 0 && notes(&fixture, "PE") == 0);
    CHECK(active_matches(&fixture, 0));
    CHECK(recover(&fixture, server, &config.tls) == 0 &&
          notes(&fixture, "") == 0);
    CHECK(publish(&fixture, server, 3) == 0 && notes(&fixture, "P") == 0);
    CHECK(active_matches(&fixture, 0));
    CHECK(recover(&fixture, server, &config.tls) == 0 &&
          notes(&fixture, "") == 0);
    fixture.current = 1;
    CHECK(active_matches(&fixture, 1));
    rotmos_acme_store_free(fixture.store);
    CHECK(rotmos_acme_store_new(&config.tls, "DEFAULT", &fixture.store,
                                &error) == ROTMOS_OK);
    CHECK(rotmos_acme_store_restore(fixture.store, server, &process, &error) ==
          ROTMOS_OK);
    CHECK(active_matches(&fixture, 1));
    fixture.mode = 7;
    CHECK(rotmos_acme_store_restore(fixture.store, server, &process, &error) ==
          ROTMOS_OK);
    fixture.current = 0;
    CHECK(active_matches(&fixture, 0));
    CHECK(publish(&fixture, server, 2) == 0 && notes(&fixture, "P") == 0);
    CHECK(active_matches(&fixture, 0));
    CHECK(recover(&fixture, server, &config.tls) == 0 &&
          notes(&fixture, "P") == 0);
    fixture.current = 1;
    CHECK(active_matches(&fixture, 1));
    config.tls.acme.persist_timeout_ms = 1;
    CHECK(publish(&fixture, server, 5) == 0 && notes(&fixture, "PE") == 0);
    config.tls.acme.persist_timeout_ms = 30000;
    CHECK(active_matches(&fixture, 1));
    CHECK(publish(&fixture, server, 0) == 0 && notes(&fixture, "P") == 0);
    CHECK(active_matches(&fixture, 1));
    fixture.mode = 6;
    CHECK(rotmos_acme_store_restore(fixture.store, server, &process, &error) ==
          ROTMOS_IO);
    CHECK(!strcmp(error.message, "expected restore failure") &&
          notes(&fixture, "E") == 0);
    CHECK(active_matches(&fixture, 1));
    CHECK(invalid_candidate(&fixture) == 0);
    CHECK(cleanup_root(&fixture) == 0);
    CHECK(rotmos_server_destroy(server, NULL) == ROTMOS_OK);
    CHECK(close(fixture.notes[0]) == 0 && close(fixture.notes[1]) == 0);
    for (i = 0; i < 2; ++i) {
        free(fixture.keys[i]);
        free(fixture.certificates[i]);
    }
    return 0;
}
