#include "kore_internal.h"
#include "logging_internal.h"
#include "access_log_file.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct knurl_log_runtime {
    pslog_logger *root, *access;
    knurl_access_log_config policy;
    knurl_log_fn callback;
    void *userdata;
    int access_min_level;
    int owned_root, owned_access;
    struct knurl_access_file *file;
    const knurl_http_config *file_config;
    int file_failed;
    long file_error;
};

struct access_packet {
    u_int64_t bytes;
    u_int16_t status;
};

static struct knurl_log_runtime *parent_log;

static knurl_log_level diagnostic_level(int priority)
{
    switch (priority) {
    case KORE_LOG_TRACE:
        return KNURL_LOG_TRACE;
    case LOG_DEBUG:
        return KNURL_LOG_DEBUG;
    case LOG_WARNING:
        return KNURL_LOG_WARN;
    case LOG_ERR:
    case LOG_CRIT:
    case LOG_ALERT:
    case LOG_EMERG:
        return KNURL_LOG_ERROR;
    default:
        return KNURL_LOG_INFO;
    }
}

static void diagnostic(struct knurl_log_runtime *log, int priority,
                       const char *process, const char *message,
                       const knurl_error *error)
{
    knurl_log_record record;
    pslog_field fields[6];
    size_t count = 0;
    if (log == NULL)
        return;
    memset(&record, 0, sizeof(record));
    record.level = diagnostic_level(priority);
    record.message = message;
    record.process = process;
    record.native_priority = priority;
    record.error = error;
    if (log->callback != NULL) {
        log->callback(&record, log->userdata);
        return;
    }
    if (log->root == NULL)
        return;
    fields[count++] = pslog_str("proc", process);
    fields[count++] = pslog_i64("prio", priority);
    if (error != NULL) {
        fields[count++] =
            pslog_str("status", knurl_status_string(error->status));
        fields[count++] = pslog_str("operation", error->operation);
        fields[count++] = pslog_str("error", error->message);
        fields[count++] = pslog_str("detail", error->detail);
    }
    pslog_fields(log->root, (pslog_level)record.level, message, fields, count);
}

static void runtime_record(int priority, const char *process,
                           const char *message, void *data)
{
    diagnostic(data, priority, process, message, NULL);
}

void knurl_kore_log_failure(struct knurl_runtime *runtime, const char *process,
                            const knurl_error *error)
{
    diagnostic(runtime->logging, LOG_ERR, process, error->message, error);
}

void knurl_request_log_handler_error(knurl_request *request,
                                     const knurl_error *error)
{
    (void)request;
    (void)kore_log_forward(KORE_MSG_HANDLER_ERROR, error, sizeof(*error));
}

static void handler_error(struct kore_msg *msg, const void *data)
{
    knurl_error error;
    if (parent_log == NULL || msg->length != sizeof(error))
        return;
    memcpy(&error, data, sizeof(error));
    error.operation[sizeof(error.operation) - 1] = '\0';
    error.message[sizeof(error.message) - 1] = '\0';
    error.detail[sizeof(error.detail) - 1] = '\0';
    diagnostic(parent_log, LOG_ERR, kore_worker_name(msg->src),
               "application handler failed", &error);
}

static pslog_level access_level(const struct knurl_log_runtime *log,
                                unsigned int status)
{
    size_t i;
    for (i = 0; i < log->policy.status_level_rule_count; ++i) {
        if (log->policy.status_level_rules[i].status == status)
            return log->policy.status_level_rules[i].level;
    }
    if (status >= 500)
        return log->policy.server_error_level;
    if (status >= 400)
        return log->policy.client_error_level;
    return log->policy.success_level;
}

static void queue_access(const struct kore_accesslog_record *record, void *data)
{
    const struct knurl_log_runtime *log = data;
    const char *text[8];
    size_t lengths[sizeof(text) / sizeof(text[0])];
    size_t i, length = sizeof(struct access_packet);
    struct access_packet header;
    unsigned char *packet, *slot;
    pslog_level level = access_level(log, record->status);
    if (level == PSLOG_LEVEL_DISABLED || (int)level < log->access_min_level)
        return;
    text[0] = record->client_ip;
    text[1] = record->real_ip;
    text[2] = record->method;
    text[3] = record->path;
    text[4] = record->http_version;
    text[5] = record->referer;
    text[6] = record->user_agent;
    text[7] = record->correlation_id;
    for (i = 0; i < sizeof(text) / sizeof(text[0]); ++i) {
        if (text[i] == NULL)
            text[i] = "";
        lengths[i] = strlen(text[i]) + 1;
        if (lengths[i] > KORE_ACCESS_LOG_MAX - length)
            return;
        length += lengths[i];
    }
    packet = malloc(length);
    if (packet == NULL)
        return;
    memset(&header, 0, sizeof(header));
    header.bytes = record->bytes;
    header.status = record->status;
    memcpy(packet, &header, sizeof(header));
    slot = packet + sizeof(header);
    for (i = 0; i < sizeof(text) / sizeof(text[0]); ++i) {
        memcpy(slot, text[i], lengths[i]);
        slot += lengths[i];
    }
    (void)kore_log_forward(KORE_MSG_ACCESS_LOG, packet, length);
    free(packet);
}

