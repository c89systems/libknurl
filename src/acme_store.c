#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "acme_internal.h"
#include "loop_internal.h"
#include <rotmos/loop.h>

#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int rotmos_acme_domain_valid(const char *domain)
{
    size_t i;
    size_t label;
    unsigned char byte;
    if (domain == NULL || *domain == '\0' || strlen(domain) > 253)
        return 0;
    label = 0;
    for (i = 0; domain[i] != '\0'; ++i) {
        byte = (unsigned char)domain[i];
        if (byte == '.') {
            if (label == 0 || domain[i - 1] == '-')
                return 0;
            label = 0;
        } else {
            if (!((byte >= 'a' && byte <= 'z') ||
                  (byte >= 'A' && byte <= 'Z') ||
                  (byte >= '0' && byte <= '9') || (byte == '-' && label != 0)))
                return 0;
            if (++label > 63)
                return 0;
        }
    }
    return label != 0 && domain[i - 1] != '-';
}

static rotmos_status ensure(struct rotmos_acme_store *store, int create,
                            rotmos_error *error)
{
    rotmos_status status;
    struct stat info;
    int root;
    if (store->descriptor >= 0)
        return ROTMOS_OK;
    if (!create && lstat(store->directory, &info) != 0 && errno == ENOENT)
        return ROTMOS_EOF;
    status = rotmos_acme_directory(store->directory, create, 1, error);
    if (status != ROTMOS_OK)
        return status;
    store->descriptor =
        open(store->directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (store->descriptor < 0)
        return rotmos_fail(error, ROTMOS_IO, "acme_store",
                           "cannot open library-owned ACME directory");
    if (create) {
        root =
            open(store->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (root < 0)
            return rotmos_fail(error, ROTMOS_IO, "acme_store",
                               "cannot open private ACME root");
        status = rotmos_acme_sync(root, error);
        (void)close(root);
        if (status != ROTMOS_OK)
            return status;
    }
    return ROTMOS_OK;
}

rotmos_status rotmos_acme_store_new(const rotmos_tls_config *config,
                                    const char *default_ciphers,
                                    struct rotmos_acme_store **out,
                                    rotmos_error *error)
{
    struct rotmos_acme_store *store;
    const rotmos_tls_identity *identity;
    size_t count;
    size_t i;
    size_t j;
    size_t k;
    rotmos_status status;
    *out = NULL;
    if (config->acme.runtime_directory == NULL || config->acme.state == NULL ||
        config->max_material_bytes == 0 ||
        config->max_material_bytes > INT_MAX - 1UL)
        return rotmos_fail(error, ROTMOS_INVALID, "acme_store",
                           "provide private runtime directory, state callback "
                           "and bounded materials");
    status = rotmos_acme_directory(config->acme.runtime_directory, 0, 1, error);
    if (status != ROTMOS_OK)
        return status;
    count = 0;
    for (i = 0; i < config->identity_count; ++i) {
        if (config->identities[i].mode == ROTMOS_TLS_ACME) {
            if (config->identities[i].domain_count > 65535UL - count)
                return rotmos_fail(error, ROTMOS_LIMIT, "acme_store",
                                   "too many ACME domains");
            count += config->identities[i].domain_count;
        }
    }
    if (count == 0)
        return rotmos_fail(error, ROTMOS_INVALID, "acme_store",
                           "declare at least one ACME domain");
    store = calloc(1, sizeof(*store));
    if (store == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "acme_store",
                           "cannot allocate ACME state");
    store->config = config;
    store->default_ciphers = default_ciphers;
    store->descriptor = -1;
    store->root = realpath(config->acme.runtime_directory, NULL);
    store->domains = calloc(count, sizeof(*store->domains));
    store->directory =
        store->root != NULL ? rotmos_acme_path(store->root, ".rotmos") : NULL;
    if (store->root == NULL || store->domains == NULL ||
        store->directory == NULL) {
        rotmos_acme_store_free(store);
        return rotmos_fail(error, ROTMOS_NOMEM, "acme_store",
                           "cannot allocate ACME paths/domain registry");
    }
    for (i = 0; i < config->identity_count; ++i) {
        identity = &config->identities[i];
        if (identity->mode != ROTMOS_TLS_ACME)
            continue;
        for (j = 0; j < identity->domain_count; ++j) {
            if (!rotmos_acme_domain_valid(identity->domains[j])) {
                rotmos_acme_store_free(store);
                return rotmos_fail(
                    error, ROTMOS_INVALID, "acme_store",
                    "ACME domain must be a DNS name with safe nonempty labels");
            }
            for (k = 0; k < store->domain_count; ++k)
                if (rotmos_text_equal_case(store->domains[k],
                                           identity->domains[j])) {
                    rotmos_acme_store_free(store);
                    return rotmos_fail(
                        error, ROTMOS_INVALID, "acme_store",
                        "ACME domain appears in more than one identity");
                }
            store->domains[store->domain_count++] = identity->domains[j];
        }
    }
    *out = store;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_acme_store_free(struct rotmos_acme_store *store)
{
    if (store == NULL)
        return;
    if (store->work != NULL)
        (void)rotmos_acme_remove(store, store->work);
    if (store->descriptor >= 0)
        (void)close(store->descriptor);
    free(store->domains);
    free(store->root);
    free(store->directory);
    free(store->work);
    free(store->candidate);
    free(store);
}

static int no_password(char *buffer, int size, int writing, void *userdata)
{
    (void)buffer;
    (void)size;
    (void)writing;
    (void)userdata;
    return 0;
}

static rotmos_status check_key(struct rotmos_acme_store *store,
                               const char *path, rotmos_error *error)
{
    rotmos_tls_material material;
    unsigned char *data;
    size_t length;
    struct stat info;
    BIO *input;
    EVP_PKEY *key;
    rotmos_status status;
    if (lstat(path, &info) != 0) {
        if (errno == ENOENT)
            return ROTMOS_EOF;
        return rotmos_fail(error, ROTMOS_IO, "acme_key",
                           "cannot inspect ACME private key");
    }
    if (!S_ISREG(info.st_mode))
        return rotmos_fail(error, ROTMOS_INVALID, "acme_key",
                           "ACME private key must be a regular file");
    rotmos_tls_material_init(&material);
    material.kind = ROTMOS_MATERIAL_FILE;
    material.path = path;
    status = rotmos_tls_material_read(
        &material, store->config->max_material_bytes, &data, &length, error);
    if (status != ROTMOS_OK)
        return status;
    input = BIO_new_mem_buf(data, (int)length);
    key = input != NULL
              ? PEM_read_bio_PrivateKey(input, NULL, no_password, NULL)
              : NULL;
    if (key == NULL || !EVP_PKEY_is_a(key, "RSA") ||
        EVP_PKEY_get_bits(key) < 2048)
        status = rotmos_fail(error, ROTMOS_INVALID, "acme_key",
                             "ACME keys require an unencrypted RSA private key "
                             "of at least 2048 bits");
    EVP_PKEY_free(key);
    BIO_free(input);
    rotmos_tls_material_clear(data, length);
    return status;
}

rotmos_status rotmos_acme_check(struct rotmos_acme_store *store,
                                const char *directory, rotmos_error *error)
{
    rotmos_tls_identity identity;
    rotmos_status status;
    char *path;
    char *certs;
    char *domain;
    char *certificate;
    char *key;
    struct stat info;
    SSL_CTX *context;
    size_t i;
    path = rotmos_acme_path(directory, "account-key.pem");
    if (path == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "acme_check",
                           "cannot allocate account path");
    status = check_key(store, path, error);
    if (status == ROTMOS_EOF)
        status = ROTMOS_OK;
    free(path);
    certs = rotmos_acme_path(directory, "certificates");
    if (certs == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "acme_check",
                           "cannot allocate certificate path");
    for (i = 0; i < store->domain_count && status == ROTMOS_OK; ++i) {
        domain = rotmos_acme_path(certs, store->domains[i]);
        certificate =
            domain != NULL ? rotmos_acme_path(domain, "fullchain.pem") : NULL;
        key = domain != NULL ? rotmos_acme_path(domain, "key.pem") : NULL;
        if (domain == NULL || certificate == NULL || key == NULL)
            status = rotmos_fail(error, ROTMOS_NOMEM, "acme_check",
                                 "cannot allocate identity paths");
        else {
            status = check_key(store, key, error);
            if (status == ROTMOS_EOF)
                status = ROTMOS_OK;
            if (status == ROTMOS_OK && lstat(certificate, &info) == 0) {
                if (!S_ISREG(info.st_mode))
                    status = rotmos_fail(error, ROTMOS_INVALID, "acme_check",
                                         "ACME chain must be a regular file");
                else {
                    rotmos_tls_identity_init(&identity);
                    identity.mode = ROTMOS_TLS_MANUAL;
                    identity.certificate.kind = ROTMOS_MATERIAL_FILE;
                    identity.certificate.path = certificate;
                    identity.private_key.kind = ROTMOS_MATERIAL_FILE;
                    identity.private_key.path = key;
                    context = NULL;
                    status = rotmos_tls_identity_prepare(
                        store->config, &identity,
                        store->config->cipher_list != NULL
                            ? store->config->cipher_list
                            : store->default_ciphers,
                        &context, error);
                    if (status == ROTMOS_OK &&
                        X509_check_host(SSL_CTX_get0_certificate(context),
                                        store->domains[i], 0,
                                        X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS,
                                        NULL) != 1)
                        status =
                            rotmos_fail(error, ROTMOS_INVALID, "acme_check",
                                        "ACME certificate does not cover its "
                                        "configured domain");
                    SSL_CTX_free(context);
                }
            } else if (status == ROTMOS_OK && errno != ENOENT)
                status = rotmos_fail(error, ROTMOS_IO, "acme_check",
                                     "cannot inspect ACME chain");
        }
        free(domain);
        free(certificate);
        free(key);
    }
    free(certs);
    return status;
}

static rotmos_status state_callback(struct rotmos_acme_store *store,
                                    rotmos_server *server,
                                    const rotmos_process_context *process,
                                    rotmos_acme_event event, const char *domain,
                                    const char *directory, long budget,
                                    rotmos_error *error)
{
    rotmos_error local;
    rotmos_status status;
    rotmos_u64 start;
    rotmos_u64 duration;
    rotmos_u64 deadline;
    rotmos_u64 now;
    rotmos_bytes name;
    rotmos_bytes path;
    int order;
    name.data = domain != NULL ? domain : "";
    name.length = strlen(name.data);
    path.data = directory;
    path.length = strlen(directory);
    rotmos_error_clear(&local);
    status = rotmos_loop_monotonic(&start, &local);
    if (status != ROTMOS_OK)
        goto failed;
    status = store->config->acme.state(server, process, event, name, path,
                                       store->config->acme.userdata, &local);
    if (status < ROTMOS_OK || status == ROTMOS_AGAIN || status == ROTMOS_EOF ||
        status > ROTMOS_BACKEND)
        status = ROTMOS_INVALID;
    if (status == ROTMOS_OK && budget > 0) {
        (void)rotmos_u64_from_size((size_t)budget, &duration, NULL);
        if (rotmos_u64_add(start, duration, &deadline, &local) != ROTMOS_OK ||
            rotmos_loop_monotonic(&now, &local) != ROTMOS_OK)
            status = local.status;
        else {
            (void)rotmos_u64_compare(now, deadline, &order, NULL);
            if (order >= 0)
                status = rotmos_fail(
                    &local, ROTMOS_TIMEOUT, "acme_callback",
                    "ACME state callback exceeded its cooperative budget");
        }
    }
failed:
    if (status != ROTMOS_OK && local.status != status)
        (void)rotmos_fail(&local, status, "acme_callback",
                          "ACME state callback failed");
    if (status == ROTMOS_OK)
        rotmos_error_clear(&local);
    if (error != NULL)
        *error = local;
    return status;
}

static void observe_error(struct rotmos_acme_store *store,
                          rotmos_server *server,
                          const rotmos_process_context *process,
                          const char *domain, const char *directory,
                          const rotmos_error *cause)
{
    rotmos_error copy;
    rotmos_bytes name;
    rotmos_bytes path;
    copy = *cause;
    name.data = domain != NULL ? domain : "";
    name.length = strlen(name.data);
    path.data = directory;
    path.length = strlen(directory);
    (void)store->config->acme.state(server, process, ROTMOS_ACME_ERROR, name,
                                    path, store->config->acme.userdata, &copy);
}

static rotmos_status acknowledged(struct rotmos_acme_store *store, int create,
                                  rotmos_error *error)
{
    static const char marker[] = "acknowledged\n";
    char data[sizeof(marker)];
    char *path;
    int fd;
    int directory;
    ssize_t count;
    rotmos_status status;
    path = rotmos_acme_path(store->candidate, ".acknowledged");
    if (path == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "acme_acknowledge",
                           "cannot allocate acknowledgement path");
    fd = open(path,
              (create ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY) | O_CLOEXEC |
                  O_NOFOLLOW | O_NONBLOCK,
              0600);
    free(path);
    if (fd < 0) {
        if (!create && errno == ENOENT)
            return ROTMOS_EOF;
        return rotmos_fail(error, ROTMOS_IO, "acme_acknowledge",
                           "cannot open generation acknowledgement");
    }
    do {
        count = create ? write(fd, marker, sizeof(marker) - 1)
                       : read(fd, data, sizeof(data));
    } while (count < 0 && errno == EINTR);
    status = count == sizeof(marker) - 1 &&
                     (create || !memcmp(data, marker, sizeof(marker) - 1))
                 ? ROTMOS_OK
                 : rotmos_fail(error, ROTMOS_INVALID, "acme_acknowledge",
                               "incomplete generation acknowledgement");
    if (status == ROTMOS_OK && create)
        status = rotmos_acme_sync(fd, error);
    if (close(fd) != 0 && status == ROTMOS_OK)
        status = rotmos_fail(error, ROTMOS_IO, "acme_acknowledge",
                             "cannot close generation acknowledgement");
    if (status == ROTMOS_OK && create) {
        directory = open(store->candidate,
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory < 0)
            status = rotmos_fail(error, ROTMOS_IO, "acme_acknowledge",
                                 "cannot open acknowledged generation");
        else {
            status = rotmos_acme_sync(directory, error);
            (void)close(directory);
        }
    }
    return status;
}

static void discard(struct rotmos_acme_store *store)
{
    (void)unlinkat(store->descriptor, "pending", 0);
    (void)rotmos_acme_sync(store->descriptor, NULL);
    if (store->candidate != NULL)
        (void)rotmos_acme_remove(store, store->candidate);
    free(store->candidate);
    store->candidate = NULL;
}

rotmos_status rotmos_acme_store_stage(struct rotmos_acme_store *store,
                                      rotmos_error *error)
{
    char *candidate;
    rotmos_status status;
    if (store->work == NULL || store->candidate != NULL)
        return rotmos_fail(error, ROTMOS_STATE, "acme_stage",
                           "prepare one working generation before staging");
    status = rotmos_acme_snapshot(store, store->work, "generation-XXXXXX",
                                  &candidate, error);
    if (status != ROTMOS_OK)
        return status;
    status = rotmos_acme_check(store, candidate, error);
    if (status == ROTMOS_OK)
        status = rotmos_acme_select(store, "pending", candidate, error);
    if (status != ROTMOS_OK) {
        (void)rotmos_acme_remove(store, candidate);
        free(candidate);
        return status;
    }
    store->candidate = candidate;
    return rotmos_acme_sync(store->descriptor, error);
}

rotmos_status rotmos_acme_store_publish(struct rotmos_acme_store *store,
                                        rotmos_error *error)
{
    rotmos_status status;
    if (store->candidate == NULL)
        return rotmos_fail(
            error, ROTMOS_STATE, "acme_publish",
            "stage and acknowledge one candidate before publication");
    status = acknowledged(store, 0, error);
    if (status == ROTMOS_EOF)
        return rotmos_fail(error, ROTMOS_STATE, "acme_publish",
                           "candidate persistence has not been acknowledged");
    if (status == ROTMOS_OK)
        status = rotmos_acme_check(store, store->candidate, error);
    if (status == ROTMOS_OK)
        status = rotmos_acme_select(store, "active", store->candidate, error);
    if (status == ROTMOS_OK)
        status = rotmos_acme_sync(store->descriptor, error);
    if (status != ROTMOS_OK)
        return status;
    if (unlinkat(store->descriptor, "pending", 0) != 0 && errno != ENOENT)
        return rotmos_fail(error, ROTMOS_IO, "acme_publish",
                           "cannot clear published candidate selection");
    status = rotmos_acme_sync(store->descriptor, error);
    if (status != ROTMOS_OK)
        return status;
    free(store->candidate);
    store->candidate = NULL;
    return rotmos_acme_sweep(store, error);
}

rotmos_status rotmos_acme_store_active(struct rotmos_acme_store *store,
                                       char **out, rotmos_error *error)
{
    rotmos_status status;
    *out = NULL;
    status = ensure(store, 0, error);
    return status == ROTMOS_OK
               ? rotmos_acme_selected(store, "active", out, error)
               : status;
}

rotmos_status rotmos_acme_store_begin(struct rotmos_acme_store *store,
                                      rotmos_error *error)
{
    char *active;
    char *pending;
    rotmos_status status;
    if (store->work != NULL)
        return rotmos_fail(error, ROTMOS_STATE, "acme_begin",
                           "working generation already exists");
    status = ensure(store, 0, error);
    if (status != ROTMOS_OK)
        return rotmos_fail(error, ROTMOS_STATE, "acme_begin",
                           "restore state before starting a key manager");
    status = rotmos_acme_selected(store, "pending", &pending, error);
    free(pending);
    if (status != ROTMOS_EOF)
        return status == ROTMOS_OK
                   ? rotmos_fail(
                         error, ROTMOS_STATE, "acme_begin",
                         "recover pending publication before starting work")
                   : status;
    status = rotmos_acme_store_active(store, &active, error);
    if (status == ROTMOS_OK)
        status = rotmos_acme_check(store, active, error);
    if (status == ROTMOS_OK)
        status = rotmos_acme_snapshot(store, active, "work-XXXXXX",
                                      &store->work, error);
    free(active);
    if (status == ROTMOS_OK)
        status = rotmos_acme_sweep(store, error);
    return status;
}

rotmos_status rotmos_acme_store_persist(struct rotmos_acme_store *store,
                                        rotmos_server *server,
                                        const rotmos_process_context *process,
                                        const char *domain, rotmos_error *error)
{
    rotmos_error local;
    rotmos_status status;
    if (store->candidate == NULL ||
        process->role != ROTMOS_PROCESS_KEY_MANAGER ||
        process->process_id != (long)getpid())
        return rotmos_fail(
            error, ROTMOS_STATE, "acme_persist",
            "persist a staged candidate in the key-manager process");
    rotmos_error_clear(&local);
    status = acknowledged(store, 0, &local);
    if (status == ROTMOS_OK) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (status != ROTMOS_EOF) {
        if (error != NULL)
            *error = local;
        return status;
    }
    status = state_callback(store, server, process, ROTMOS_ACME_PERSIST, domain,
                            store->candidate,
                            store->config->acme.persist_timeout_ms != 0
                                ? store->config->acme.persist_timeout_ms
                                : 30000,
                            &local);
    if (status == ROTMOS_OK)
        status = rotmos_acme_check(store, store->candidate, &local);
    if (status == ROTMOS_OK)
        status = acknowledged(store, 1, &local);
    if (status != ROTMOS_OK) {
        observe_error(store, server, process, domain, store->candidate, &local);
        discard(store);
    }
    if (error != NULL)
        *error = local;
    return status;
}

rotmos_status rotmos_acme_store_recover(struct rotmos_acme_store *store,
                                        rotmos_server *server,
                                        const rotmos_process_context *process,
                                        rotmos_error *error)
{
    rotmos_status status;
    status = ensure(store, 0, error);
    if (status != ROTMOS_OK)
        return status;
    if (store->candidate != NULL)
        return rotmos_fail(error, ROTMOS_STATE, "acme_recover",
                           "candidate already owned by this operation");
    status = rotmos_acme_selected(store, "pending", &store->candidate, error);
    if (status == ROTMOS_EOF)
        return rotmos_acme_sweep(store, error);
    if (status != ROTMOS_OK)
        return status;
    status = rotmos_acme_check(store, store->candidate, error);
    if (status != ROTMOS_OK) {
        rotmos_error local;
        if (error != NULL)
            local = *error;
        else
            (void)rotmos_fail(&local, status, "acme_recover",
                              "invalid pending ACME generation");
        observe_error(store, server, process, NULL, store->candidate, &local);
        discard(store);
        return status;
    }
    if (status == ROTMOS_OK)
        status = rotmos_acme_store_persist(store, server, process, NULL, error);
    if (status == ROTMOS_OK)
        status = rotmos_acme_store_publish(store, error);
    return status;
}

rotmos_status rotmos_acme_store_restore(struct rotmos_acme_store *store,
                                        rotmos_server *server,
                                        const rotmos_process_context *process,
                                        rotmos_error *error)
{
    rotmos_error local;
    rotmos_status status;
    char *active;
    int selected;
    if (process->role != ROTMOS_PROCESS_PARENT ||
        process->process_id != (long)getpid())
        return rotmos_fail(error, ROTMOS_STATE, "acme_restore",
                           "restore state in the quiescent runtime parent");
    if (store->work != NULL || store->candidate != NULL)
        return rotmos_fail(error, ROTMOS_STATE, "acme_restore",
                           "restore before starting private work");
    rotmos_error_clear(&local);
    status = ensure(store, 1, &local);
    selected = 0;
    /* Complete an already acknowledged publication before hydrating a fresh
     * restore snapshot. An unacknowledged candidate remains unselected. */
    if (status == ROTMOS_OK) {
        status =
            rotmos_acme_selected(store, "pending", &store->candidate, &local);
        selected = store->candidate != NULL;
        if (status == ROTMOS_OK) {
            status = acknowledged(store, 0, &local);
            if (status == ROTMOS_OK)
                status = rotmos_acme_store_publish(store, &local);
            else if (status == ROTMOS_EOF) {
                free(store->candidate);
                store->candidate = NULL;
                selected = 0;
                status = ROTMOS_OK;
            }
            if (status == ROTMOS_OK && store->candidate == NULL)
                selected = 0;
        } else if (status == ROTMOS_EOF)
            status = ROTMOS_OK;
    }
    active = NULL;
    if (status == ROTMOS_OK) {
        status = rotmos_acme_selected(store, "active", &active, &local);
        if (status == ROTMOS_OK) {
            status = rotmos_acme_check(store, active, &local);
            store->candidate = active;
            if (status == ROTMOS_OK)
                status = acknowledged(store, 0, &local);
            store->candidate = NULL;
            if (status == ROTMOS_EOF)
                status = rotmos_fail(
                    &local, ROTMOS_INVALID, "acme_restore",
                    "selected ACME generation has no acknowledgement");
        } else if (status == ROTMOS_EOF)
            status = ROTMOS_OK;
    }
    if (status == ROTMOS_OK)
        status = rotmos_acme_sweep(store, &local);
    if (status == ROTMOS_OK)
        status = rotmos_acme_snapshot(
            store, active != NULL ? active : store->root, "generation-XXXXXX",
            &store->candidate, &local);
    free(active);
    if (status == ROTMOS_OK)
        status = state_callback(
            store, server, process, ROTMOS_ACME_RESTORE, NULL, store->candidate,
            server->config.timeouts.startup.disabled
                ? 0
                : server->config.timeouts.startup.milliseconds,
            &local);
    if (status == ROTMOS_OK)
        status = rotmos_acme_check(store, store->candidate, &local);
    if (status == ROTMOS_OK)
        status = acknowledged(store, 1, &local);
    if (status == ROTMOS_OK) {
        status = rotmos_acme_select(store, "pending", store->candidate, &local);
        if (status == ROTMOS_OK)
            selected = 1;
    }
    if (status == ROTMOS_OK)
        status = rotmos_acme_sync(store->descriptor, &local);
    if (status == ROTMOS_OK)
        status = rotmos_acme_store_publish(store, &local);
    if (status != ROTMOS_OK) {
        observe_error(store, server, process, NULL,
                      store->candidate != NULL ? store->candidate : store->root,
                      &local);
        if (!selected && store->candidate != NULL) {
            (void)rotmos_acme_remove(store, store->candidate);
            free(store->candidate);
            store->candidate = NULL;
        }
    }
    if (error != NULL)
        *error = local;
    return status;
}
