#ifndef ROTMOS_OPENAPI_H
#define ROTMOS_OPENAPI_H
#include <rotmos/router.h>
#ifdef __cplusplus
extern "C" {
#endif
struct lonejson_map;
typedef enum rotmos_openapi_format {
    ROTMOS_OPENAPI_JSON = 0,
    ROTMOS_OPENAPI_YAML
} rotmos_openapi_format;
typedef struct rotmos_openapi_schema {
    const char *name;
    const struct lonejson_map *map;
} rotmos_openapi_schema;
typedef struct rotmos_openapi_response {
    int status_code;
    const char *description;
    rotmos_openapi_schema schema;
} rotmos_openapi_response;
typedef struct rotmos_openapi_route_doc {
    const char *summary, *operation_id;
    const char *const *tags;
    size_t tag_count;
    rotmos_openapi_schema request_schema;
    rotmos_openapi_response *responses;
    size_t response_count, response_capacity;
} rotmos_openapi_route_doc;
typedef struct rotmos_openapi_document {
    const char *title, *version;
    size_t max_bytes;
} rotmos_openapi_document;
ROTMOS_API void rotmos_openapi_document_init(rotmos_openapi_document *);
ROTMOS_API void rotmos_openapi_route_doc_init(rotmos_openapi_route_doc *);
/* Cleans the builder's response vector only; strings/maps remain borrowed. */
ROTMOS_API void rotmos_openapi_route_doc_cleanup(rotmos_openapi_route_doc *);
ROTMOS_API rotmos_openapi_schema
rotmos_openapi_lonejson_schema(const char *, const struct lonejson_map *);
ROTMOS_API rotmos_status rotmos_openapi_request_json(rotmos_openapi_route_doc *,
                                                     rotmos_openapi_schema,
                                                     rotmos_error *);
ROTMOS_API rotmos_status rotmos_openapi_response_json(
    rotmos_openapi_route_doc *, int status, const char *description,
    rotmos_openapi_schema, rotmos_error *);
/* Router-owned annotations; copies selectors, descriptions, names and tags.
 * Maps (including child maps) stay immutable/alive through router destruction.
 * Attachment is inert and unavailable during a run. Overlapping annotations
 * for the same normalized path/method fail with CONFLICT. Routes and their
 * count remain the router's existing registry, not an application-side copy. */
ROTMOS_API rotmos_status rotmos_router_openapi_doc(
    rotmos_router *, rotmos_http_methods, const char *path,
    const rotmos_openapi_route_doc *, rotmos_error *);
/* Materialized JSON/YAML OpenAPI 3.1.0 metadata. NULL document selects
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
