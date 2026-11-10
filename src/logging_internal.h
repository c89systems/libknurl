#ifndef KNURL_LOGGING_INTERNAL_H
#define KNURL_LOGGING_INTERNAL_H
#include <knurl/pslog.h>
struct knurl_logging {
    pslog_logger *root;
    knurl_access_log_config access;
    knurl_access_log_status_level_rule
        rules[KNURL_ACCESS_LOG_MAX_STATUS_LEVEL_RULES];
};
void knurl_logging_clear(knurl_server *);
struct knurl_runtime;
struct knurl_log_runtime;
knurl_status knurl_kore_logging_start(struct knurl_runtime *, knurl_error *);
void knurl_kore_logging_finish(struct knurl_runtime *);
void knurl_kore_logging_child(struct knurl_runtime *);
#endif
