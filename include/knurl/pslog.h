#ifndef KNURL_PSLOG_H
#define KNURL_PSLOG_H
#include <knurl/server.h>
#include <pslog.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Maximum copied per-status structured access-log severity rules: 32. */
#define KNURL_ACCESS_LOG_MAX_STATUS_LEVEL_RULES 32U
/** Access log status level rule. */
typedef struct knurl_access_log_status_level_rule {
    /** HTTP status or operation status according to the containing type. */
    unsigned int status;
    /** Public pslog level assigned to the matching HTTP status. */
    pslog_level level;
} knurl_access_log_status_level_rule;
/** Configuration for access log. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_access_log_config {
    /** Borrowed public pslog logger; retain through server runs using it. */
    pslog_logger *logger;
    /** Nonzero disables structured access logging independently of file access
     * logs.
     */
    int logger_disabled;
    /** Structured access level for successful responses. */
    pslog_level success_level;
    /** Structured access level for client-error responses. */
    pslog_level client_error_level;
    /** Structured access level for server-error responses. */
    pslog_level server_error_level;
    /** Copied per-HTTP-status structured log level overrides. */
    const knurl_access_log_status_level_rule *status_level_rules;
    /** Number of status_level_rules entries. */
    size_t status_level_rule_count;
} knurl_access_log_config;
/** Initialize access log config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_access_log_config_init(knurl_access_log_config *);
/** Inert outside-run registration. Both logger pointers are borrowed for every
 * run; their sinks must support native process callbacks. Rules are copied.
 * NULL configuration uses trace/warn/error; NULL root uses a library-owned
 * default logger during run. Without an override, access derives sub=http.
 * logger_disabled affects structured access only, independently of file logs.
 */
KNURL_API knurl_status knurl_server_set_pslog(knurl_server *, pslog_logger *,
                                              const knurl_access_log_config *,
                                              knurl_error *);
#ifdef __cplusplus
}
#endif
#endif
