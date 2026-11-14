#ifndef KNURL_LONEJSON_H
#define KNURL_LONEJSON_H
#include <knurl/body.h>
#include <knurl/router.h>
#include <lonejson.h>

/** @file lonejson.h Public lonejson API. Expected failures return knurl_status
 * and optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Validate one NUL-terminated JSON value through LoneJSON. NULL is INVALID;
 * parser failures preserve the baseline INVALID/LONEJSON diagnostic. */
KNURL_API knurl_status knurl_json_validate_cstr(const char *, knurl_error *);

/** Schema metadata and optional runtime are borrowed through route destruction
 * or generator close. NULL runtime selects an independently owned runtime. */
KNURL_API knurl_status knurl_json_schema_from_map(lonejson *runtime,
                                                  const lonejson_map *map,
                                                  lonejson_schema_view *out,
                                                  knurl_error *error);
/** Completed BUFFERED/MATERIALIZED bodies only. Rewinds before and after,
 * including failure. The caller initializes and cleans up mapped storage. */
KNURL_API knurl_status knurl_request_json_into(
    knurl_request *request, const lonejson_schema_view *schema, void *record,
    knurl_error *error);
/** Validate a completed body through LoneJSON; rewind before and after,
 * including failure.
 */
KNURL_API knurl_status knurl_request_json_validate(knurl_request *request,
                                                   lonejson *runtime,
                                                   knurl_error *error);
/** Synchronous mapped input from the current source position. Borrows source
 * and consumes through EOF; never resets or closes it. AGAIN fails with STATE;
 * live blocking readers belong on native threads. Caller supplies initialized
 * mutable record storage and owns cleanup, including partial parse failure. */
KNURL_API knurl_status knurl_json_parse_source(knurl_source *,
                                               const lonejson_schema_view *,
                                               void *, knurl_error *);

/** Synchronous selected-array iteration, matching Vectis. NULL/empty selector
 * selects the root array; otherwise it selects one direct object key. Caller
 * provides initialized, empty mapped storage. Each item is reset before parsing
 * and after delivery, including failures, preserving fixed-capacity array
 * backing. Record reuse is selected per stream for both owned and supplied
 * runtimes without changing their clearing policy. Caller owns final cleanup.
 * The callback borrows that
 * same record at a zero-based index; copy values that must survive return.
 * No array/body is collected.
 * Non-OK callback results stop iteration. Sources are borrowed and read from
 * their current position; AGAIN fails with STATE (use a blocking reader on a
 * native thread for live input). Request bodies must be completed and are
 * rewound before and after, including failure. */
KNURL_API knurl_status knurl_json_array_each_source(
    knurl_source *, const char *selector, const lonejson_schema_view *,
    void *item, knurl_json_array_item_fn, void *userdata, knurl_error *);
/** Iterate a completed body using caller-owned item storage; rewind before and
 * after. See knurl_json_array_each_source.
 */
KNURL_API knurl_status knurl_request_json_array_each(
    knurl_request *, const char *selector, const lonejson_schema_view *,
    void *item, knurl_json_array_item_fn, void *userdata, knurl_error *);

/** Configuration for json source. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_json_source_config {
    /** Mapped record/view; retain backing storage according to the codec's
     * materialized/live contract.
     */
    lonejson_record_view record;
    /** Exactly-once owner-loop record-owner release after successful
     * construction; failure takes nothing. Native work acknowledges before
     * release.
     */
    knurl_source_close_fn release;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_json_source_config;
/** Initialize json source config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_json_source_config_init(knurl_json_source_config *config);
/** Actual pull generation. Success borrows unchanged record/schema storage
 * through exactly one release notification; failure takes nothing. */
KNURL_API knurl_status knurl_source_json(const knurl_json_source_config *config,
                                         knurl_source **out,
                                         knurl_error *error);
/** Select an explicit live JSON generator; retain record/schema until release.
 * See knurl_source_json.
 */
KNURL_API knurl_status knurl_reply_json(knurl_reply *reply, int status,
                                        const knurl_json_source_config *config,
                                        knurl_error *error);
