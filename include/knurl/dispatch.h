#ifndef KNURL_DISPATCH_H
#define KNURL_DISPATCH_H

/** @file dispatch.h Public dispatch API. Expected failures return knurl_status
 * and optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/http.h>
#include <knurl/response.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for request disposition. */
typedef enum knurl_request_disposition {
    /** Headers dispatch disposition: managed. */
    KNURL_REQUEST_MANAGED = 0,
    /** Headers dispatch disposition: reply. */
    KNURL_REQUEST_REPLY,
    /** Headers dispatch disposition: websocket. */
    KNURL_REQUEST_WEBSOCKET,
    /** Headers dispatch disposition: takeover. */
    KNURL_REQUEST_TAKEOVER
} knurl_request_disposition;

/** Final request input/output accounting, borrowed during terminal callbacks.
 * Body failure and output failure remain distinct.
 */
typedef struct knurl_request_outcome {
    /** Portable terminal status; OK indicates successful completion. */
    knurl_status cause;
    /** Terminal request-input status, separate from output failure. */
    knurl_status body_cause;
    /** Nonzero after all framed input has been consumed. */
    int body_complete;
    /** Nonzero when the request became a WebSocket/tunnel. */
    int upgraded;
    /** Exact decoded input-body bytes received from transport, independently
     * of consumer acceptance. Excludes transfer framing and pipeline tails. */
    knurl_u64 received_body_bytes;
    /** Exact input-body bytes delivered to the managed consumer. */
    knurl_u64 delivered_body_bytes;
    /** Final response accounting attached to a request outcome. */
    knurl_response_outcome response;
} knurl_request_outcome;

/** Consume a positive prefix of this borrowed live chunk, or return AGAIN with
 * zero consumed. Resume schedules a later owner-loop delivery.
 */
typedef knurl_status (*knurl_body_fn)(knurl_request *request, const void *data,
                                      size_t length, size_t *consumed,
                                      void *userdata, knurl_error *error);
/** Owner-loop notification that all admitted input completed; does not imply
 * the response has drained.
 */
typedef knurl_status (*knurl_complete_fn)(knurl_request *, void *,
                                          knurl_error *);
/** Owner-loop notification to retry bounded live response output without
 * recursion.
 */
typedef void (*knurl_writable_fn)(knurl_request *, void *);
/** Terminal dispatch-plan cleanup with valid request metadata; borrowed
 * request/outcome expire after callback. Native acknowledgement may retire
 * source owners later.
 */
typedef void (*knurl_request_close_fn)(knurl_request *,
                                       const knurl_request_outcome *, void *);
/** Owner-loop headers dispatch before body/proxy allocation; fill the copied
 * request plan and retain callback state until terminal notification.
 */
typedef knurl_status (*knurl_headers_fn)(knurl_request *, knurl_request_plan *,
                                         void *, knurl_error *);

/** Headers-time request dispatch selection and callback state. */
struct knurl_request_plan {
    /** Managed body/reply/WebSocket/takeover dispatch selection. */
    knurl_request_disposition disposition;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    knurl_body_policy body;
    /** Live input callback; consumes a positive prefix or pauses with AGAIN and
     * zero.
     */
    knurl_body_fn on_body;
    /** Input-completion callback after the admitted body ends. */
    knurl_complete_fn on_body_end;
    /** Owner-loop callback when bounded output capacity becomes available. */
    knurl_writable_fn on_writable;
    /** Terminal owner-loop callback; its borrowed handle expires on return. */
    knurl_request_close_fn on_close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
};

/** Values for dispatch mode. */
typedef enum knurl_dispatch_mode {
    /** Request dispatch mode: router. */
    KNURL_DISPATCH_ROUTER = 0,
    /** Request dispatch mode: callback. */
    KNURL_DISPATCH_CALLBACK
} knurl_dispatch_mode;

/** Configuration for dispatch. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_dispatch_config {
    /** Selected policy mode; use the enclosing initializer for defaults. */
    knurl_dispatch_mode mode;
    /** Headers-time dispatch callback, before managed body delivery. */
    knurl_headers_fn on_headers;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_dispatch_config;

/** Initialize request plan with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_request_plan_init(knurl_request_plan *plan);
/** Initialize dispatch config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_dispatch_config_init(knurl_dispatch_config *config);
/** Owner-loop only; resume schedules later delivery and coalesces. */
KNURL_API knurl_status knurl_request_body_resume(knurl_request *request,
                                                 knurl_error *error);

#ifdef __cplusplus
}
#endif
#endif
