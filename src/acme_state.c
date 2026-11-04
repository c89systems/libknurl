#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "acme_internal.h"
#include "loop_internal.h"
#include <rotmos/loop.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int rotmos_acme_domain_valid(const char *domain)
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

rotmos_status rotmos_acme_runtime_directory(const rotmos_tls_config *config,
                                            char **out, rotmos_error *error)
{
    struct stat info;
    *out = NULL;
    if (config->acme.state == NULL || config->acme.runtime_directory == NULL ||
        lstat(config->acme.runtime_directory, &info) != 0 ||
        !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 077) != 0 ||
        access(config->acme.runtime_directory, R_OK | W_OK | X_OK) != 0)
        return rotmos_fail(
            error, ROTMOS_INVALID, "acme_prepare",
            "provide a state callback and owned private runtime directory");
    *out = realpath(config->acme.runtime_directory, NULL);
    if (*out == NULL)
        return rotmos_fail(error, ROTMOS_IO, "acme_prepare",
                           "cannot resolve ACME runtime directory");
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_acme_state_call(const rotmos_tls_config *config,
                                     rotmos_server *server,
                                     const rotmos_process_context *process,
                                     rotmos_acme_event event,
                                     const char *domain, const char *directory,
                                     rotmos_error *error)
{
    rotmos_error local, observation;
    rotmos_status status;
    rotmos_bytes name, path;
    rotmos_u64 start, duration, deadline, now;
    int order;
    name.data = domain != NULL ? domain : "";
    name.length = strlen(name.data);
    path.data = directory;
    path.length = strlen(directory);
    rotmos_error_clear(&local);
    status = rotmos_loop_monotonic(&start, &local);
    if (status == ROTMOS_OK)
        status = config->acme.state(server, process, event, name, path,
                                    config->acme.userdata, &local);
    if (status < ROTMOS_OK || status == ROTMOS_AGAIN || status == ROTMOS_EOF ||
        status > ROTMOS_BACKEND)
        status = ROTMOS_INVALID;
    if (status == ROTMOS_OK && event == ROTMOS_ACME_PERSIST &&
        config->acme.persist_timeout_ms > 0) {
        (void)rotmos_u64_from_size((size_t)config->acme.persist_timeout_ms,
                                   &duration, NULL);
        if (rotmos_u64_add(start, duration, &deadline, &local) != ROTMOS_OK ||
            rotmos_loop_monotonic(&now, &local) != ROTMOS_OK)
            status = local.status;
        else {
            (void)rotmos_u64_compare(now, deadline, &order, NULL);
            if (order >= 0)
                status = rotmos_fail(
                    &local, ROTMOS_TIMEOUT, "acme_callback",
                    "ACME persistence callback exceeded its budget");
        }
    }
    if (status != ROTMOS_OK) {
        if (local.status != status)
            (void)rotmos_fail(&local, status, "acme_callback",
                              "ACME state callback failed");
        observation = local;
        (void)config->acme.state(server, process, ROTMOS_ACME_ERROR, name, path,
                                 config->acme.userdata, &observation);
    } else
        rotmos_error_clear(&local);
    if (error != NULL)
        *error = local;
    return status;
}
