#ifndef ROTMOS_LONEJSON_H
#define ROTMOS_LONEJSON_H
#include <rotmos/body.h>
#include <rotmos/router.h>
#include <lonejson.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Validate one NUL-terminated JSON value through LoneJSON. NULL is INVALID;
 * parser failures preserve the baseline INVALID/LONEJSON diagnostic. */
ROTMOS_API rotmos_status rotmos_json_validate_cstr(const char *,
                                                   rotmos_error *);

/* Schema metadata and optional runtime are borrowed through route destruction
 * or generator close. NULL runtime selects an independently owned runtime. */
ROTMOS_API rotmos_status rotmos_json_schema_from_map(lonejson *runtime,
                                                     const lonejson_map *map,
                                                     lonejson_schema_view *out,
                                                     rotmos_error *error);
/* Completed BUFFERED/MATERIALIZED bodies only. Rewinds before and after,
 * including failure. The caller initializes and cleans up mapped storage. */
ROTMOS_API rotmos_status rotmos_request_json_into(
    rotmos_request *request, const lonejson_schema_view *schema, void *record,
    rotmos_error *error);
ROTMOS_API rotmos_status rotmos_request_json_validate(rotmos_request *request,
                                                      lonejson *runtime,
                                                      rotmos_error *error);
/* Synchronous mapped input from the current source position. Borrows source
 * and consumes through EOF; never resets or closes it. AGAIN fails with STATE;
 * live blocking readers belong on native threads. Caller supplies initialized
 * mutable record storage and owns cleanup, including partial parse failure. */
ROTMOS_API rotmos_status rotmos_json_parse_source(rotmos_source *,
                                                  const lonejson_schema_view *,
                                                  void *, rotmos_error *);

typedef rotmos_status (*rotmos_json_array_item_fn)(
    size_t index, const lonejson_record_view *item, void *userdata,
    rotmos_error *error);
/* Synchronous selected-array iteration, matching Vectis. NULL/empty selector
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
ROTMOS_API rotmos_status rotmos_request_json_array_each(
    rotmos_request *, const char *selector, const lonejson_schema_view *,
    void *item, rotmos_json_array_item_fn, void *userdata, rotmos_error *);

typedef struct rotmos_json_source_config {
    lonejson_record_view record;
    rotmos_source_close_fn release;
    void *userdata;
} rotmos_json_source_config;
ROTMOS_API void
rotmos_json_source_config_init(rotmos_json_source_config *config);
/* Actual pull generation. Success borrows unchanged record/schema storage
 * through exactly one release notification; failure takes nothing. */
ROTMOS_API rotmos_status
rotmos_source_json(const rotmos_json_source_config *config, rotmos_source **out,
                   rotmos_error *error);
ROTMOS_API rotmos_status
rotmos_reply_json(rotmos_reply *reply, int status,
                  const rotmos_json_source_config *config, rotmos_error *error);
/* Explicitly materializes output; zero adds no output ceiling.
 * These calls finish synchronously, so a C stack record is safe. */
