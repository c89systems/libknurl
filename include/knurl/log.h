#ifndef KNURL_LOG_H
#define KNURL_LOG_H

#include <knurl/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Severity of a native server log record. */
typedef enum knurl_log_level {
    KNURL_LOG_TRACE = -1,
    KNURL_LOG_DEBUG = 0,
    KNURL_LOG_INFO,
    KNURL_LOG_WARN,
    KNURL_LOG_ERROR,
    KNURL_LOG_FATAL,
    KNURL_LOG_PANIC
} knurl_log_level;

/** Snapshot of a completed HTTP access record. Text is NUL-terminated. */
typedef struct knurl_log_access {
    const char *client_ip, *real_ip;
    const char *method, *path, *http_version;
    const char *referer, *user_agent, *correlation_id;
    unsigned int status;
    knurl_u64 bytes;
} knurl_log_access;

/** Borrowed for the callback only. Process identifies the record's origin.
 * Access and error are optional and carry typed details when applicable. */
typedef struct knurl_log_record {
    knurl_log_level level;
    const char *message, *process;
    /** Kore diagnostic priority. Zero for access records. */
    int native_priority;
    const knurl_log_access *access;
    const knurl_error *error;
} knurl_log_record;

typedef void (*knurl_log_fn)(const knurl_log_record *, void *);

/** Outside-run registration for native diagnostic and structured access logs.
 * Callbacks run on the parent owner thread and should return promptly. Worker
 * delivery is bounded and best effort. NULL selects the pslog adapter.
 * Userdata is borrowed through each run. */
KNURL_API knurl_status knurl_server_set_log_callback(knurl_server *,
                                                     knurl_log_fn, void *,
                                                     knurl_error *);

#ifdef __cplusplus
}
#endif
#endif
