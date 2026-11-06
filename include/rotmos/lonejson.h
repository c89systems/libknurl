#ifndef ROTMOS_LONEJSON_H
#define ROTMOS_LONEJSON_H
#include <rotmos/body.h>
#include <rotmos/router.h>
#include <lonejson.h>

/** @file lonejson.h Public lonejson API. Expected failures return rotmos_status
 * and optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Validate one NUL-terminated JSON value through LoneJSON. NULL is INVALID;
 * parser failures preserve the baseline INVALID/LONEJSON diagnostic. */
ROTMOS_API rotmos_status rotmos_json_validate_cstr(const char *,
                                                   rotmos_error *);

/** Schema metadata and optional runtime are borrowed through route destruction
 * or generator close. NULL runtime selects an independently owned runtime. */
ROTMOS_API rotmos_status rotmos_json_schema_from_map(lonejson *runtime,
                                                     const lonejson_map *map,
                                                     lonejson_schema_view *out,
                                                     rotmos_error *error);
/** Completed BUFFERED/MATERIALIZED bodies only. Rewinds before and after,
 * including failure. The caller initializes and cleans up mapped storage. */
ROTMOS_API rotmos_status rotmos_request_json_into(
    rotmos_request *request, const lonejson_schema_view *schema, void *record,
    rotmos_error *error);
/** Validate a completed body through LoneJSON; rewind before and after,
 * including failure.
 */
ROTMOS_API rotmos_status rotmos_request_json_validate(rotmos_request *request,
                                                      lonejson *runtime,
                                                      rotmos_error *error);
/** Synchronous mapped input from the current source position. Borrows source
 * and consumes through EOF; never resets or closes it. AGAIN fails with STATE;
 * live blocking readers belong on native threads. Caller supplies initialized
 * mutable record storage and owns cleanup, including partial parse failure. */
ROTMOS_API rotmos_status rotmos_json_parse_source(rotmos_source *,
                                                  const lonejson_schema_view *,
                                                  void *, rotmos_error *);

/** Borrow the caller's reused mapped record at a zero-based array index; copy
 * anything needed after return. Non-OK stops iteration.
 */
typedef rotmos_status (*rotmos_json_array_item_fn)(
    size_t index, const lonejson_record_view *item, void *userdata,
    rotmos_error *error);
/** Synchronous selected-array iteration, matching Vectis. NULL/empty selector
 * selects the root array; otherwise it selects one direct object key. Caller
 * provides empty mapped storage, initialized and cleaned for every item,
 * including failures. The callback borrows that same record at a zero-based
 * index; copy values that must survive return. No array/body is collected.
 * Non-OK callback results stop iteration. Sources are borrowed and read from
 * their current position; AGAIN fails with STATE (use a blocking reader on a
 * native thread for live input). Request bodies must be completed and are
 * rewound before and after, including failure. */
ROTMOS_API rotmos_status rotmos_json_array_each_source(
    rotmos_source *, const char *selector, const lonejson_schema_view *,
    void *item, rotmos_json_array_item_fn, void *userdata, rotmos_error *);
/** Iterate a completed body using caller-owned item storage; rewind before and
 * after. See rotmos_json_array_each_source.
 */
ROTMOS_API rotmos_status rotmos_request_json_array_each(
    rotmos_request *, const char *selector, const lonejson_schema_view *,
    void *item, rotmos_json_array_item_fn, void *userdata, rotmos_error *);

/** Configuration for json source. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_json_source_config {
    /** Mapped record/view; retain backing storage according to the codec's
     * materialized/live contract.
     */
    lonejson_record_view record;
    /** Exactly-once owner-loop record-owner release after successful
     * construction; failure takes nothing. Native work acknowledges before
     * release.
     */
    rotmos_source_close_fn release;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_json_source_config;
/** Initialize json source config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void
rotmos_json_source_config_init(rotmos_json_source_config *config);
/** Actual pull generation. Success borrows unchanged record/schema storage
 * through exactly one release notification; failure takes nothing. */
ROTMOS_API rotmos_status
rotmos_source_json(const rotmos_json_source_config *config, rotmos_source **out,
                   rotmos_error *error);
/** Select an explicit live JSON generator; retain record/schema until release.
 * See rotmos_source_json.
 */