ROTMOS_API rotmos_status rotmos_json_serialize_materialized(
    const lonejson_schema_view *schema, const void *record, size_t max_bytes,
    rotmos_owned_bytes *out, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_reply_json_materialized(
    rotmos_reply *reply, int status, const lonejson_schema_view *schema,
    const void *record, size_t max_bytes, rotmos_error *error);
typedef rotmos_status (*rotmos_json_sink_fn)(const void *, size_t, void *,
                                             rotmos_error *);
/* Synchronous bounded generation into a sink. The sink accepts the complete
 * offered chunk with OK or fails terminally. No complete JSON buffer/file is
 * created. Borrows record/schema unchanged through return. */
ROTMOS_API rotmos_status rotmos_json_write_sink(const lonejson_schema_view *,
                                                const void *,
                                                rotmos_json_sink_fn, void *,
                                                rotmos_error *);
/* Vectis selected-array rewriting through LoneJSON's public mapped callback
 * options. Borrows source, optional runtime, options/item storage and sink.
 * Reads from the current position without reset/close. Output flows before
 * input EOF; no complete document/array is collected. */
ROTMOS_API rotmos_status
rotmos_json_array_rewrite_source(rotmos_source *, const char *, lonejson *,
                                 const lonejson_array_rewrite_options *,
                                 rotmos_json_sink_fn, void *, rotmos_error *);
/* Synchronously serializes into a private file, without a full JSON buffer or
 * retaining the record/schema after return. Directory/ownership follow
 * rotmos_reply_source_spooled. Failure preserves the existing reply. */
ROTMOS_API rotmos_status rotmos_reply_json_spooled(rotmos_reply *, int status,
                                                   const lonejson_schema_view *,
                                                   const void *record,
                                                   const char *directory,
                                                   rotmos_error *);

typedef rotmos_status (*rotmos_json_route_fn)(
    rotmos_request *request, const lonejson_record_view *input,
    const lonejson_record_view *output, void *userdata, rotmos_error *error);
typedef rotmos_status (*rotmos_json_typed_route_fn)(
    rotmos_request *request, const lonejson_record_view *input,
    rotmos_reply *reply, void *userdata, rotmos_error *error);

/* Optional owner adapter for typed-route storage (for example Lua records).
 * Both callbacks are required. Acquire supplies initialized mutable storage
 * matching the declared schema; output is 0 for input and 1 for output. Owner
 * is an opaque adapter token. Release runs once for every acquire attempt,
 * including partial failure, and releases the adapter's reference. The owner
 * controls final record cleanup; rotmos never frees supplied storage. Parsing
 * can reset fields under the ordinary mapped-parser contract.
 * Callbacks run on the owner loop. XML input stays pinned through native parser
 * acknowledgement; its allocator/field callbacks must support native parsing.
 * The declaration is copied; callbacks use the route's handler userdata. */
typedef struct rotmos_record_storage {
    lonejson_record_view view;
    void *owner;
} rotmos_record_storage;
typedef struct rotmos_record_provider {
    rotmos_status (*acquire)(rotmos_request *, int output,
                             const lonejson_schema_view *,
                             rotmos_record_storage *, void *, rotmos_error *);
    void (*release)(rotmos_record_storage *, void *);
} rotmos_record_provider;

typedef struct rotmos_json_route_config {
    rotmos_http_method method;
    rotmos_http_methods methods;
    const char *path;
    rotmos_route_path_kind path_kind;
    rotmos_body_policy body;
    lonejson_schema_view input;
    lonejson_schema_view output;
    rotmos_json_route_fn handler;
    void *userdata;
    rotmos_route_hooks hooks;
    const rotmos_record_provider *records;
    size_t max_output_bytes;
} rotmos_json_route_config;
typedef struct rotmos_json_typed_route_config {
    rotmos_http_method method;
    rotmos_http_methods methods;
    const char *path;
    rotmos_route_path_kind path_kind;
    rotmos_body_policy body;
    lonejson_schema_view input;
    rotmos_json_typed_route_fn handler;
    void *userdata;
    rotmos_route_hooks hooks;
    const rotmos_record_provider *records;
} rotmos_json_typed_route_config;

ROTMOS_API void rotmos_json_route_config_init(rotmos_json_route_config *config);
ROTMOS_API void
rotmos_json_typed_route_config_init(rotmos_json_typed_route_config *config);
/* Copies declarations, borrows schemas/runtime/userdata. Ordinary route output
 * is materialized before after-hooks, without a new output-size ceiling, and
 * its records are then cleaned. Typed-reply records instead remain valid
 * through producer cleanup when an explicit live reply borrows them. Ordinary
 * staged middleware order applies. No schema means a NULL callback view.
 * Optional max_output_bytes bounds ordinary output; zero adds no limit. */
ROTMOS_API rotmos_status rotmos_router_json_route(
    rotmos_router *router, const rotmos_json_route_config *config,
    rotmos_route **out, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_router_json_typed_route(
    rotmos_router *router, const rotmos_json_typed_route_config *config,
    rotmos_route **out, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_route_group_json_route(
    rotmos_route_group *group, const rotmos_json_route_config *config,
    rotmos_route **out, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_route_group_json_typed_route(
    rotmos_route_group *group, const rotmos_json_typed_route_config *config,
    rotmos_route **out, rotmos_error *error);
/* Buffered REST conveniences. Decoded values are borrowed through response
 * completion. Default decoding owns a LoneJSON JSON value (cast decoded to
 * const lonejson_json_value *); empty input and decode_disabled produce NULL.
 * A custom decoder runs even on empty input (unless decode_disabled) and owns
 * its successful result until release; failure cleans its own partial result.
 * Validation finish skips the handler; after-hooks may still compose a reply.
 * An on_error failure propagates as an ordinary route failure, skipping after.
 * All callbacks execute on the owner loop. */
typedef enum rotmos_rest_phase {
    ROTMOS_REST_DECODE = 0,
    ROTMOS_REST_VALIDATE,
    ROTMOS_REST_HANDLER
} rotmos_rest_phase;
typedef rotmos_status (*rotmos_rest_decode_fn)(rotmos_request *, rotmos_bytes,
                                               void **decoded, void *userdata,
                                               rotmos_error *);
typedef void (*rotmos_rest_release_fn)(void *decoded, void *userdata);
typedef rotmos_status (*rotmos_rest_validate_fn)(rotmos_request *, const void *,
                                                 rotmos_reply *, int *finish,
                                                 void *userdata,
                                                 rotmos_error *);
typedef rotmos_status (*rotmos_rest_handler_fn)(rotmos_request *, const void *,
                                                rotmos_reply *, void *userdata,
                                                rotmos_error *);
typedef rotmos_status (*rotmos_rest_error_fn)(rotmos_request *,
                                              rotmos_rest_phase,
                                              const rotmos_error *cause,
                                              rotmos_reply *, void *userdata,
                                              rotmos_error *);
typedef struct rotmos_rest_route_config {
    rotmos_http_method method;
    rotmos_http_methods methods;
    const char *path;
    rotmos_route_path_kind path_kind;
    rotmos_body_policy body;
    rotmos_route_hooks hooks;
    rotmos_rest_decode_fn decode;
    rotmos_rest_release_fn release;
    rotmos_rest_validate_fn validate;
    rotmos_rest_handler_fn handler;
    rotmos_rest_error_fn on_error;
    void *userdata;
    int decode_disabled;
} rotmos_rest_route_config;
typedef struct rotmos_rest_reply_config {
    int status;
    const char *content_type;
    size_t max_bytes;
    int newline_disabled;
} rotmos_rest_reply_config;
ROTMOS_API void rotmos_rest_route_config_init(rotmos_rest_route_config *);
ROTMOS_API void rotmos_rest_reply_config_init(rotmos_rest_reply_config *);
ROTMOS_API rotmos_status
rotmos_router_rest_route(rotmos_router *, const rotmos_rest_route_config *,
                         rotmos_route **, rotmos_error *);
ROTMOS_API rotmos_status rotmos_route_group_rest_route(
    rotmos_route_group *, const rotmos_rest_route_config *, rotmos_route **,
    rotmos_error *);
/* Synchronous bounded JSON composition; copies before returning. Defaults:
 * 200, application/json; charset=utf-8, no output ceiling and a trailing
 * newline. Failure preserves the existing staged reply. Header composition uses
 * reply helpers.
 */
ROTMOS_API rotmos_status
rotmos_reply_rest_json(rotmos_reply *, const lonejson_json_value *,
                       const rotmos_rest_reply_config *, rotmos_error *);
/* Generic default envelope; callers can instead own their error policy through
 * on_error. Zero status derives 400/409/501/504/500 from the cause. */
ROTMOS_API rotmos_status rotmos_reply_error_json(rotmos_reply *, int status,
                                                 const char *kind,
                                                 const rotmos_error *cause,
                                                 rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif
