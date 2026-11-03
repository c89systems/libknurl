#ifndef ROTMOS_DISPATCH_H
#define ROTMOS_DISPATCH_H

#include <rotmos/http.h>
#include <rotmos/response.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rotmos_request_disposition {
    ROTMOS_REQUEST_MANAGED = 0,
    ROTMOS_REQUEST_REPLY,
    ROTMOS_REQUEST_WEBSOCKET,
    ROTMOS_REQUEST_TAKEOVER
} rotmos_request_disposition;

typedef struct rotmos_request_outcome {
    rotmos_status cause;
    rotmos_status body_cause;
    int body_complete;
    rotmos_u64 received_body_bytes;
    rotmos_u64 delivered_body_bytes;
    rotmos_response_outcome response;
} rotmos_request_outcome;

typedef rotmos_status (*rotmos_body_fn)(rotmos_request *request,
                                        const void *data, size_t length,
                                        size_t *consumed, void *userdata,
                                        rotmos_error *error);
typedef rotmos_status (*rotmos_complete_fn)(rotmos_request *, void *,
                                            rotmos_error *);
typedef void (*rotmos_writable_fn)(rotmos_request *, void *);
typedef void (*rotmos_request_close_fn)(rotmos_request *,
                                        const rotmos_request_outcome *, void *);
typedef rotmos_status (*rotmos_headers_fn)(rotmos_request *,
                                           rotmos_request_plan *, void *,
                                           rotmos_error *);

struct rotmos_request_plan {
    rotmos_request_disposition disposition;
    rotmos_body_policy body;
    rotmos_body_fn on_body;
    rotmos_complete_fn on_body_end;
    rotmos_writable_fn on_writable;
    rotmos_request_close_fn on_close;
    void *userdata;
};

typedef enum rotmos_dispatch_mode {
    ROTMOS_DISPATCH_ROUTER = 0,
    ROTMOS_DISPATCH_CALLBACK
} rotmos_dispatch_mode;

typedef struct rotmos_dispatch_config {
    rotmos_dispatch_mode mode;
    rotmos_headers_fn on_headers;
    void *userdata;
} rotmos_dispatch_config;

ROTMOS_API void rotmos_request_plan_init(rotmos_request_plan *plan);
ROTMOS_API void rotmos_dispatch_config_init(rotmos_dispatch_config *config);
/* Owner-loop only; resume schedules later delivery and coalesces. */
ROTMOS_API rotmos_status rotmos_request_body_resume(rotmos_request *request,
                                                    rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