static void access_record(struct kore_msg *msg, const void *data)
{
    struct knurl_log_runtime *log = parent_log;
    struct access_packet header;
    knurl_log_access access;
    const char **fields[8];
    const char *cursor = data, *end, *nul;
    size_t i;
    pslog_level level;
    if (log == NULL || log->policy.logger_disabled ||
        msg->length < sizeof(header) || msg->length > KORE_ACCESS_LOG_MAX)
        return;
    memcpy(&header, cursor, sizeof(header));
    if (header.status < 100 || header.status > 599)
        return;
    level = access_level(log, header.status);
    if (level == PSLOG_LEVEL_DISABLED)
        return;
    fields[0] = &access.client_ip;
    fields[1] = &access.real_ip;
    fields[2] = &access.method;
    fields[3] = &access.path;
    fields[4] = &access.http_version;
    fields[5] = &access.referer;
    fields[6] = &access.user_agent;
    fields[7] = &access.correlation_id;
    end = cursor + msg->length;
    cursor += sizeof(header);
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        nul = memchr(cursor, '\0', (size_t)(end - cursor));
        if (nul == NULL)
            return;
        *fields[i] = cursor;
        cursor = nul + 1;
    }
    if (cursor != end)
        return;
    access.status = header.status;
    access.bytes.high = (unsigned long)(header.bytes >> 32);
    access.bytes.low = (unsigned long)(header.bytes & 0xffffffffU);
    if (log->callback != NULL) {
        knurl_log_record record;
        memset(&record, 0, sizeof(record));
        record.level = (knurl_log_level)level;
        record.message = "http request";
        record.process = kore_worker_name(msg->src);
        record.access = &access;
        log->callback(&record, log->userdata);
    } else if (log->access != NULL) {
        pslog_field output[10];
        size_t count = 9;
        output[0] = pslog_str("client_ip", access.client_ip);
        output[1] = pslog_str("real_ip", access.real_ip);
        output[2] = pslog_str("method", access.method);
        output[3] = pslog_str("path", access.path);
        output[4] = pslog_str("http_version", access.http_version);
        output[5] = pslog_u64("status", access.status);
        output[6] = pslog_u64("bytes", (pslog_uint64)header.bytes);
        output[7] = pslog_str("referer", access.referer);
        output[8] = pslog_str("user_agent", access.user_agent);
        if (*access.correlation_id != '\0')
            output[count++] = pslog_str("cid", access.correlation_id);
        pslog_fields(log->access, level, "http request", output, count);
    }
}

void knurl_kore_logging_messages(void)
{
    kore_msg_register(KORE_MSG_ACCESS_LOG, access_record);
    kore_msg_register(KORE_MSG_HANDLER_ERROR, handler_error);
}

static void file_failure(struct knurl_log_runtime *log,
                         const knurl_error *error, const char *action)
{
    pslog_field fields[5];
    if (!log->file_failed || log->file_error != error->native_code) {
        if (log->callback != NULL)
            diagnostic(log, LOG_ERR, "parent", "access.log file output failed",
                       error);
        else {
            fields[0] = pslog_str("operation", error->operation);
            fields[1] = pslog_str("error", error->message);
            fields[2] =
                pslog_str("detail", error->native_code != 0
                                        ? strerror((int)error->native_code)
                                        : knurl_status_string(error->status));
            fields[3] = pslog_str("action", action);
            fields[4] = pslog_str("path", log->file_config->access_log_path);
            pslog_error(log->root, "access.log file output failed", fields, 5);
        }
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
    pslog_config config;
    pslog_logger *base;
    knurl_status status;
    log = calloc(1, sizeof(*log));
    if (log == NULL)
        return knurl_fail(error, KNURL_NOMEM, "logging",
                          "allocating run logging state");
    runtime->logging = log;
    log->callback = runtime->server->impl->log_callback;
    log->userdata = runtime->server->impl->log_userdata;
    log->access_min_level = PSLOG_LEVEL_TRACE;
    knurl_access_log_config_init(&log->policy);
    if (configured != NULL) {
        log->root = configured->root;
        log->policy = configured->access;
    }
    if (log->callback == NULL && log->root == NULL) {
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
    if (log->callback == NULL && !log->policy.logger_disabled) {
        log->access = log->policy.logger;
        if (log->access == NULL) {
            log->access = pslog_withf(log->root, "sub=%s", "http");
            if (log->access == NULL)
                return knurl_fail(error, KNURL_NOMEM, "logging",
                                  "deriving access logger");
            log->owned_access = 1;
            if (log->owned_root)
                log->access_min_level = PSLOG_LEVEL_INFO;
        }
    }
    parent_log = log;
    kore_log_set_callback(runtime_record, log);
    if (!log->policy.logger_disabled)
        kore_accesslog_set_callback(queue_access, log);
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
    parent_log = NULL;
    if (runtime->logging != NULL) {
        (void)knurl_access_file_close(runtime->logging->file);
        runtime->logging->file = NULL;
        runtime->logging->file_config = NULL;
        runtime->logging->owned_root = runtime->logging->owned_access = 0;
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
    kore_accesslog_set_callback(NULL, NULL);
    kore_accesslog_set_real_ip_resolver(NULL);
    kore_accesslog_set_correlation_id_resolver(NULL);
    kore_log_set_callback(NULL, NULL);
    parent_log = NULL;
    if (log->owned_access)
        log->access->destroy(log->access);
    if (log->owned_root)
        log->root->destroy(log->root);
    free(log);
    runtime->logging = NULL;
}