ROTMOS_API rotmos_status
rotmos_reply_json(rotmos_reply *reply, int status,
                  const rotmos_json_source_config *config, rotmos_error *error);
/** Explicitly materializes output; zero adds no output ceiling.
 * These calls finish synchronously, so a C stack record is safe. */
ROTMOS_API rotmos_status rotmos_json_serialize_materialized(
    const lonejson_schema_view *schema, const void *record, size_t max_bytes,
    rotmos_owned_bytes *out, rotmos_error *error);
/** Serialize synchronously into a copied staged reply; zero max_bytes adds no
 * ceiling. Stack records may expire on return.
 */
ROTMOS_API rotmos_status rotmos_reply_json_materialized(
    rotmos_reply *reply, int status, const lonejson_schema_view *schema,
    const void *record, size_t max_bytes, rotmos_error *error);
/** Synchronously accept the complete offered JSON chunk with OK or fail
 * terminally. Chunk bytes are borrowed only during the call.
 */
typedef rotmos_status (*rotmos_json_sink_fn)(const void *, size_t, void *,
                                             rotmos_error *);
/** Synchronous bounded generation into a sink. The sink accepts the complete
 * offered chunk with OK or fails terminally. No complete JSON buffer/file is
 * created. Borrows record/schema unchanged through return. */
ROTMOS_API rotmos_status rotmos_json_write_sink(const lonejson_schema_view *,
                                                const void *,
                                                rotmos_json_sink_fn, void *,
                                                rotmos_error *);
/** Vectis selected-array rewriting through LoneJSON's public mapped callback
 * options. Borrows source, optional runtime, options/item storage and sink.
 * Reads from the current position without reset/close. Output flows before
 * input EOF; no complete document/array is collected. */
ROTMOS_API rotmos_status
rotmos_json_array_rewrite_source(rotmos_source *, const char *, lonejson *,
                                 const lonejson_array_rewrite_options *,
                                 rotmos_json_sink_fn, void *, rotmos_error *);
/** Synchronously serializes into a private file, without a full JSON buffer or
 * retaining the record/schema after return. Directory/ownership follow
 * rotmos_reply_source_spooled. Failure preserves the existing reply. */
ROTMOS_API rotmos_status rotmos_reply_json_spooled(rotmos_reply *, int status,
                                                   const lonejson_schema_view *,
                                                   const void *record,
                                                   const char *directory,
                                                   rotmos_error *);

/** Owner-loop mapped input/output handler. Borrow schema-backed records;
 * ordinary output serializes before after-hooks. Missing schemas supply NULL
 * views.
 */
typedef rotmos_status (*rotmos_json_route_fn)(
    rotmos_request *request, const lonejson_record_view *input,
    const lonejson_record_view *output, void *userdata, rotmos_error *error);
/** Owner-loop mapped input handler selecting a staged reply. Explicit live
 * outputs retain their record owners through producer cleanup.
 */
typedef rotmos_status (*rotmos_json_typed_route_fn)(
    rotmos_request *request, const lonejson_record_view *input,
    rotmos_reply *reply, void *userdata, rotmos_error *error);

/** Caller/adapter-owned initialized mapped storage and opaque owner token; Core
 * never frees supplied record storage.
 */
typedef struct rotmos_record_storage {
    /** Public mapped record view; backing storage remains owner-controlled. */
    lonejson_record_view view;
    /** Opaque adapter owner token; Core never interprets its representation. */
    void *owner;
} rotmos_record_storage;
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
typedef struct rotmos_record_provider {
    /** Owner-loop record acquisition callback; supplies initialized mutable
     * storage for the declared schema.
     */
    rotmos_status (*acquire)(rotmos_request *, int output,
                             const lonejson_schema_view *,
                             rotmos_record_storage *, void *, rotmos_error *);
    /** Owner-release callback; called under the enclosing acquire/release
     * contract.
     */
    void (*release)(rotmos_record_storage *, void *);
} rotmos_record_provider;

