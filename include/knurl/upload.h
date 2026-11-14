#ifndef KNURL_UPLOAD_H
#define KNURL_UPLOAD_H
#include <knurl/router.h>
#include <knurl/request.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Owner-loop callbacks; write consumes a positive prefix, or AGAIN with zero.
 * Request body_resume schedules another delivery. Never block these callbacks.
 * open runs only after framing/length/body-limit admission, including for an
 * empty body. Open is optional; omitting it supplies NULL state. Successful
 * open (or its omission) owns one close notification with valid request
 * metadata. On open failure the callback cleans its own partial state.
 * finish selects a staged reply before the normal after hooks run. */
typedef knurl_status (*knurl_upload_open_fn)(knurl_request *, void *userdata,
                                             void **state, knurl_error *);
/** Owner-loop live upload chunk, valid only during the call. Consume a positive
 * prefix or AGAIN with zero; body_resume schedules later delivery. Never
 * block.
 */
typedef knurl_status (*knurl_upload_write_fn)(knurl_request *, const void *,
                                              size_t length, size_t *consumed,
                                              void *state, void *userdata,
                                              knurl_error *);
/** Owner-loop successful input-completion callback; select the staged reply
 * before normal after-hooks run.
 */
typedef knurl_status (*knurl_upload_finish_fn)(knurl_request *, knurl_reply *,
                                               void *state, void *userdata,
                                               knurl_error *);
/** Exactly-once owner-loop cleanup after successful upload open (or omitted
 * opener), on success/failure with valid request metadata. Caller owns state
 * cleanup.
 */
typedef void (*knurl_upload_close_fn)(knurl_request *, knurl_status outcome,
                                      void *state, void *userdata);
/** Configuration for upload stream. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_upload_stream_config {
    /** Single HTTP method; methods supplies the explicit multi-method mask when
     * nonzero.
     */
    knurl_http_method method;
    /** HTTP method mask; zero uses this operation's single-method/default
     * policy.
     */
    knurl_http_methods methods;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    knurl_route_path_kind path_kind;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    knurl_body_policy body;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    knurl_route_hooks hooks;
    /** Optional retained route ACL. */
    knurl_acl *acl;
    /** Resource/state opener; failed construction must clean its own partial
     * state.
     */
    knurl_upload_open_fn open;
    /** Bounded upload write callback; consumes a positive prefix or AGAIN with
     * zero.
     */
    knurl_upload_write_fn write;
    /** Owner-loop upload completion callback; selects a staged reply before
     * after-hooks.
     */
    knurl_upload_finish_fn finish;
    /** Terminal cleanup callback; retain its context until actual retirement.
     */
    knurl_upload_close_fn close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_upload_stream_config;
/** Initialize upload stream config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_upload_stream_config_init(knurl_upload_stream_config *);
/** Copies declarations; callback userdata remains caller-owned. Registration
 * invokes no callbacks and opens no resources. Body mode is STREAMING_UPLOAD
 * or STREAM; both use the same bounded live input delivery. */
KNURL_API knurl_status
knurl_router_upload_stream(knurl_router *, const knurl_upload_stream_config *,
                           knurl_route **, knurl_error *);
/** Register a live chunk upload route under the group prefix. Owner-loop
 * callbacks consume prefixes; AGAIN pauses until body_resume. Userdata remains
 * borrowed.
 */
KNURL_API knurl_status knurl_route_group_upload_stream(
    knurl_route_group *, const knurl_upload_stream_config *, knurl_route **,
    knurl_error *);
/** Immutable copied request metadata borrowed during a native handler. Text has
 * trailing NUL bytes and explicit lengths preserve duplicates/empty values. No
 * live request/backend/Lua handles enter native work.
 */
typedef struct knurl_upload_metadata {
    /** Parsed method of the copied request metadata; immutable during the
     * native handler.
     */
    knurl_http_method method;
    /** Copied raw method token, borrowed during a native upload handler. */
    knurl_bytes method_token;
    /** OpenAPI version text or protocol version selector according to the
     * containing type.
     */
    knurl_bytes version;
    /** Copied raw request target, borrowed during a native upload handler. */
    knurl_bytes target;
    /** Copied undecoded request path, borrowed during a native upload handler.
     */
    knurl_bytes raw_path;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    knurl_bytes path;
    /** Copied raw query, borrowed during a native upload handler. */
    knurl_bytes query;
    /** Copied inbound Host/authority, borrowed during a native upload handler.
     */
    knurl_bytes authority;
    /** Real downstream TCP peer endpoint, independent of forwarded-address
     * policy.
     */
    knurl_endpoint peer;
    /** Accepted local endpoint. */
    knurl_endpoint local;
    /** Resolved client address; does not replace the real TCP peer. */
    knurl_bytes effective_address;
    /** Host-resolved request identifier; empty when no resolver is configured.
     */
    knurl_bytes correlation_id;
    /** Ordered header pairs; repeated names preserve order. */
    const knurl_header *headers;
    /** Ordered decoded query pairs in a native upload metadata snapshot. */
    const knurl_header *queries;
    /** Named decoded route captures in a native upload metadata snapshot. */
    const knurl_header *params;
    /** Number of ordered header pairs. */
    size_t header_count;
    /** Number of decoded query pairs. */
    size_t query_count;
    /** Number of decoded path captures. */
    size_t param_count;
    /** Zero-based configured listener index. */
    size_t listener_index;
    /** Exact declared request Content-Length when has_content_length is
     * nonzero.
     */
    knurl_u64 content_length;
    /** Nonzero when Content-Length was declared, including an explicit zero. */
    int has_content_length;
    /** Nonzero when this downstream connection uses TLS. */
    int tls;
} knurl_upload_metadata;
/** Runs once on a default-stack native thread. Consume the admitted input
 * through the borrowed reader; reset, destroy and ownership transfer are
 * rejected. Manual reads block only this native thread. Cancellation is
 * cooperative via source_read/source_cancelled. Result is an independent staged
 * reply, seeded with before-hook headers and exclusively borrowed until
 * callback return. Select a result; after hooks and publication run on the
 * owner loop after ACK. Do not access Lua or retain metadata/reader/result past
 * callback return. */
