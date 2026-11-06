#ifndef ROTMOS_OPENAPI_H
#define ROTMOS_OPENAPI_H
#include <rotmos/router.h>
#ifdef __cplusplus
extern "C" {
#endif
struct lonejson_map;
/** Values for openapi format. */
typedef enum rotmos_openapi_format {
    /** OpenAPI output encoding: json. */
    ROTMOS_OPENAPI_JSON = 0,
    /** OpenAPI output encoding: yaml. */
    ROTMOS_OPENAPI_YAML
} rotmos_openapi_format;
/** Openapi schema. */
typedef struct rotmos_openapi_schema {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Immutable public LoneJSON map; retain it and child maps through router
     * destruction.
     */
    const struct lonejson_map *map;
} rotmos_openapi_schema;
/** Openapi response. */
typedef struct rotmos_openapi_response {
    /** HTTP response status represented by this annotation. */
    int status_code;
    /** Response description borrowed until annotation attachment. */
    const char *description;
    /** Borrowed named LoneJSON schema map descriptor. */
    rotmos_openapi_schema schema;
} rotmos_openapi_response;
/** Annotation builder with an owned response vector and borrowed strings/maps.
 * Router attachment copies text and retains immutable map pointers.
 */
typedef struct rotmos_openapi_route_doc {
    /** Copied operation summary after router annotation attachment. */
    const char *summary;
    /** Copied OpenAPI operation identifier. */
    const char *operation_id;
    /** Tag strings borrowed by the builder and copied on router attachment. */
    const char *const *tags;
    /** Number of tags. */
    size_t tag_count;
    /** Borrowed LoneJSON map descriptor for request documentation. */
    rotmos_openapi_schema request_schema;
    /** Builder-owned response vector; schema maps/strings remain borrowed until
     * attachment.
     */
    rotmos_openapi_response *responses;
    /** Number of used response entries. */
    size_t response_count;
    /** Allocated response-vector capacity; managed by builder helpers. */
    size_t response_capacity;
} rotmos_openapi_route_doc;
/** Openapi document. */
typedef struct rotmos_openapi_document {
    /** OpenAPI document title. */
    const char *title;
    /** Application API version string, not the OpenAPI specification version.
     */
    const char *version;
    /** Maximum bytes for this operation; zero-inheritance/unlimited behavior is
     * operation-specific.
     */
    size_t max_bytes;
} rotmos_openapi_document;
/** Initialize openapi document with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_openapi_document_init(rotmos_openapi_document *);
/** Initialize openapi route doc with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_openapi_route_doc_init(rotmos_openapi_route_doc *);
/** Cleans the builder's response vector only; strings/maps remain borrowed. */
ROTMOS_API void rotmos_openapi_route_doc_cleanup(rotmos_openapi_route_doc *);
/** Build a borrowed name/map schema descriptor; caller retains the immutable
 * map.
 */
ROTMOS_API rotmos_openapi_schema
rotmos_openapi_lonejson_schema(const char *, const struct lonejson_map *);
/** Set a borrowed request schema on an annotation builder. */
ROTMOS_API rotmos_status rotmos_openapi_request_json(rotmos_openapi_route_doc *,
                                                     rotmos_openapi_schema,
                                                     rotmos_error *);
/** Append a response schema/status to the builder; strings/maps remain borrowed
 * until router attachment.
 */
ROTMOS_API rotmos_status rotmos_openapi_response_json(
    rotmos_openapi_route_doc *, int status, const char *description,
    rotmos_openapi_schema, rotmos_error *);
/** Router-owned annotations; copies selectors, descriptions, names and tags.
 * Maps (including child maps) stay immutable/alive through router destruction.
 * Attachment is inert and unavailable during a run. Overlapping annotations
 * for the same normalized path/method fail with CONFLICT. Routes and their
 * count remain the router's existing registry, not an application-side copy. */
ROTMOS_API rotmos_status rotmos_router_openapi_doc(
    rotmos_router *, rotmos_http_methods, const char *path,
    const rotmos_openapi_route_doc *, rotmos_error *);
/** Materialized JSON/YAML OpenAPI 3.1.0 metadata. NULL document selects
 * defaults; zero max_bytes adds no output ceiling. Output is owned through
 * owned_bytes_cleanup; failure leaves an empty output. Generation never invokes
 * route callbacks. */
ROTMOS_API rotmos_status rotmos_router_openapi(const rotmos_router *,
                                               const rotmos_openapi_document *,
                                               rotmos_openapi_format,
                                               rotmos_owned_bytes *,
                                               rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif
