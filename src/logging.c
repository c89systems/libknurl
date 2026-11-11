#include "logging_internal.h"
#include "server_internal.h"
#include <stdlib.h>
#include <string.h>
void knurl_access_log_config_init(knurl_access_log_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->success_level = PSLOG_LEVEL_TRACE;
    config->client_error_level = PSLOG_LEVEL_WARN;
    config->server_error_level = PSLOG_LEVEL_ERROR;
}
static int valid_level(pslog_level level)
{
    return (level >= PSLOG_LEVEL_TRACE && level <= PSLOG_LEVEL_PANIC) ||
           level == PSLOG_LEVEL_DISABLED;
}
void knurl_logging_clear(knurl_server *server)
{
    free(server->impl->logging);
    server->impl->logging = NULL;
}
knurl_status knurl_server_set_pslog(knurl_server *server, pslog_logger *logger,
                                    const knurl_access_log_config *input,
                                    knurl_error *error)
{
    knurl_access_log_config config;
    struct knurl_logging *copy;
    size_t i, j;
    if (server == NULL)
        return knurl_fail(error, KNURL_INVALID, "server_set_pslog",
                          "server is required");
    if (knurl_server_running(server))
        return knurl_fail(error, KNURL_STATE, "server_set_pslog",
                          "configure logging outside a run");
    knurl_access_log_config_init(&config);
    if (input != NULL)
        config = *input;
    if (!valid_level(config.success_level) ||
        !valid_level(config.client_error_level) ||
        !valid_level(config.server_error_level) ||
        config.status_level_rule_count >
            KNURL_ACCESS_LOG_MAX_STATUS_LEVEL_RULES ||
        (config.status_level_rule_count != 0 &&
         config.status_level_rules == NULL))
        return knurl_fail(error, KNURL_INVALID, "server_set_pslog",
                          "invalid access levels or rule array");
    for (i = 0; i < config.status_level_rule_count; ++i) {
        if (config.status_level_rules[i].status < 100 ||
            config.status_level_rules[i].status > 599 ||
            !valid_level(config.status_level_rules[i].level))
            return knurl_fail(
                error, KNURL_INVALID, "server_set_pslog",
                "access rules require HTTP 100..599 and a valid pslog level");
        for (j = 0; j < i; ++j)
            if (config.status_level_rules[i].status ==
                config.status_level_rules[j].status)
                return knurl_fail(error, KNURL_CONFLICT, "server_set_pslog",
                                  "duplicate access status override");
    }
    copy = calloc(1, sizeof(*copy));
    if (copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "server_set_pslog",
                          "copying logging policy");
    copy->root = logger;
    copy->access = config;
    if (config.status_level_rule_count != 0)
        memcpy(copy->rules, config.status_level_rules,
               config.status_level_rule_count * sizeof(copy->rules[0]));
    copy->access.status_level_rules = copy->rules;
    knurl_logging_clear(server);
    server->impl->logging = copy;
    knurl_error_clear(error);
    return KNURL_OK;
}