/** Configuration for json route. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_json_route_config {
    /** Single HTTP method; methods supplies the explicit multi-method mask when
     * nonzero.
     */
    rotmos_http_method method;
    /** HTTP method mask; zero uses this operation's single-method/default
     * policy.
     */
    rotmos_http_methods methods;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    rotmos_route_path_kind path_kind;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    rotmos_body_policy body;
    /** Borrowed input schema; an absent schema supplies no mapped input view.
     */
    lonejson_schema_view input;
    /** Borrowed output schema used by ordinary typed JSON routes. */
    lonejson_schema_view output;
    /** Application handler invoked under this route's documented execution and
     * lifetime rules.
     */
    rotmos_json_route_fn handler;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    rotmos_route_hooks hooks;
    /** Optional copied record-owner adapter; its callbacks/storage obey
     * rotmos_record_provider.
     */
    const rotmos_record_provider *records;
    /** Ordinary mapped output ceiling; zero adds no limit. */
    size_t max_output_bytes;
} rotmos_json_route_config;
/** Configuration for json typed route. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct rotmos_json_typed_route_config {
    /** Single HTTP method; methods supplies the explicit multi-method mask when
     * nonzero.
     */
    rotmos_http_method method;
    /** HTTP method mask; zero uses this operation's single-method/default
     * policy.
     */
    rotmos_http_methods methods;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    rotmos_route_path_kind path_kind;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    rotmos_body_policy body;
    /** Borrowed input schema; an absent schema supplies no mapped input view.
     */
    lonejson_schema_view input;
    /** Application handler invoked under this route's documented execution and
     * lifetime rules.
     */
    rotmos_json_typed_route_fn handler;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    rotmos_route_hooks hooks;
    /** Optional copied record-owner adapter; its callbacks/storage obey
     * rotmos_record_provider.
     */
    const rotmos_record_provider *records;
} rotmos_json_typed_route_config;

/** Initialize json route config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_json_route_config_init(rotmos_json_route_config *config);
/** Initialize json typed route config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void
rotmos_json_typed_route_config_init(rotmos_json_typed_route_config *config);
/** Copies declarations, borrows schemas/runtime/userdata. Ordinary route output
 * is materialized before after-hooks, without a new output-size ceiling, and
 * its records are then cleaned. Typed-reply records instead remain valid
 * through producer cleanup when an explicit live reply borrows them. Ordinary
 * staged middleware order applies. No schema means a NULL callback view.
 * Optional max_output_bytes bounds ordinary output; zero adds no limit. */
ROTMOS_API rotmos_status rotmos_router_json_route(
    rotmos_router *router, const rotmos_json_route_config *config,
    rotmos_route **out, rotmos_error *error);
/** Register a mapped JSON input/staged-reply route. Schemas/runtime and handler
 * userdata remain borrowed; explicit live outputs retain their record owners.
 */
ROTMOS_API rotmos_status rotmos_router_json_typed_route(
    rotmos_router *router, const rotmos_json_typed_route_config *config,
    rotmos_route **out, rotmos_error *error);
/** Register a mapped JSON input/output route under the group prefix.
 * Schemas/runtime and handler userdata remain borrowed; ordinary output is
 * serialized before after-hooks.
 */
ROTMOS_API rotmos_status rotmos_route_group_json_route(
    rotmos_route_group *group, const rotmos_json_route_config *config,
    rotmos_route **out, rotmos_error *error);
/** Register a mapped JSON input/staged-reply route under the group prefix.
 * Schemas/runtime and handler userdata remain borrowed; explicit live outputs
 * retain their record owners.
 */
ROTMOS_API rotmos_status rotmos_route_group_json_typed_route(
    rotmos_route_group *group, const rotmos_json_typed_route_config *config,
    rotmos_route **out, rotmos_error *error);
/** Buffered REST conveniences. Decoded values are borrowed through response
 * completion. Default decoding owns a LoneJSON JSON value (cast decoded to
 * const lonejson_json_value *); empty input and decode_disabled produce NULL.
 * A custom decoder runs even on empty input (unless decode_disabled) and owns
 * its successful result until release; failure cleans its own partial result.
 * Validation finish skips the handler; after-hooks may still compose a reply.
 * An on_error failure propagates as an ordinary route failure, skipping after.
 * All callbacks execute on the owner loop. */
typedef enum rotmos_rest_phase {
    /** Buffered REST error phase: decode. */
    ROTMOS_REST_DECODE = 0,
    /** Buffered REST error phase: validate. */
    ROTMOS_REST_VALIDATE,
    /** Buffered REST error phase: handler. */
    ROTMOS_REST_HANDLER
} rotmos_rest_phase;
/** Owner-loop buffered decoder, including empty input when selected. Successful
 * decoded ownership lasts through release; a failed decoder cleans its own
 * partial result.
 */
typedef rotmos_status (*rotmos_rest_decode_fn)(rotmos_request *, rotmos_bytes,
                                               void **decoded, void *userdata,
                                               rotmos_error *);
/** Release one successful custom decoded REST value after response completion;
 * retain userdata through this callback.
 */
typedef void (*rotmos_rest_release_fn)(void *decoded, void *userdata);
/** Validate the borrowed decoded REST value; select a staged reply and set
 * finish to skip the handler. After-hooks may still run.
 */
typedef rotmos_status (*rotmos_rest_validate_fn)(rotmos_request *, const void *,
                                                 rotmos_reply *, int *finish,
                                                 void *userdata,
                                                 rotmos_error *);
/** Owner-loop REST handler with borrowed decoded value; select a staged reply
 * through public builders.
 */
typedef rotmos_status (*rotmos_rest_handler_fn)(rotmos_request *, const void *,
                                                rotmos_reply *, void *userdata,
                                                rotmos_error *);
/** Compose a staged response for decode/validate/handler failure. Returning an
 * error propagates normal route failure and skips after-hooks.
 */
typedef rotmos_status (*rotmos_rest_error_fn)(rotmos_request *,
                                              rotmos_rest_phase,
                                              const rotmos_error *cause,
                                              rotmos_reply *, void *userdata,
                                              rotmos_error *);
/** Configuration for rest route. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_rest_route_config {
    /** Single HTTP method; methods supplies the explicit multi-method mask when
     * nonzero.
     */
    rotmos_http_method method;
    /** HTTP method mask; zero uses this operation's single-method/default
     * policy.
     */
    rotmos_http_methods methods;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    rotmos_route_path_kind path_kind;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    rotmos_body_policy body;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    rotmos_route_hooks hooks;
    /** Optional REST decoder; successful output remains alive until release. */
    rotmos_rest_decode_fn decode;
    /** Owner-release callback; called under the enclosing acquire/release
     * contract.
     */
    rotmos_rest_release_fn release;
    /** Optional REST validator; may select a reply and finish before the
     * handler.
     */
    rotmos_rest_validate_fn validate;
    /** Application handler invoked under this route's documented execution and
     * lifetime rules.
     */
    rotmos_rest_handler_fn handler;
    /** Error-policy callback under this operation's commitment/timing rules. */
    rotmos_rest_error_fn on_error;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Nonzero skips buffered REST JSON decoding. */
    int decode_disabled;
} rotmos_rest_route_config;
/** Configuration for rest reply. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_rest_reply_config {
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
} rotmos_rest_reply_config;
/** Initialize rest route config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_rest_route_config_init(rotmos_rest_route_config *);
/** Initialize rest reply config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_rest_reply_config_init(rotmos_rest_reply_config *);
/** Register a buffered REST route. Decode/validate/handler/error phases reuse
 * Core; successful decoded owners release once.
 */
ROTMOS_API rotmos_status
rotmos_router_rest_route(rotmos_router *, const rotmos_rest_route_config *,
                         rotmos_route **, rotmos_error *);
/** Register a buffered REST route under the group prefix.
 * Decode/validate/handler/error phases reuse Core; successful decoded owners
 * release once.
 */
ROTMOS_API rotmos_status rotmos_route_group_rest_route(
    rotmos_route_group *, const rotmos_rest_route_config *, rotmos_route **,
    rotmos_error *);
/** Synchronous bounded JSON composition; copies before returning. Defaults:
 * 200, application/json; charset=utf-8, no output ceiling and a trailing
 * newline. Failure preserves the existing staged reply. Header composition uses
 * reply helpers.
 */
ROTMOS_API rotmos_status
rotmos_reply_rest_json(rotmos_reply *, const lonejson_json_value *,
                       const rotmos_rest_reply_config *, rotmos_error *);
/** Generic default envelope; callers can instead own their error policy through
 * on_error. Zero status derives 400/409/501/504/500 from the cause. */
ROTMOS_API rotmos_status rotmos_reply_error_json(rotmos_reply *, int status,
                                                 const char *kind,
                                                 const rotmos_error *cause,
                                                 rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif
