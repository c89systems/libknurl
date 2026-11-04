#ifndef ROTMOS_LONEJSON_H
#define ROTMOS_LONEJSON_H
#include <rotmos/body.h>
#include <rotmos/router.h>
#include <lonejson.h>

#ifdef __cplusplus
extern "C" {
#endif

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

typedef struct rotmos_json_array rotmos_json_array;
typedef rotmos_status (*rotmos_json_array_item_fn)(
    const lonejson_record_view *item, void *userdata, rotmos_error *error);
typedef struct rotmos_json_array_config {
    lonejson_schema_view schema;
    const char *selector;
    rotmos_json_array_item_fn item;
    void *userdata;
} rotmos_json_array_config;
ROTMOS_API void rotmos_json_array_config_init(rotmos_json_array_config *config);
/* Incremental selected-array flow. Feed at most 64 KiB per call. An item is
 * borrowed only during its synchronous callback and is then cleaned/reused.
 * Callbacks accept an item with OK or fail terminally; AGAIN cannot pause a
 * partly consumed chunk. Supply capacity before feeding. No array is collected.
 * Recursive feed/finish/destroy is rejected and latches STATE. */
ROTMOS_API rotmos_status rotmos_json_array_new(const rotmos_json_array_config *,
                                               rotmos_json_array **,
                                               rotmos_error *);
ROTMOS_API rotmos_status rotmos_json_array_feed(rotmos_json_array *,
                                                const void *, size_t,
                                                rotmos_error *);
ROTMOS_API rotmos_status rotmos_json_array_finish(rotmos_json_array *,
                                                  rotmos_error *);
ROTMOS_API rotmos_status rotmos_json_array_destroy(rotmos_json_array *,
                                                   rotmos_error *);

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
/* Explicitly materializes output with a finite bound; zero selects 10 MiB.
 * These calls finish synchronously, so a C stack record is safe. */
ROTMOS_API rotmos_status rotmos_json_serialize_materialized(
    const lonejson_schema_view *schema, const void *record, size_t max_bytes,
    rotmos_owned_bytes *out, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_reply_json_materialized(
    rotmos_reply *reply, int status, const lonejson_schema_view *schema,
    const void *record, size_t max_bytes, rotmos_error *error);
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
} rotmos_json_typed_route_config;

ROTMOS_API void rotmos_json_route_config_init(rotmos_json_route_config *config);
ROTMOS_API void
rotmos_json_typed_route_config_init(rotmos_json_typed_route_config *config);
/* Copies declarations, borrows schemas/runtime/userdata. Records are private
 * per request and remain valid through response producer cleanup. Ordinary
 * staged middleware order applies. No schema means a NULL callback view. */
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
 * A custom decoder instead owns its successful result until release; failure
 * cleans its own partial result. All callbacks execute on the owner loop. */
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
 * 200, application/json; charset=utf-8, 10 MiB and a trailing newline. Failure
 * preserves the existing staged reply. Header composition uses reply helpers.
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
