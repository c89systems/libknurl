#ifndef KNURL_OPENAPI_INTERNAL_H
#define KNURL_OPENAPI_INTERNAL_H
#include <knurl/openapi.h>
#include "router_internal.h"
#include "json_internal.h"
struct knurl_openapi_entry {
    char *path;
    knurl_http_methods methods;
    knurl_openapi_route_doc doc;
    struct knurl_openapi_entry *next;
};
struct openapi_output {
    struct knurl_json_buffer buffer;
    lonejson *runtime;
    lonejson_writer *writer;
    lonejson_error error;
    lonejson_status code;
    knurl_openapi_schema *schemas;
    size_t count, capacity;
};
void knurl_openapi_clear(knurl_router *);
const char *knurl_openapi_name(knurl_openapi_schema);
knurl_status knurl_openapi_json(const knurl_router *,
                                const knurl_openapi_document *,
                                struct openapi_output *, knurl_error *);
knurl_status knurl_openapi_yaml(const knurl_router *,
                                const knurl_openapi_document *,
                                struct openapi_output *, knurl_error *);
const char *knurl_openapi_method(knurl_http_method);
const char *knurl_openapi_type(const lonejson_field *);
int knurl_openapi_array(const lonejson_field *);
char *knurl_openapi_ref(knurl_openapi_schema);
int knurl_openapi_path_seen(const struct knurl_openapi_entry *,
                            const struct knurl_openapi_entry *);
#endif