typedef knurl_status (*knurl_upload_reader_fn)(const knurl_upload_metadata *,
                                               knurl_source *reader,
                                               knurl_reply *result,
                                               void *userdata, knurl_error *);
/** Optional host request facade. Acquire runs on the owner loop after metadata
 * is copied and before native work. Its state replaces handler userdata.
 * Metadata and state remain valid through native ACK and request cleanup;
 * release runs once on the owner loop after both. Failed acquire cleans its
 * own partial state. Supply both callbacks or neither. Live core/backend/Lua
 * handles must not be used by native handlers. */
typedef knurl_status (*knurl_upload_reader_acquire_fn)(
    knurl_request *, const knurl_upload_metadata *, void *userdata,
    void **state, knurl_error *);
/** Owner-loop release of host-acquired native-handler state after both native
 * acknowledgement and request cleanup. No live Core/Lua handles may be used by
 * native handlers.
 */
typedef void (*knurl_upload_reader_release_fn)(void *state);
/** Configuration for upload reader. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_upload_reader_config {
    /** Single HTTP method; methods supplies the explicit multi-method mask when
     * nonzero.
     */
    knurl_http_method method;
    /** HTTP method mask; zero uses this operation's single-method/default
     * policy.
     */
    knurl_http_methods methods;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    knurl_route_path_kind path_kind;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    knurl_body_policy body;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    knurl_route_hooks hooks;
    /** Optional retained route ACL. */
    knurl_acl *acl;
    /** Bounded live-upload queue/chunk capacity; zero selects the operation's
     * documented default.
     */
    size_t buffer_bytes;
    /** Application handler invoked under this route's documented execution and
     * lifetime rules.
     */
    knurl_upload_reader_fn handler;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Owner-loop host-facade acquisition after request metadata is copied.
     * Supplies retained native-handler state; must be paired with release. No
     * live Core/backend/Lua handle may enter native work.
     */
    knurl_upload_reader_acquire_fn acquire;
    /** Release host-acquired handler state once on the owner loop after both
     * native acknowledgement and request cleanup.
     */
    knurl_upload_reader_release_fn release;
} knurl_upload_reader_config;
/** Configuration for upload file. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_upload_file_config {
    /** Single HTTP method; methods supplies the explicit multi-method mask when
     * nonzero.
     */
    knurl_http_method method;
    /** HTTP method mask; zero uses this operation's single-method/default
     * policy.
     */
    knurl_http_methods methods;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    knurl_route_path_kind path_kind;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    knurl_body_policy body;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    knurl_route_hooks hooks;
    /** Optional retained route ACL. */
    knurl_acl *acl;
    /** Explicit file destination/source path copied at registration; opening
     * follows the operation's admission/submission timing.
     */
    const char *file_path;
} knurl_upload_file_config;
/** Initialize upload reader config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_upload_reader_config_init(knurl_upload_reader_config *);
/** Initialize upload file config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_upload_file_config_init(knurl_upload_file_config *);
/** Bounded live queue, at most buffer_bytes (zero selects 256 KiB) and OS pipe
 * capacity. No materialization/spool or owner-loop wait/join. Caller userdata
 * stays alive until server run returns, including native cancellation ACK. */
KNURL_API knurl_status
knurl_router_upload_reader(knurl_router *, const knurl_upload_reader_config *,
                           knurl_route **, knurl_error *);
/** Register a bounded native-reader upload route under the group prefix. Native
 * handlers use copied metadata and a borrowed reader; retain userdata through
 * cancellation acknowledgement.
 */
KNURL_API knurl_status knurl_route_group_upload_reader(
    knurl_route_group *, const knurl_upload_reader_config *, knurl_route **,
    knurl_error *);
/** Explicit caller artifact, opened/truncated only after admission. Incremental
 * writes and flush/close run natively; partial output remains on failure.
 * Success matches Vectis: 200 text/plain with the decimal uploaded byte count.
 */
KNURL_API knurl_status
knurl_router_upload_file(knurl_router *, const knurl_upload_file_config *,
                         knurl_route **, knurl_error *);
/** Register a native incremental file upload under the group prefix. The
 * explicit destination opens after admission; partial files remain caller
 * artifacts.
 */
KNURL_API knurl_status knurl_route_group_upload_file(
    knurl_route_group *, const knurl_upload_file_config *, knurl_route **,
    knurl_error *);
#ifdef __cplusplus
}
#endif
#endif
