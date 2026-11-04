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
    status = rotmos_acme_runtime_directory(config, &runtime->acme_root, error);
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
    if (runtime->acme_root == NULL)
        return;
    kore_free(acme_email);
    kore_free(acme_provider);
    acme_email = kore_strdup(runtime->server->config.tls.acme.email);
    acme_provider = kore_strdup(runtime->server->config.tls.acme.directory_url);
}

void rotmos_kore_acme_clear(struct rotmos_runtime *runtime)
{
    free(runtime->acme_root);
    runtime->acme_root = NULL;
    rotmos_tls_material_clear(runtime->acme_ca, runtime->acme_ca_length);
    runtime->acme_ca = NULL;
    runtime->acme_ca_length = 0;
    free(runtime->acme_ca_directory);
    runtime->acme_ca_directory = NULL;
}

int rotmos_kore_acme_start(void *userdata)
{
    struct rotmos_runtime *runtime;
    runtime = userdata;
    if (runtime->acme_root == NULL)
        return 1;
    if (chdir(runtime->acme_root) != 0)
        return failed(runtime,
                      rotmos_fail(&runtime->error, ROTMOS_IO, "acme_start",
                                  "cannot enter ACME runtime directory"));
    return 1;
}

int rotmos_kore_acme_persist(void *userdata, const char *domain)
{
    struct rotmos_runtime *runtime;
    rotmos_status status;
    runtime = userdata;
    if (runtime->acme_root == NULL)
        return 1;
    operation(runtime, domain);
    status =
        rotmos_acme_state_call(&runtime->server->config.tls, runtime->server,
                               &runtime->server->process, ROTMOS_ACME_PERSIST,
                               domain, runtime->acme_root, &runtime->error);
    if (status != ROTMOS_OK)
        return failed(runtime, status);
    kore_library_worker_deadline(0);
    __atomic_store_n(&runtime->shared->acme_identity_valid, 0,
                     __ATOMIC_RELEASE);
    return 1;
}

int rotmos_kore_acme_request(void *userdata, struct kore_curl *request)
{
    struct rotmos_runtime *runtime;
    struct curl_blob blob;
    const curl_version_info_data *defaults;
    const char *directory;
    long timeout;
    runtime = userdata;
    /* Generic Kore curl initialization imports the current environment. Reset
     * both components before applying the captured ACME policy. */
    defaults = curl_version_info(CURLVERSION_NOW);
    if (defaults == NULL)
        return 0;
    directory = defaults->capath;
    if (directory == NULL && defaults->cainfo == NULL)
        directory = X509_get_default_cert_dir();
    if (curl_easy_setopt(request->handle, CURLOPT_CAINFO, defaults->cainfo) !=
            CURLE_OK ||
        curl_easy_setopt(request->handle, CURLOPT_CAPATH, directory) !=
            CURLE_OK)
        return 0;
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