/** Explicitly materializes output; zero adds no output ceiling.
 * These calls finish synchronously, so a C stack record is safe. */
KNURL_API knurl_status knurl_json_serialize_materialized(
    const lonejson_schema_view *schema, const void *record, size_t max_bytes,
    knurl_owned_bytes *out, knurl_error *error);
/** Serialize synchronously into a copied staged reply; zero max_bytes adds no
 * ceiling. Stack records may expire on return.
 */
KNURL_API knurl_status knurl_reply_json_materialized(
    knurl_reply *reply, int status, const lonejson_schema_view *schema,
    const void *record, size_t max_bytes, knurl_error *error);
/** Synchronous bounded generation into a sink. The sink accepts the complete
 * offered chunk with OK or fails terminally. No complete JSON buffer/file is
 * created. Borrows record/schema unchanged through return. */
KNURL_API knurl_status knurl_json_write_sink(const lonejson_schema_view *,
                                             const void *, knurl_json_sink_fn,
                                             void *, knurl_error *);
/** Vectis selected-array rewriting through LoneJSON's public mapped callback
 * options. Borrows source, optional runtime, options/item storage and sink.
 * Reads from the current position without reset/close. Output flows before
 * input EOF; no complete document/array is collected. */
KNURL_API knurl_status
knurl_json_array_rewrite_source(knurl_source *, const char *, lonejson *,
                                const lonejson_array_rewrite_options *,
                                knurl_json_sink_fn, void *, knurl_error *);
/** Synchronously serializes into a private file, without a full JSON buffer or
 * retaining the record/schema after return. Directory/ownership follow
 * knurl_reply_source_spooled. Failure preserves the existing reply. */
KNURL_API knurl_status knurl_reply_json_spooled(knurl_reply *, int status,
                                                const lonejson_schema_view *,
                                                const void *record,
                                                const char *directory,
                                                knurl_error *);

/** Owner-loop mapped input/output handler. Borrow schema-backed records;
 * ordinary output serializes before after-hooks. Missing schemas supply NULL
 * views.
 */
typedef knurl_status (*knurl_json_route_fn)(knurl_request *request,
                                            const lonejson_record_view *input,
                                            const lonejson_record_view *output,
                                            void *userdata, knurl_error *error);
/** Owner-loop mapped input handler selecting a staged reply. Explicit live
 * outputs retain their record owners through producer cleanup.
 */
typedef knurl_status (*knurl_json_typed_route_fn)(
    knurl_request *request, const lonejson_record_view *input,
    knurl_reply *reply, void *userdata, knurl_error *error);

/** Caller/adapter-owned initialized mapped storage and opaque owner token; Core
 * never frees supplied record storage.
 */
typedef struct knurl_record_storage {
    /** Public mapped record view; backing storage remains owner-controlled. */
    lonejson_record_view view;
    /** Opaque adapter owner token; Core never interprets its representation. */
    void *owner;
} knurl_record_storage;
/** Optional typed-record owner adapter; both callbacks are required. Acquire
 * supplies initialized mutable storage matching the declared schema:
 * output=0 selects input, output=1 output. The returned owner is opaque to
 * Core. Release runs once per acquire attempt, including partial failure,
 * and drops the adapter reference; the owner controls record cleanup and
 * Core never frees supplied storage. Parsing may reset mapped fields.
 * Declarations are copied and callbacks use the route handler userdata on
 * the owner loop. XML storage stays pinned through native parser
 * acknowledgement, so its allocator/field callbacks must support native
 * parsing.
 */
typedef struct knurl_record_provider {
    /** Owner-loop record acquisition callback; supplies initialized mutable
     * storage for the declared schema.
     */
    knurl_status (*acquire)(knurl_request *, int output,
                            const lonejson_schema_view *,
                            knurl_record_storage *, void *, knurl_error *);
    /** Owner-release callback; called under the enclosing acquire/release
     * contract.
     */
    void (*release)(knurl_record_storage *, void *);
} knurl_record_provider;

