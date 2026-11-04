#include "kore_internal.h"
#include "logging_internal.h"
#include <stdlib.h>
struct rotmos_log_runtime {
    pslog_logger *root, *access;
    int owned_root, owned_access;
};
static const char *effective(struct http_request *native, const char *peer,
                             char *buffer, size_t size)
{
    rotmos_request *request = native->hdlr_extra;
    (void)buffer;
    (void)size;
    return request != NULL && request->lifecycle_started &&
                   request->effective_address.length
               ? request->effective_address.data
               : peer;
}
static const char *correlation(struct http_request *native, char *buffer,
                               size_t size)
{
    rotmos_request *request = native->hdlr_extra;
    (void)buffer;
    (void)size;
    return request != NULL && request->lifecycle_started &&
                   request->correlation_id.length
               ? request->correlation_id.data
               : NULL;
}
rotmos_status rotmos_kore_logging_start(struct rotmos_runtime *runtime,
                                        rotmos_error *error)
{
    struct rotmos_log_runtime *log;
    const struct rotmos_logging *configured = runtime->server->logging;
    rotmos_access_log_config access;
    struct kore_accesslog_status_level_rule
        rules[ROTMOS_ACCESS_LOG_MAX_STATUS_LEVEL_RULES];
    pslog_config config;
    pslog_logger *base;
    size_t i;
    log = calloc(1, sizeof(*log));
    if (log == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "logging",
                           "allocating run logging state");
    runtime->logging = log;
    rotmos_access_log_config_init(&access);
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
                pslog_withf(base, "service=%s component=%s", "rotmos", "http");
            base->destroy(base);
        }
        if (log->root == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "logging",
                               "creating default pslog logger");
        log->owned_root = 1;
    }
    if (!access.logger_disabled) {
        log->access = access.logger;
        if (log->access == NULL) {
            log->access = pslog_withf(log->root, "sub=%s", "http");
            if (log->access == NULL)
                return rotmos_fail(error, ROTMOS_NOMEM, "logging",
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
    return ROTMOS_OK;
}
void rotmos_kore_logging_finish(struct rotmos_runtime *runtime)
{
    struct rotmos_log_runtime *log = runtime->logging;
    if (log == NULL)
        return;
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
