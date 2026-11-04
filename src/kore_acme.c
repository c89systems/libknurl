#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "acme.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failed(struct rotmos_runtime *runtime, rotmos_status status)
{
    runtime->status = status;
    rotmos_kore_child_failure(runtime, &runtime->error);
    return 0;
}

static int observe_failure(struct rotmos_runtime *runtime, const char *domain,
                           rotmos_status status)
{
    rotmos_error cause;
    const char *directory;
    cause = runtime->error;
    directory = runtime->acme->candidate != NULL ? runtime->acme->candidate
                                                 : runtime->acme->work;
    (void)runtime->server->config.tls.acme.state(
        runtime->server, &runtime->server->process, ROTMOS_ACME_ERROR,
        rotmos_view(domain != NULL ? domain : ""),
        rotmos_view(directory != NULL ? directory : runtime->acme->root),
        runtime->server->config.tls.acme.userdata, &cause);
    return failed(runtime, status);
}

static void operation(struct rotmos_runtime *runtime, const char *domain)
{
    size_t i;
    size_t j;
    size_t identity;
    size_t name;
    identity = (size_t)-1;
    name = 0;
    for (i = 0;
         domain != NULL && i < runtime->server->config.tls.identity_count;
         ++i) {
        if (runtime->server->config.tls.identities[i].mode != ROTMOS_TLS_ACME)
            continue;
        for (j = 0; j < runtime->server->config.tls.identities[i].domain_count;
             ++j)
            if (rotmos_text_equal_case(
                    domain,
                    runtime->server->config.tls.identities[i].domains[j])) {
                identity = i;
                name = j;
            }
    }
    __atomic_store_n(&runtime->shared->acme_identity, identity,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&runtime->shared->acme_domain, name, __ATOMIC_RELEASE);
    __atomic_store_n(&runtime->shared->acme_identity_valid, 1,
                     __ATOMIC_RELEASE);
    kore_library_worker_deadline(
        (u_int32_t)runtime->server->config.tls.acme.persist_timeout_ms);
}

rotmos_status rotmos_kore_acme_prepare(struct rotmos_runtime *runtime,
                                       rotmos_error *error)
{
    const rotmos_tls_config *config;
    const char *file;
    const char *directory;
    const char *scheme;
    rotmos_tls_material material;
    SSL_CTX *trust;
    CURLU *url;
    char *parsed;
    struct stat info;
    rotmos_status status;
    size_t i;
    config = &runtime->server->config.tls;
    for (i = 0; i < config->identity_count; ++i)
        if (config->identities[i].mode == ROTMOS_TLS_ACME)
            break;
    if (i == config->identity_count)
        return ROTMOS_OK;
    status = rotmos_acme_store_new(config, KORE_DEFAULT_CIPHER_LIST,
                                   &runtime->acme, error);
    if (status != ROTMOS_OK)
        return status;
    url = curl_url();
    parsed = NULL;
    if (url == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "acme_prepare",
                           "cannot allocate ACME directory URL");
    status = ROTMOS_INVALID;
    if (curl_url_set(url, CURLUPART_URL, config->acme.directory_url, 0) ==
            CURLUE_OK &&
        curl_url_get(url, CURLUPART_SCHEME, &parsed, 0) == CURLUE_OK) {
        scheme = parsed;
        if (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0)
            status = ROTMOS_OK;
    }
    curl_free(parsed);
    curl_url_cleanup(url);
    if (status != ROTMOS_OK)
        return rotmos_fail(error, status, "acme_prepare",
                           "ACME directory URL requires HTTP or HTTPS");
    file = config->acme.ca_file != NULL ? config->acme.ca_file
                                        : getenv("SSL_CERT_FILE");
    directory = config->acme.ca_directory != NULL ? config->acme.ca_directory
                                                  : getenv("SSL_CERT_DIR");
    if (file != NULL && *file == '\0')
        file = NULL;
    if (directory != NULL && *directory == '\0')
        directory = NULL;
    if (directory != NULL) {
        if (stat(directory, &info) != 0 || !S_ISDIR(info.st_mode) ||
            access(directory, R_OK | X_OK) != 0)
            return rotmos_fail(error, ROTMOS_INVALID, "acme_trust",
                               "ACME CA directory must be readable");
        runtime->acme_ca_directory = realpath(directory, NULL);
        if (runtime->acme_ca_directory == NULL)
            return rotmos_fail(error, ROTMOS_IO, "acme_trust",
                               "cannot resolve ACME CA directory");
    }
    if (file == NULL && directory == NULL)
        return ROTMOS_OK;
    /* Enforce the input bound before asking OpenSSL to parse the prepared
     * file. Configuration/material inputs stay stable through preflight. */
    if (file != NULL) {
        rotmos_tls_material_init(&material);
        material.kind = ROTMOS_MATERIAL_FILE;
        material.path = file;
        status = rotmos_tls_material_read(&material, config->max_material_bytes,
                                          &runtime->acme_ca,
                                          &runtime->acme_ca_length, error);
        if (status != ROTMOS_OK)
            return status;
    }
    trust = SSL_CTX_new(TLS_client_method());
    if (trust == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "acme_trust",
                           "cannot prepare ACME trust");
    status = SSL_CTX_load_verify_locations(trust, file, directory)
                 ? ROTMOS_OK
                 : ROTMOS_INVALID;
    SSL_CTX_free(trust);
    if (status != ROTMOS_OK)
        return rotmos_fail(error, status, "acme_trust",
                           "invalid ACME CA file or directory");
    return status;
}

