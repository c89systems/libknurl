#ifndef ROTMOS_DISPATCH_H
#define ROTMOS_DISPATCH_H

/** @file dispatch.h Public dispatch API. Expected failures return rotmos_status
 * and optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/http.h>
#include <rotmos/response.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for request disposition. */
typedef enum rotmos_request_disposition {
    /** Headers dispatch disposition: managed. */
    ROTMOS_REQUEST_MANAGED = 0,
    /** Headers dispatch disposition: reply. */
    ROTMOS_REQUEST_REPLY,
    /** Headers dispatch disposition: websocket. */
    ROTMOS_REQUEST_WEBSOCKET,
    /** Headers dispatch disposition: takeover. */
    ROTMOS_REQUEST_TAKEOVER
} rotmos_request_disposition;

/** Final request input/output accounting, borrowed during terminal callbacks.
 * Body failure and output failure remain distinct.
 */
typedef struct rotmos_request_outcome {
    /** Portable terminal status; OK indicates successful completion. */
    rotmos_status cause;
    /** Terminal request-input status, separate from output failure. */
    rotmos_status body_cause;
    /** Nonzero after all framed input has been consumed. */
    int body_complete;
    /** Nonzero when the request became a WebSocket/tunnel. */
    int upgraded;
    /** Exact input-body bytes received from transport. */
    rotmos_u64 received_body_bytes;
    /** Exact input-body bytes delivered to the managed consumer. */
    rotmos_u64 delivered_body_bytes;
    /** Final response accounting attached to a request outcome. */
    rotmos_response_outcome response;
} rotmos_request_outcome;

/** Consume a positive prefix of this borrowed live chunk, or return AGAIN with
 * zero consumed. Resume schedules a later owner-loop delivery.
 */
typedef rotmos_status (*rotmos_body_fn)(rotmos_request *request,
                                        const void *data, size_t length,
                                        size_t *consumed, void *userdata,
                                        rotmos_error *error);
/** Owner-loop notification that all admitted input completed; does not imply
 * the response has drained.
 */
typedef rotmos_status (*rotmos_complete_fn)(rotmos_request *, void *,
                                            rotmos_error *);
/** Owner-loop notification to retry bounded live response output without
 * recursion.
 */
typedef void (*rotmos_writable_fn)(rotmos_request *, void *);
/** Terminal dispatch-plan cleanup with valid request metadata; borrowed
 * request/outcome expire after callback. Native acknowledgement may retire
 * source owners later.
 */
typedef void (*rotmos_request_close_fn)(rotmos_request *,
                                        const rotmos_request_outcome *, void *);
/** Owner-loop headers dispatch before body/proxy allocation; fill the copied
 * request plan and retain callback state until terminal notification.
 */
typedef rotmos_status (*rotmos_headers_fn)(rotmos_request *,
                                           rotmos_request_plan *, void *,
                                           rotmos_error *);

/** Headers-time request dispatch selection and callback state. */
struct rotmos_request_plan {
    /** Managed body/reply/WebSocket/takeover dispatch selection. */
    rotmos_request_disposition disposition;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    rotmos_body_policy body;
    /** Live input callback; consumes a positive prefix or pauses with AGAIN and
     * zero.
     */
    rotmos_body_fn on_body;
    /** Input-completion callback after the admitted body ends. */
    rotmos_complete_fn on_body_end;
    /** Owner-loop callback when bounded output capacity becomes available. */
    rotmos_writable_fn on_writable;
    /** Terminal owner-loop callback; its borrowed handle expires on return. */
    rotmos_request_close_fn on_close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
};

/** Values for dispatch mode. */
typedef enum rotmos_dispatch_mode {
    /** Request dispatch mode: router. */
    ROTMOS_DISPATCH_ROUTER = 0,
    /** Request dispatch mode: callback. */
    ROTMOS_DISPATCH_CALLBACK
} rotmos_dispatch_mode;

/** Configuration for dispatch. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_dispatch_config {
    /** Selected policy mode; use the enclosing initializer for defaults. */
    rotmos_dispatch_mode mode;
    /** Headers-time dispatch callback, before managed body delivery. */
    rotmos_headers_fn on_headers;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_dispatch_config;

/** Initialize request plan with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_request_plan_init(rotmos_request_plan *plan);
/** Initialize dispatch config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_dispatch_config_init(rotmos_dispatch_config *config);
/** Owner-loop only; resume schedules later delivery and coalesces. */
ROTMOS_API rotmos_status rotmos_request_body_resume(rotmos_request *request,
                                                    rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
