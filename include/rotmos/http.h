#ifndef ROTMOS_HTTP_H
#define ROTMOS_HTTP_H

#include <rotmos/common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rotmos_http_method {
    ROTMOS_HTTP_ANY = 0,
    ROTMOS_HTTP_GET,
    ROTMOS_HTTP_POST,
    ROTMOS_HTTP_PUT,
    ROTMOS_HTTP_PATCH,
    ROTMOS_HTTP_DELETE,
    ROTMOS_HTTP_HEAD,
    ROTMOS_HTTP_OPTIONS,
    ROTMOS_HTTP_PROPFIND,
    ROTMOS_HTTP_MKCOL,
    ROTMOS_HTTP_COPY,
    ROTMOS_HTTP_MOVE
} rotmos_http_method;

typedef unsigned int rotmos_http_methods;
#define ROTMOS_HTTP_METHOD_MASK(method) (1U << (unsigned int)(method))
#define ROTMOS_HTTP_METHODS_ALL 0xffeU
#define ROTMOS_HTTP_METHODS_GET ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_GET)
#define ROTMOS_HTTP_METHODS_POST ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_POST)
#define ROTMOS_HTTP_METHODS_PUT ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_PUT)
#define ROTMOS_HTTP_METHODS_PATCH ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_PATCH)
#define ROTMOS_HTTP_METHODS_DELETE ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_DELETE)
#define ROTMOS_HTTP_METHODS_HEAD ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_HEAD)
#define ROTMOS_HTTP_METHODS_OPTIONS ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_OPTIONS)
#define ROTMOS_HTTP_METHODS_PROPFIND                                           \
    ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_PROPFIND)
#define ROTMOS_HTTP_METHODS_MKCOL ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_MKCOL)
#define ROTMOS_HTTP_METHODS_COPY ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_COPY)
#define ROTMOS_HTTP_METHODS_MOVE ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_MOVE)

typedef enum rotmos_body_mode {
    ROTMOS_BODY_NONE = 0,
    ROTMOS_BODY_JSON,
    ROTMOS_BODY_BUFFERED,
    ROTMOS_BODY_STREAMING_UPLOAD,
    ROTMOS_BODY_MATERIALIZED,
    ROTMOS_BODY_STREAM
} rotmos_body_mode;

typedef struct rotmos_body_policy {
    rotmos_body_mode mode;
    rotmos_u64 max_bytes;
    size_t memory_buffer_limit_bytes;
    int disk_spool_disabled;
} rotmos_body_policy;

ROTMOS_API void rotmos_body_policy_init(rotmos_body_policy *policy);
ROTMOS_API void rotmos_body_policy_init_json(rotmos_body_policy *policy);
ROTMOS_API void rotmos_body_policy_init_buffered(rotmos_body_policy *policy);
ROTMOS_API void rotmos_body_policy_init_upload(rotmos_body_policy *policy);

typedef struct rotmos_sse_event {
    rotmos_bytes comment;
    rotmos_bytes id;
    rotmos_bytes event;
    rotmos_bytes data;
    rotmos_bytes frame;
    rotmos_u64 retry_ms;
    int has_comment;
    int has_id;
    int has_event;
    int has_data;
    int has_frame;
    int has_retry;
} rotmos_sse_event;

ROTMOS_API void rotmos_sse_event_init(rotmos_sse_event *event);
/* Encodes one event. Inputs must not overlap the destination. */
ROTMOS_API rotmos_status rotmos_sse_encode(const rotmos_sse_event *event,
                                           void *buffer, size_t capacity,
                                           size_t *written,
                                           rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