void rotmos_kore_acme_configure(struct rotmos_runtime *runtime)
{
    if (runtime->acme == NULL)
        return;
    kore_free(acme_email);
    kore_free(acme_provider);
    acme_email = kore_strdup(runtime->server->config.tls.acme.email);
    acme_provider = kore_strdup(runtime->server->config.tls.acme.directory_url);
}

void rotmos_kore_acme_clear(struct rotmos_runtime *runtime)
{
    rotmos_acme_store_free(runtime->acme);
    runtime->acme = NULL;
    rotmos_tls_material_clear(runtime->acme_ca, runtime->acme_ca_length);
    runtime->acme_ca = NULL;
    runtime->acme_ca_length = 0;
    free(runtime->acme_ca_directory);
    runtime->acme_ca_directory = NULL;
}

int rotmos_kore_acme_start(void *userdata)
{
    struct rotmos_runtime *runtime;
    rotmos_status status;
    runtime = userdata;
    if (runtime->acme == NULL)
        return 1;
    operation(runtime, NULL);
    status =
        rotmos_acme_store_recover(runtime->acme, runtime->server,
                                  &runtime->server->process, &runtime->error);
    if (status == ROTMOS_OK)
        status = rotmos_acme_store_begin(runtime->acme, &runtime->error);
    if (status == ROTMOS_OK && chdir(runtime->acme->work) != 0)
        status = rotmos_fail(&runtime->error, ROTMOS_IO, "acme_start",
                             "cannot enter private ACME working generation");
    if (status != ROTMOS_OK)
        return failed(runtime, status);
    /* Keep the budget armed through key generation and initial persistence. */
    return 1;
}

int rotmos_kore_acme_persist(void *userdata, const char *domain)
{
    struct rotmos_runtime *runtime;
    rotmos_status status;
    runtime = userdata;
    if (runtime->acme == NULL)
        return 1;
    operation(runtime, domain);
    status = rotmos_acme_store_stage(runtime->acme, &runtime->error);
    if (status != ROTMOS_OK)
        return observe_failure(runtime, domain, status);
    if (status == ROTMOS_OK)
        status = rotmos_acme_store_persist(runtime->acme, runtime->server,
                                           &runtime->server->process, domain,
                                           &runtime->error);
    if (status != ROTMOS_OK)
        return failed(runtime, status);
    status = rotmos_acme_store_publish(runtime->acme, &runtime->error);
    if (status != ROTMOS_OK)
        return observe_failure(runtime, domain, status);
    kore_library_worker_deadline(0);
    __atomic_store_n(&runtime->shared->acme_identity_valid, 0,
                     __ATOMIC_RELEASE);
    return 1;
}

int rotmos_kore_acme_install(void *userdata, struct kore_domain *domain,
                             const void *data, size_t length)
{
    struct rotmos_runtime *runtime;
    const unsigned char *bytes;
    size_t offset;
    ssize_t count;
    int fd;
    rotmos_status status;
    runtime = userdata;
    operation(runtime, domain->domain);
    if (length == 0 || length > runtime->server->config.tls.max_material_bytes)
        return observe_failure(
            runtime, domain->domain,
            rotmos_fail(&runtime->error, ROTMOS_LIMIT, "acme_install",
                        "ACME certificate exceeds material bound"));
    fd = open(domain->certfile,
              O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    status = fd < 0 ? ROTMOS_IO : ROTMOS_OK;
    bytes = data;
    offset = 0;
    while (status == ROTMOS_OK && offset < length) {
        count = write(fd, bytes + offset, length - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            status = ROTMOS_IO;
        else
            offset += (size_t)count;
    }
    if (fd >= 0 && close(fd) != 0)
        status = ROTMOS_IO;
    if (status != ROTMOS_OK)
        return observe_failure(
            runtime, domain->domain,
            rotmos_fail(&runtime->error, status, "acme_install",
                        "cannot finish private ACME candidate"));
    return rotmos_kore_acme_persist(runtime, domain->domain);
}

int rotmos_kore_acme_request(void *userdata, struct kore_curl *request)
{
    struct rotmos_runtime *runtime;
    struct curl_blob blob;
    long timeout;
    runtime = userdata;
    timeout =
        runtime->server->config.http.backend_curl_timeout_seconds != 0
            ? (long)runtime->server->config.http.backend_curl_timeout_seconds
            : 8;
    if (curl_easy_setopt(request->handle, CURLOPT_TIMEOUT, timeout) != CURLE_OK)
        return 0;
    if (runtime->acme_ca != NULL) {
        blob.data = runtime->acme_ca;
        blob.len = runtime->acme_ca_length;
        blob.flags = CURL_BLOB_COPY;
        if (curl_easy_setopt(request->handle, CURLOPT_CAINFO_BLOB, &blob) !=
            CURLE_OK)
            return 0;
    }
    if (runtime->acme_ca_directory != NULL &&
        curl_easy_setopt(request->handle, CURLOPT_CAPATH,
                         runtime->acme_ca_directory) != CURLE_OK)
        return 0;
    return 1;
}
