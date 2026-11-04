#ifndef ROTMOS_OPENAPI_INTERNAL_H
#define ROTMOS_OPENAPI_INTERNAL_H
#include <rotmos/openapi.h>
#include "router_internal.h"
#include "json_internal.h"
struct rotmos_openapi_entry {
    char *path;
    rotmos_http_methods methods;
    rotmos_openapi_route_doc doc;
    struct rotmos_openapi_entry *next;
};
struct openapi_output {
    struct rotmos_json_buffer buffer;
    lonejson *runtime;
    lonejson_writer *writer;
    lonejson_error error;
    lonejson_status code;
    rotmos_openapi_schema *schemas;
    size_t count, capacity;
};
void rotmos_openapi_clear(rotmos_router *);
const char *rotmos_openapi_name(rotmos_openapi_schema);
rotmos_status rotmos_openapi_json(const rotmos_router *,
                                  const rotmos_openapi_document *,
                                  struct openapi_output *, rotmos_error *);
rotmos_status rotmos_openapi_yaml(const rotmos_router *,
                                  const rotmos_openapi_document *,
                                  struct openapi_output *, rotmos_error *);
const char *rotmos_openapi_method(rotmos_http_method);
const char *rotmos_openapi_type(const lonejson_field *);
int rotmos_openapi_array(const lonejson_field *);
char *rotmos_openapi_ref(rotmos_openapi_schema);
int rotmos_openapi_path_seen(const struct rotmos_openapi_entry *,
                             const struct rotmos_openapi_entry *);
#endif