/** Configuration for json route. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_json_route_config {
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
    /** Borrowed input schema; an absent schema supplies no mapped input view.
     */
    lonejson_schema_view input;
    /** Borrowed output schema used by ordinary typed JSON routes. */
    lonejson_schema_view output;
    /** Application handler invoked under this route's documented execution and
     * lifetime rules.
     */
    knurl_json_route_fn handler;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    knurl_route_hooks hooks;
    /** Optional retained route ACL. */
    knurl_acl *acl;
    /** Optional copied record-owner adapter; its callbacks/storage obey
     * knurl_record_provider.
     */
    const knurl_record_provider *records;
    /** Ordinary mapped output ceiling; zero adds no limit. */
    size_t max_output_bytes;
} knurl_json_route_config;
/** Configuration for json typed route. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_json_typed_route_config {
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
    /** Borrowed input schema; an absent schema supplies no mapped input view.
     */
    lonejson_schema_view input;
    /** Application handler invoked under this route's documented execution and
     * lifetime rules.
     */
    knurl_json_typed_route_fn handler;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    knurl_route_hooks hooks;
    /** Optional retained route ACL. */
    knurl_acl *acl;
    /** Optional copied record-owner adapter; its callbacks/storage obey
     * knurl_record_provider.
     */
    const knurl_record_provider *records;
} knurl_json_typed_route_config;

/** Initialize json route config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_json_route_config_init(knurl_json_route_config *config);
/** Initialize json typed route config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void
knurl_json_typed_route_config_init(knurl_json_typed_route_config *config);
/** Copies declarations, borrows schemas/runtime/userdata. Ordinary route output
 * is materialized before after-hooks, without a new output-size ceiling, and
 * its records are then cleaned. Typed-reply records instead remain valid
 * through producer cleanup when an explicit live reply borrows them. Ordinary
 * staged middleware order applies. No schema means a NULL callback view.
 * Optional max_output_bytes bounds ordinary output; zero adds no limit. */
KNURL_API knurl_status knurl_router_json_route(
    knurl_router *router, const knurl_json_route_config *config,
    knurl_route **out, knurl_error *error);
/** Register a mapped JSON input/staged-reply route. Schemas/runtime and handler
 * userdata remain borrowed; explicit live outputs retain their record owners.
 */
KNURL_API knurl_status knurl_router_json_typed_route(
    knurl_router *router, const knurl_json_typed_route_config *config,
    knurl_route **out, knurl_error *error);
/** Register a mapped JSON input/output route under the group prefix.
 * Schemas/runtime and handler userdata remain borrowed; ordinary output is
 * serialized before after-hooks.
 */
KNURL_API knurl_status knurl_route_group_json_route(
    knurl_route_group *group, const knurl_json_route_config *config,
    knurl_route **out, knurl_error *error);
/** Register a mapped JSON input/staged-reply route under the group prefix.
 * Schemas/runtime and handler userdata remain borrowed; explicit live outputs
 * retain their record owners.
 */
KNURL_API knurl_status knurl_route_group_json_typed_route(
    knurl_route_group *group, const knurl_json_typed_route_config *config,
    knurl_route **out, knurl_error *error);
/** Buffered REST conveniences. Decoded values are borrowed through response
 * completion. Default decoding owns a LoneJSON JSON value (cast decoded to
 * const lonejson_json_value *); empty input and decode_disabled produce NULL.
 * A custom decoder runs even on empty input (unless decode_disabled) and owns
 * its successful result until release; failure cleans its own partial result.
 * Validation finish skips the handler; after-hooks may still compose a reply.
 * An on_error failure propagates as an ordinary route failure, skipping after.
 * All callbacks execute on the owner loop. */
typedef enum knurl_rest_phase {
    /** Buffered REST error phase: decode. */
    KNURL_REST_DECODE = 0,
    /** Buffered REST error phase: validate. */
    KNURL_REST_VALIDATE,
    /** Buffered REST error phase: handler. */
    KNURL_REST_HANDLER
} knurl_rest_phase;
/** Owner-loop buffered decoder, including empty input when selected. Completed
 * spills are copied into memory within the admitted body limit. Input bytes
 * remain borrowed until request cleanup, including decoded-value release.
 * Successful decoded ownership lasts through release; a failed decoder cleans
 * its own partial result.
 */
