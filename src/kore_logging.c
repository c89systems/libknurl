#include "kore_internal.h"
#include "logging_internal.h"
#include "access_log_file.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct knurl_log_runtime {
    pslog_logger *root, *access;
    int owned_root, owned_access;
    struct knurl_access_file *file;
    const knurl_http_config *file_config;
    int file_failed;
    long file_error;
};
static void file_failure(struct knurl_log_runtime *log,
                         const knurl_error *error, const char *action)
{
    pslog_field fields[5];
    if (!log->file_failed || log->file_error != error->native_code) {
        fields[0] = pslog_str("operation", error->operation);
        fields[1] = pslog_str("error", error->message);
        fields[2] =
            pslog_str("detail", error->native_code != 0
                                    ? strerror((int)error->native_code)
                                    : knurl_status_string(error->status));
        fields[3] = pslog_str("action", action);
        fields[4] = pslog_str("path", log->file_config != NULL
                                          ? log->file_config->access_log_path
                                          : "-");
        /* Startup may precede Kore's allocator initialization, and shutdown
         * follows its cleanup. The already-owned root logger is valid in both.
         */
        pslog_error(log->root, "access.log file output failed", fields, 5);
    }
    log->file_failed = 1;
    log->file_error = error->native_code;
}
static void file_record(const void *bytes, size_t length, void *data)
{
    struct knurl_log_runtime *log = data;
    knurl_error error;
    knurl_status status = KNURL_OK;
    if (log->file_config == NULL || length == 0)
        return;
    if (log->file == NULL)
        status = knurl_access_file_open(log->file_config, &log->file, &error);
    if (status == KNURL_OK)
        status = knurl_access_file_write(log->file, bytes, length, &error);
    if (status != KNURL_OK)
        file_failure(log, &error, "record dropped; will retry");
    else
        log->file_failed = 0;
}
static const char *effective(struct http_request *native, const char *peer,
                             char *buffer, size_t size)
{
    knurl_request *request = native->hdlr_extra;
    (void)buffer;
    (void)size;
    return request != NULL && request->impl->lifecycle_started &&
                   request->impl->effective_address.length
               ? request->impl->effective_address.data
               : peer;
}
static const char *correlation(struct http_request *native, char *buffer,
                               size_t size)
{
    knurl_request *request = native->hdlr_extra;
    (void)buffer;
    (void)size;
    return request != NULL && request->impl->lifecycle_started &&
                   request->impl->correlation_id.length
               ? request->impl->correlation_id.data
               : NULL;
}
knurl_status knurl_kore_logging_start(struct knurl_runtime *runtime,
                                      knurl_error *error)
{
    struct knurl_log_runtime *log;
    const struct knurl_logging *configured = runtime->server->impl->logging;
    knurl_access_log_config access;
    struct kore_accesslog_status_level_rule
        rules[KNURL_ACCESS_LOG_MAX_STATUS_LEVEL_RULES];
    pslog_config config;
    pslog_logger *base;
    knurl_status status;
    size_t i;
    log = calloc(1, sizeof(*log));
    if (log == NULL)
        return knurl_fail(error, KNURL_NOMEM, "logging",
                          "allocating run logging state");
    runtime->logging = log;
    knurl_access_log_config_init(&access);
    if (configured != NULL) {
        log->root = configured->root;
        access = configured->access;
    }
    if (log->root == NULL) {
        pslog_default_config(&config);
        config.mode = PSLOG_MODE_JSON;
        config.min_level = PSLOG_LEVEL_INFO;
        config.output = pslog_output_from_fp(stdout, 0);
        base = pslog_new(&config);
        if (base != NULL) {
            log->root =
                pslog_withf(base, "service=%s component=%s", "knurl", "http");
            base->destroy(base);
        }
        if (log->root == NULL)
            return knurl_fail(error, KNURL_NOMEM, "logging",
                              "creating default pslog logger");
        log->owned_root = 1;
    }
    if (!access.logger_disabled) {
        log->access = access.logger;
        if (log->access == NULL) {
            log->access = pslog_withf(log->root, "sub=%s", "http");
            if (log->access == NULL)
                return knurl_fail(error, KNURL_NOMEM, "logging",
                                  "deriving access logger");
            log->owned_access = 1;
        }
    }
    for (i = 0; i < access.status_level_rule_count; ++i) {
        rules[i].status = (u_int16_t)access.status_level_rules[i].status;
        rules[i].level = access.status_level_rules[i].level;
    }
    kore_log_set_logger(log->root);
    kore_accesslog_set_logger(log->access);
    kore_accesslog_set_levels(access.success_level, access.client_error_level,
                              access.server_error_level, rules,
                              access.status_level_rule_count);
    kore_accesslog_set_real_ip_resolver(effective);
    kore_accesslog_set_correlation_id_resolver(correlation);
    if (runtime->server->impl->config.http.access_log_path != NULL) {
        log->file_config = &runtime->server->impl->config.http;
        status = knurl_access_file_open(log->file_config, &log->file, error);
        if (status == KNURL_IO && error != NULL &&
            error->native_code == ENOSPC) {
            file_failure(log, error,
                         "will retry; HTTP service remains available");
            status = KNURL_OK;
            knurl_error_clear(error);
        }
        if (status == KNURL_IO) {
            if (error != NULL)
                error->status = KNURL_BACKEND;
            return KNURL_BACKEND;
        }
        if (status != KNURL_OK)
            return status;
        kore_accesslog_set_file_sink(file_record, log);
    }
    return KNURL_OK;
}
void knurl_kore_logging_child(struct knurl_runtime *runtime)
{
    if (runtime->logging != NULL) {
        (void)knurl_access_file_close(runtime->logging->file);
        runtime->logging->file = NULL;
        runtime->logging->file_config = NULL;
    }
}
void knurl_kore_logging_finish(struct knurl_runtime *runtime)
{
    struct knurl_log_runtime *log = runtime->logging;
    knurl_error error;
    int code;
    if (log == NULL)
        return;
    kore_accesslog_set_file_sink(NULL, NULL);
    code = knurl_access_file_close(log->file);
    if (code != 0) {
        knurl_fail(&error, KNURL_IO, "access_log_close",
                   "cannot close access.log");
        error.native_code = code;
        error.source = KNURL_ERROR_SYSTEM;
        file_failure(log, &error, "file output closed");
    }
    kore_accesslog_set_logger(NULL);
    kore_accesslog_set_real_ip_resolver(NULL);
    kore_accesslog_set_correlation_id_resolver(NULL);
    kore_accesslog_set_levels(PSLOG_LEVEL_TRACE, PSLOG_LEVEL_WARN,
                              PSLOG_LEVEL_ERROR, NULL, 0);
    kore_log_set_logger(NULL);
    if (log->owned_access)
        log->access->destroy(log->access);
    if (log->owned_root)
        log->root->destroy(log->root);
    free(log);
    runtime->logging = NULL;
}
