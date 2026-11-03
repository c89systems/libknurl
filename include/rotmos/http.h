#ifndef ROTMOS_HTTP_H
#define ROTMOS_HTTP_H

#include <rotmos/common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rotmos_request rotmos_request;
typedef struct rotmos_response rotmos_response;

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
