/* Lifted from Vectis's OpenAPI metadata helpers; MIT, see LICENSE. */
#include "openapi_internal.h"
#include <stdlib.h>
#include <string.h>
void rotmos_openapi_document_init(rotmos_openapi_document *document)
{
    if (document != NULL) {
        memset(document, 0, sizeof(*document));
        document->title = "librotmos API";
        document->version = "0.0.0";
    }
}
void rotmos_openapi_route_doc_init(rotmos_openapi_route_doc *doc)
{
    if (doc != NULL)
        memset(doc, 0, sizeof(*doc));
}
void rotmos_openapi_route_doc_cleanup(rotmos_openapi_route_doc *doc)
{
    if (doc != NULL) {
        free(doc->responses);
        doc->responses = NULL;
        doc->response_count = doc->response_capacity = 0;
    }
}
rotmos_openapi_schema
rotmos_openapi_lonejson_schema(const char *name, const struct lonejson_map *map)
{
    rotmos_openapi_schema schema;
    schema.name = name;
    schema.map = map;
    return schema;
}
const char *rotmos_openapi_name(rotmos_openapi_schema schema)
{
    return schema.name != NULL                              ? schema.name
           : schema.map != NULL && schema.map->name != NULL ? schema.map->name
                                                            : "Anonymous";
}
rotmos_status rotmos_openapi_request_json(rotmos_openapi_route_doc *doc,
                                          rotmos_openapi_schema schema,
                                          rotmos_error *error)
{
    if (doc == NULL || schema.map == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "openapi",
                           "provide document and request schema");
    doc->request_schema = schema;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
rotmos_status rotmos_openapi_response_json(rotmos_openapi_route_doc *doc,
                                           int code, const char *description,
                                           rotmos_openapi_schema schema,
                                           rotmos_error *error)
{
    rotmos_openapi_response *grown;
    size_t capacity;
    if (doc == NULL || code < 100 || code > 599 || schema.map == NULL ||
        doc->response_count > doc->response_capacity)
        return rotmos_fail(
            error, ROTMOS_INVALID, "openapi",
            "provide document, status 100..599 and response schema");
    if (doc->response_count == doc->response_capacity) {
        capacity = doc->response_capacity == 0 ? 4 : doc->response_capacity * 2;
        if (capacity < doc->response_capacity ||
            capacity > (size_t)-1 / sizeof(*grown))
            return rotmos_fail(error, ROTMOS_OVERFLOW, "openapi",
                               "response vector overflow");
        grown = realloc(doc->responses, capacity * sizeof(*grown));
        if (grown == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "openapi",
                               "growing response vector");
        doc->responses = grown;
        doc->response_capacity = capacity;
    }
    doc->responses[doc->response_count].status_code = code;
    doc->responses[doc->response_count].description = description;
    doc->responses[doc->response_count++].schema = schema;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static void clear_doc(rotmos_openapi_route_doc *doc)
{
    size_t i;
    free((void *)doc->summary);
    free((void *)doc->operation_id);
    if (doc->tags != NULL)
        for (i = 0; i < doc->tag_count; ++i)
            free((void *)doc->tags[i]);
    free((void *)doc->tags);
    free((void *)doc->request_schema.name);
    if (doc->responses != NULL)
        for (i = 0; i < doc->response_count; ++i) {
            free((void *)doc->responses[i].description);
            free((void *)doc->responses[i].schema.name);
        }
    free(doc->responses);
}
void rotmos_openapi_clear(rotmos_router *router)
{
    struct rotmos_openapi_entry *entry, *next;
    for (entry = router->openapi; entry != NULL; entry = next) {
        next = entry->next;
        clear_doc(&entry->doc);
        free(entry->path);
        free(entry);
    }
    router->openapi = NULL;
}
static int copy_string(const char *source, const char **out)
{
    *out = rotmos_copy_text(source);
    return source == NULL || *out != NULL;
}
static int copy_doc(rotmos_openapi_route_doc *out,
                    const rotmos_openapi_route_doc *input)
{
    size_t i;
    const char **tags;
    if (!copy_string(input->summary, &out->summary) ||
        !copy_string(input->operation_id, &out->operation_id) ||
        !copy_string(input->request_schema.name, &out->request_schema.name))
        return 0;
    out->request_schema.map = input->request_schema.map;
    out->tag_count = input->tag_count;
    out->response_count = input->response_count;
    tags =
        input->tag_count != 0 ? calloc(input->tag_count, sizeof(*tags)) : NULL;
    out->tags = tags;
    if (input->tag_count != 0 && tags == NULL)
        return 0;
    for (i = 0; i < input->tag_count; ++i)
        if (!copy_string(input->tags[i], &tags[i]))
            return 0;
    out->responses =
        input->response_count != 0
            ? calloc(input->response_count, sizeof(*out->responses))
            : NULL;
    if (input->response_count != 0 && out->responses == NULL)
        return 0;
    for (i = 0; i < input->response_count; ++i) {
        out->responses[i].status_code = input->responses[i].status_code;
        out->responses[i].schema.map = input->responses[i].schema.map;
        if (!copy_string(input->responses[i].description,
                         &out->responses[i].description) ||
            !copy_string(input->responses[i].schema.name,
                         &out->responses[i].schema.name))
            return 0;
    }
    return 1;
}
static char *normalize_path(const char *path)
{
    const char *p = path, *start;
    char *out, *cursor;
    size_t n = strlen(path);
    if (n > ((size_t)-1 - 1) / 2)
        return NULL;
    out = malloc(n * 2 + 1);
    if (out == NULL)
        return NULL;
    cursor = out;
    while (*p != 0) {
        if (*p != ':') {
            *cursor++ = *p++;
            continue;
        }
        ++p;
        start = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_')
            ++p;
        *cursor++ = '{';
        memcpy(cursor, start, (size_t)(p - start));
        cursor += p - start;
        *cursor++ = '}';
        if (*p == '?')
            ++p;
    }
    *cursor = 0;
    return out;
}
rotmos_status rotmos_router_openapi_doc(rotmos_router *router,
                                        rotmos_http_methods methods,
                                        const char *path,
                                        const rotmos_openapi_route_doc *doc,
                                        rotmos_error *error)
{
    struct rotmos_openapi_entry *entry, **tail;
    size_t i, j;
    char *normalized;
    if (router == NULL || methods == 0 ||
        (methods & ~ROTMOS_HTTP_METHODS_ALL) != 0 || path == NULL ||
        path[0] == 0 || doc == NULL ||
        (doc->tag_count != 0 && doc->tags == NULL) ||
        (doc->response_count != 0 && doc->responses == NULL) ||
        doc->tag_count > (size_t)-1 / sizeof(*doc->tags) ||
        doc->response_count > (size_t)-1 / sizeof(*doc->responses))
        return rotmos_fail(
            error, ROTMOS_INVALID, "openapi_doc",
            "provide router, methods, path and valid metadata vectors");
    if (router->frozen)
        return rotmos_fail(error, ROTMOS_STATE, "openapi_doc",
                           "annotations cannot change during a run");
    for (i = 0; i < doc->response_count; ++i) {
        if (doc->responses[i].status_code < 100 ||
            doc->responses[i].status_code > 599)
            return rotmos_fail(error, ROTMOS_INVALID, "openapi_doc",
                               "invalid response status");
        for (j = 0; j < i; ++j)
            if (doc->responses[j].status_code == doc->responses[i].status_code)
                return rotmos_fail(error, ROTMOS_CONFLICT, "openapi_doc",
                                   "duplicate response status");
    }
    normalized = normalize_path(path);
    if (normalized == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "openapi_doc",
                           "copying normalized path");
    for (tail = &router->openapi; *tail != NULL; tail = &(*tail)->next)
        if (((*tail)->methods & methods) &&
            !strcmp((*tail)->path, normalized)) {
            free(normalized);
            return rotmos_fail(error, ROTMOS_CONFLICT, "openapi_doc",
                               "overlapping path/method annotation");
        }
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        free(normalized);
        return rotmos_fail(error, ROTMOS_NOMEM, "openapi_doc",
                           "allocating annotation");
    }
    entry->path = normalized;
    entry->methods = methods;
    if (!copy_doc(&entry->doc, doc)) {
        clear_doc(&entry->doc);
        free(normalized);
        free(entry);
        return rotmos_fail(error, ROTMOS_NOMEM, "openapi_doc",
                           "copying annotation");
    }
    *tail = entry;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static rotmos_status collect(struct openapi_output *out,
                             rotmos_openapi_schema schema, rotmos_error *error)
{
    size_t i, capacity;
    rotmos_openapi_schema *grown;
    if (schema.map == NULL)
        return ROTMOS_OK;
    if (schema.map->field_count != 0 && schema.map->fields == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "openapi",
                           "schema fields are required");
    for (i = 0; i < out->count; ++i)
        if (!strcmp(rotmos_openapi_name(schema),
                    rotmos_openapi_name(out->schemas[i])))
            return schema.map == out->schemas[i].map
                       ? ROTMOS_OK
                       : rotmos_fail(error, ROTMOS_CONFLICT, "openapi",
                                     "different maps share a component name");
    if (out->count == out->capacity) {
        capacity = out->capacity == 0 ? 8 : out->capacity * 2;
        if (capacity < out->capacity || capacity > (size_t)-1 / sizeof(*grown))
            return rotmos_fail(error, ROTMOS_OVERFLOW, "openapi",
                               "schema vector overflow");
        grown = realloc(out->schemas, capacity * sizeof(*grown));
        if (grown == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "openapi",
                               "collecting schemas");
        out->schemas = grown;
        out->capacity = capacity;
    }
    out->schemas[out->count++] = schema;
    return ROTMOS_OK;
}
rotmos_status rotmos_router_openapi(const rotmos_router *router,
                                    const rotmos_openapi_document *document,
                                    rotmos_openapi_format format,
                                    rotmos_owned_bytes *result,
                                    rotmos_error *error)
{
    rotmos_openapi_document defaults;
    struct openapi_output *out;
    const struct rotmos_openapi_entry *e;
    rotmos_openapi_schema child;
    const lonejson_map *map;
    size_t i, j;
    rotmos_status status = ROTMOS_OK;
    if (result != NULL)
        rotmos_owned_bytes_init(result);
    if (router == NULL || result == NULL ||
        (format != ROTMOS_OPENAPI_JSON && format != ROTMOS_OPENAPI_YAML))
        return rotmos_fail(error, ROTMOS_INVALID, "openapi",
                           "provide router, format and empty output");
    rotmos_openapi_document_init(&defaults);
    if (document == NULL)
        document = &defaults;
    out = calloc(1, sizeof(*out));
    if (out == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "openapi",
                           "allocating document generation");
    out->buffer.bound =
        document->max_bytes != 0 ? document->max_bytes : (size_t)-1;
    for (e = router->openapi; e != NULL && status == ROTMOS_OK; e = e->next) {
        status = collect(out, e->doc.request_schema, error);
        for (i = 0; i < e->doc.response_count && status == ROTMOS_OK; ++i)
            status = collect(out, e->doc.responses[i].schema, error);
    }
    /* Iterative traversal handles recursive maps without a C-stack walk. */
    for (i = 0; i < out->count && status == ROTMOS_OK; ++i) {
        map = out->schemas[i].map;
        for (j = 0; j < map->field_count && status == ROTMOS_OK; ++j) {
            if (map->fields[j].json_key == NULL) {
                status = rotmos_fail(error, ROTMOS_INVALID, "openapi",
                                     "field names are required");
                break;
            }
            if ((map->fields[j].kind == LONEJSON_FIELD_KIND_OBJECT ||
                 map->fields[j].kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY) &&
                map->fields[j].submap != NULL) {
                child.name = map->fields[j].submap->name;
                child.map = map->fields[j].submap;
                status = collect(out, child, error);
            }
        }
    }
    if (status == ROTMOS_OK) {
        out->runtime = lonejson_new(NULL, &out->error);
        if (out->runtime == NULL)
            status = rotmos_json_error(out->error.code, &out->error, error);
    }
    if (status == ROTMOS_OK) {
        out->writer = calloc(1, sizeof(*out->writer));
        if (out->writer == NULL)
            status = rotmos_fail(error, ROTMOS_NOMEM, "openapi",
                                 "allocating JSON writer");
    }
    if (status == ROTMOS_OK)
        status = format == ROTMOS_OPENAPI_JSON
                     ? rotmos_openapi_json(router, document, out, error)
                     : rotmos_openapi_yaml(router, document, out, error);
    if (out->buffer.failure != ROTMOS_OK)
        status = rotmos_fail(
            error, out->buffer.failure, "openapi",
            "document output exceeds its bound or allocation failed");
    if (status == ROTMOS_OK) {
        *result = out->buffer.bytes;
        rotmos_owned_bytes_init(&out->buffer.bytes);
        rotmos_error_clear(error);
    }
    rotmos_owned_bytes_cleanup(&out->buffer.bytes);
    free(out->schemas);
    free(out->writer);
    if (out->runtime != NULL)
        lonejson_free(out->runtime);
    free(out);
    return status;
}
