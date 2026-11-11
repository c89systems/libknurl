#ifndef KNURL_OPENAPI_H
#define KNURL_OPENAPI_H
#include <knurl/router.h>
#ifdef __cplusplus
extern "C" {
#endif
struct lonejson_map;
/** Openapi schema. */
typedef struct knurl_openapi_schema {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Immutable public LoneJSON map; retain it and child maps through router
     * destruction.
     */
    const struct lonejson_map *map;
} knurl_openapi_schema;
/** Openapi response. */
typedef struct knurl_openapi_response {
    /** HTTP response status represented by this annotation. */
    int status_code;
    /** Response description borrowed until annotation attachment. */
    const char *description;
    /** Borrowed named LoneJSON schema map descriptor. */
    knurl_openapi_schema schema;
} knurl_openapi_response;
/** Annotation builder with an owned response vector and borrowed strings/maps.
 * Router attachment copies text and retains immutable map pointers.
 */
typedef struct knurl_openapi_route_doc {
    /** Copied operation summary after router annotation attachment. */
    const char *summary;
    /** Copied OpenAPI operation identifier. */
    const char *operation_id;
    /** Tag strings borrowed by the builder and copied on router attachment. */
    const char *const *tags;
    /** Number of tags. */
    size_t tag_count;
    /** Borrowed LoneJSON map descriptor for request documentation. */
    knurl_openapi_schema request_schema;
    /** Builder-owned response vector; schema maps/strings remain borrowed until
     * attachment.
     */
    knurl_openapi_response *responses;
    /** Number of used response entries. */
    size_t response_count;
    /** Allocated response-vector capacity; managed by builder helpers. */
    size_t response_capacity;
} knurl_openapi_route_doc;
/** Openapi document. */
typedef struct knurl_openapi_document {
    /** OpenAPI document title. */
    const char *title;
    /** Application API version string, not the OpenAPI specification version.
     */
    const char *version;
    /** Maximum bytes for this operation; zero-inheritance/unlimited behavior is
     * operation-specific.
     */
    size_t max_bytes;
} knurl_openapi_document;
/** Initialize openapi document with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_openapi_document_init(knurl_openapi_document *);
/** Initialize openapi route doc with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_openapi_route_doc_init(knurl_openapi_route_doc *);
/** Cleans the builder's response vector only; strings/maps remain borrowed. */
KNURL_API void knurl_openapi_route_doc_cleanup(knurl_openapi_route_doc *);
/** Build a borrowed name/map schema descriptor; caller retains the immutable
 * map.
 */
KNURL_API knurl_openapi_schema
knurl_openapi_lonejson_schema(const char *, const struct lonejson_map *);
/** Set a borrowed request schema on an annotation builder. */
KNURL_API knurl_status knurl_openapi_request_json(knurl_openapi_route_doc *,
                                                  knurl_openapi_schema,
                                                  knurl_error *);
/** Append a response schema/status to the builder; strings/maps remain borrowed
 * until router attachment.
 */
KNURL_API knurl_status knurl_openapi_response_json(knurl_openapi_route_doc *,
                                                   int status,
                                                   const char *description,
                                                   knurl_openapi_schema,
                                                   knurl_error *);
/** Router-owned annotations; copies selectors, descriptions, names and tags.
 * Maps (including child maps) stay immutable/alive through router destruction.
 * Attachment is inert and unavailable during a run. Overlapping annotations
 * for the same normalized path/method fail with CONFLICT. Routes and their
 * count remain the router's existing registry, not an application-side copy. */
KNURL_API knurl_status knurl_router_openapi_doc(knurl_router *,
                                                knurl_http_methods,
                                                const char *path,
                                                const knurl_openapi_route_doc *,
                                                knurl_error *);
/** Materialized JSON/YAML OpenAPI 3.1.0 metadata. NULL document selects
 * defaults; zero max_bytes adds no output ceiling. Output is owned through
 * owned_bytes_cleanup; failure leaves an empty output. Generation never invokes
 * route callbacks. */
KNURL_API knurl_status knurl_router_openapi(const knurl_router *,
                                            const knurl_openapi_document *,
                                            knurl_openapi_format,
                                            knurl_owned_bytes *, knurl_error *);
#ifdef __cplusplus
}
#endif
#endif
