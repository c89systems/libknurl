#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "acme_internal.h"
#include "loop_internal.h"
#include <knurl/loop.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int knurl_acme_domain_valid(const char *domain)
{
    size_t i, label = 0;
    unsigned char byte;
    if (domain == NULL || *domain == '\0' || strlen(domain) > 253)
        return 0;
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

knurl_status knurl_acme_runtime_directory(const knurl_tls_config *config,
                                          char **out, knurl_error *error)
{
    struct stat info;
    *out = NULL;
    if (config->acme.state == NULL || config->acme.runtime_directory == NULL ||
        lstat(config->acme.runtime_directory, &info) != 0 ||
        !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 077) != 0 ||
        access(config->acme.runtime_directory, R_OK | W_OK | X_OK) != 0)
        return knurl_fail(
            error, KNURL_INVALID, "acme_prepare",
            "provide a state callback and owned private runtime directory");
    *out = realpath(config->acme.runtime_directory, NULL);
    if (*out == NULL)
        return knurl_fail(error, KNURL_IO, "acme_prepare",
                          "cannot resolve ACME runtime directory");
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_acme_state_call(const knurl_tls_config *config,
                                   knurl_server *server,
                                   const knurl_process_context *process,
                                   knurl_acme_event event, const char *domain,
                                   const char *directory, knurl_error *error)
{
    knurl_error local, observation;
    knurl_status status;
    knurl_bytes name, path;
    knurl_u64 start, duration, deadline, now;
    int order;
    name.data = domain != NULL ? domain : "";
    name.length = strlen(name.data);
    path.data = directory;
    path.length = strlen(directory);
    knurl_error_clear(&local);
    status = knurl_loop_monotonic(&start, &local);
    if (status == KNURL_OK)
        status = config->acme.state(server, process, event, name, path,
                                    config->acme.userdata, &local);
    if (status < KNURL_OK || status == KNURL_AGAIN || status == KNURL_EOF ||
        status > KNURL_BACKEND)
        status = KNURL_INVALID;
    if (status == KNURL_OK && event == KNURL_ACME_PERSIST &&
        config->acme.persist_timeout_ms > 0) {
        (void)knurl_u64_from_size((size_t)config->acme.persist_timeout_ms,
                                  &duration, NULL);
        if (knurl_u64_add(start, duration, &deadline, &local) != KNURL_OK ||
            knurl_loop_monotonic(&now, &local) != KNURL_OK)
            status = local.status;
        else {
            (void)knurl_u64_compare(now, deadline, &order, NULL);
            if (order >= 0)
                status =
                    knurl_fail(&local, KNURL_TIMEOUT, "acme_callback",
                               "ACME persistence callback exceeded its budget");
        }
    }
    if (status != KNURL_OK) {
        if (local.status != status)
            (void)knurl_fail(&local, status, "acme_callback",
                             "ACME state callback failed");
        observation = local;
        (void)config->acme.state(server, process, KNURL_ACME_ERROR, name, path,
                                 config->acme.userdata, &observation);
    } else
        knurl_error_clear(&local);
    if (error != NULL)
        *error = local;
    return status;
}
