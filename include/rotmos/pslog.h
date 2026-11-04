#ifndef ROTMOS_PSLOG_H
#define ROTMOS_PSLOG_H
#include <rotmos/server.h>
#include <pslog.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ROTMOS_ACCESS_LOG_MAX_STATUS_LEVEL_RULES 32U
typedef struct rotmos_access_log_status_level_rule {
    unsigned int status;
    pslog_level level;
} rotmos_access_log_status_level_rule;
typedef struct rotmos_access_log_config {
    pslog_logger *logger;
    int logger_disabled;
    pslog_level success_level, client_error_level, server_error_level;
    const rotmos_access_log_status_level_rule *status_level_rules;
    size_t status_level_rule_count;
} rotmos_access_log_config;
ROTMOS_API void rotmos_access_log_config_init(rotmos_access_log_config *);
/* Inert outside-run registration. Both logger pointers are borrowed for every
 * run; their sinks must support native process callbacks. Rules are copied.
 * NULL configuration uses trace/warn/error; NULL root uses a library-owned
 * default logger during run. Without an override, access derives sub=http.
 * logger_disabled affects structured access only, independently of file logs.
 */
ROTMOS_API rotmos_status
rotmos_server_set_pslog(rotmos_server *, pslog_logger *,
                        const rotmos_access_log_config *, rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif
