#ifndef ROTMOS_LOGGING_INTERNAL_H
#define ROTMOS_LOGGING_INTERNAL_H
#include <rotmos/pslog.h>
struct rotmos_logging {
    pslog_logger *root;
    rotmos_access_log_config access;
    rotmos_access_log_status_level_rule
        rules[ROTMOS_ACCESS_LOG_MAX_STATUS_LEVEL_RULES];
};
void rotmos_logging_clear(rotmos_server *);
struct rotmos_runtime;
struct rotmos_log_runtime;
rotmos_status rotmos_kore_logging_start(struct rotmos_runtime *,
                                        rotmos_error *);
void rotmos_kore_logging_finish(struct rotmos_runtime *);
#endif