typedef knurl_status (*knurl_rest_decode_fn)(knurl_request *, knurl_bytes,
                                             void **decoded, void *userdata,
                                             knurl_error *);
/** Release one successful custom decoded REST value after response completion;
 * retain userdata through this callback.
 */
typedef void (*knurl_rest_release_fn)(void *decoded, void *userdata);
/** Validate the borrowed decoded REST value; select a staged reply and set
 * finish to skip the handler. After-hooks may still run.
 */
typedef knurl_status (*knurl_rest_validate_fn)(knurl_request *, const void *,
                                               knurl_reply *, int *finish,
                                               void *userdata, knurl_error *);
/** Owner-loop REST handler with borrowed decoded value; select a staged reply
 * through public builders.
 */
typedef knurl_status (*knurl_rest_handler_fn)(knurl_request *, const void *,
                                              knurl_reply *, void *userdata,
                                              knurl_error *);
/** Compose a staged response for decode/validate/handler failure. Returning an
 * error propagates normal route failure and skips after-hooks.
 */
typedef knurl_status (*knurl_rest_error_fn)(knurl_request *, knurl_rest_phase,
                                            const knurl_error *cause,
                                            knurl_reply *, void *userdata,
                                            knurl_error *);
/** Configuration for rest route. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_rest_route_config {
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
    /** Optional REST decoder; successful output remains alive until release. */
    knurl_rest_decode_fn decode;
    /** Owner-release callback; called under the enclosing acquire/release
     * contract.
     */
    knurl_rest_release_fn release;
    /** Optional REST validator; may select a reply and finish before the
     * handler.
     */
    knurl_rest_validate_fn validate;
    /** Application handler invoked under this route's documented execution and
     * lifetime rules.
     */
    knurl_rest_handler_fn handler;
    /** Error-policy callback under this operation's commitment/timing rules. */
    knurl_rest_error_fn on_error;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Nonzero skips buffered REST JSON decoding. */
    int decode_disabled;
} knurl_rest_route_config;
/** Configuration for rest reply. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_rest_reply_config {
    /** HTTP status code; zero means unknown/unset where documented by this
     * event/builder.
     */
    int status;
    /** Representation MIME type; NULL selects the operation's default when
     * allowed.
     */
    const char *content_type;
    /** Maximum bytes for this operation; zero-inheritance/unlimited behavior is
     * operation-specific.
     */
    size_t max_bytes;
    /** Nonzero suppresses the REST JSON trailing newline. */
    int newline_disabled;
} knurl_rest_reply_config;
/** Initialize rest route config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_rest_route_config_init(knurl_rest_route_config *);
/** Initialize rest reply config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_rest_reply_config_init(knurl_rest_reply_config *);
/** Register a buffered REST route. Decode/validate/handler/error phases reuse
 * Core; successful decoded owners release once.
 */
KNURL_API knurl_status knurl_router_rest_route(knurl_router *,
                                               const knurl_rest_route_config *,
                                               knurl_route **, knurl_error *);
/** Register a buffered REST route under the group prefix.
 * Decode/validate/handler/error phases reuse Core; successful decoded owners
 * release once.
 */
KNURL_API knurl_status knurl_route_group_rest_route(
    knurl_route_group *, const knurl_rest_route_config *, knurl_route **,
    knurl_error *);
/** Synchronous bounded JSON composition; copies before returning. Defaults:
 * 200, application/json; charset=utf-8, no output ceiling and a trailing
 * newline. Failure preserves the existing staged reply. Header composition uses
 * reply helpers.
 */
KNURL_API knurl_status knurl_reply_rest_json(knurl_reply *,
                                             const lonejson_json_value *,
                                             const knurl_rest_reply_config *,
                                             knurl_error *);
/** Generic default envelope; callers can instead own their error policy through
 * on_error. Zero status derives 400/409/501/504/500 from the cause. */
KNURL_API knurl_status knurl_reply_error_json(knurl_reply *, int status,
                                              const char *kind,
                                              const knurl_error *cause,
                                              knurl_error *);
#ifdef __cplusplus
}
#endif
#endif
