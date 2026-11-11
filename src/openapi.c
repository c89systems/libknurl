/* Lifted from Vectis's OpenAPI metadata helpers; MIT, see LICENSE. */
#include "openapi_internal.h"
#include <stdlib.h>
#include <string.h>
void knurl_openapi_document_init(knurl_openapi_document *document)
{
    if (document != NULL) {
        memset(document, 0, sizeof(*document));
        document->title = "libknurl API";
        document->version = "0.0.0";
    }
}
void knurl_openapi_route_doc_init(knurl_openapi_route_doc *doc)
{
    if (doc != NULL)
        memset(doc, 0, sizeof(*doc));
}
void knurl_openapi_route_doc_cleanup(knurl_openapi_route_doc *doc)
{
    if (doc != NULL) {
        free(doc->responses);
        doc->responses = NULL;
        doc->response_count = doc->response_capacity = 0;
    }
}
knurl_openapi_schema
knurl_openapi_lonejson_schema(const char *name, const struct lonejson_map *map)
{
    knurl_openapi_schema schema;
    schema.name = name;
    schema.map = map;
    return schema;
}
const char *knurl_openapi_name(knurl_openapi_schema schema)
{
    return schema.name != NULL                              ? schema.name
           : schema.map != NULL && schema.map->name != NULL ? schema.map->name
                                                            : "Anonymous";
}
knurl_status knurl_openapi_request_json(knurl_openapi_route_doc *doc,
                                        knurl_openapi_schema schema,
                                        knurl_error *error)
{
    if (doc == NULL || schema.map == NULL)
        return knurl_fail(error, KNURL_INVALID, "openapi",
                          "provide document and request schema");
    doc->request_schema = schema;
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_openapi_response_json(knurl_openapi_route_doc *doc, int code,
                                         const char *description,
                                         knurl_openapi_schema schema,
                                         knurl_error *error)
{
    knurl_openapi_response *grown;
    size_t capacity;
    if (doc == NULL || code < 100 || code > 599 || schema.map == NULL ||
        doc->response_count > doc->response_capacity)
        return knurl_fail(
            error, KNURL_INVALID, "openapi",
            "provide document, status 100..599 and response schema");
    if (doc->response_count == doc->response_capacity) {
        capacity = doc->response_capacity == 0 ? 4 : doc->response_capacity * 2;
        if (capacity < doc->response_capacity ||
            capacity > (size_t)-1 / sizeof(*grown))
            return knurl_fail(error, KNURL_OVERFLOW, "openapi",
                              "response vector overflow");
        grown = realloc(doc->responses, capacity * sizeof(*grown));
        if (grown == NULL)
            return knurl_fail(error, KNURL_NOMEM, "openapi",
                              "growing response vector");
        doc->responses = grown;
        doc->response_capacity = capacity;
    }
    doc->responses[doc->response_count].status_code = code;
    doc->responses[doc->response_count].description = description;
    doc->responses[doc->response_count++].schema = schema;
    knurl_error_clear(error);
    return KNURL_OK;
}
static void clear_doc(knurl_openapi_route_doc *doc)
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
void knurl_openapi_clear(knurl_router *router)
{
    struct knurl_openapi_entry *entry, *next;
    for (entry = router->impl->openapi; entry != NULL; entry = next) {
        next = entry->next;
        clear_doc(&entry->doc);
        free(entry->path);
        free(entry);
    }
    router->impl->openapi = NULL;
}
static int copy_string(const char *source, const char **out)
{
    *out = knurl_copy_text(source);
    return source == NULL || *out != NULL;
}
static int copy_doc(knurl_openapi_route_doc *out,
                    const knurl_openapi_route_doc *input)
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
knurl_status knurl_router_openapi_doc(knurl_router *router,
                                      knurl_http_methods methods,
                                      const char *path,
                                      const knurl_openapi_route_doc *doc,
                                      knurl_error *error)
{
    struct knurl_openapi_entry *entry, **tail;
    size_t i, j;
    char *normalized;
    if (router == NULL || methods == 0 ||
        (methods & ~KNURL_HTTP_METHODS_ALL) != 0 || path == NULL ||
        path[0] == 0 || doc == NULL ||
        (doc->tag_count != 0 && doc->tags == NULL) ||
        (doc->response_count != 0 && doc->responses == NULL) ||
        doc->tag_count > (size_t)-1 / sizeof(*doc->tags) ||
        doc->response_count > (size_t)-1 / sizeof(*doc->responses))
        return knurl_fail(
            error, KNURL_INVALID, "openapi_doc",
            "provide router, methods, path and valid metadata vectors");
    if (router->impl->frozen)
        return knurl_fail(error, KNURL_STATE, "openapi_doc",
                          "annotations cannot change during a run");
    for (i = 0; i < doc->response_count; ++i) {
        if (doc->responses[i].status_code < 100 ||
            doc->responses[i].status_code > 599)
            return knurl_fail(error, KNURL_INVALID, "openapi_doc",
                              "invalid response status");
        for (j = 0; j < i; ++j)
            if (doc->responses[j].status_code == doc->responses[i].status_code)
                return knurl_fail(error, KNURL_CONFLICT, "openapi_doc",
                                  "duplicate response status");
    }
    normalized = normalize_path(path);
    if (normalized == NULL)
        return knurl_fail(error, KNURL_NOMEM, "openapi_doc",
                          "copying normalized path");
    for (tail = &router->impl->openapi; *tail != NULL; tail = &(*tail)->next)
        if (((*tail)->methods & methods) &&
            !strcmp((*tail)->path, normalized)) {
            free(normalized);
            return knurl_fail(error, KNURL_CONFLICT, "openapi_doc",
                              "overlapping path/method annotation");
        }
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        free(normalized);
        return knurl_fail(error, KNURL_NOMEM, "openapi_doc",
                          "allocating annotation");
    }
    entry->path = normalized;
    entry->methods = methods;
    if (!copy_doc(&entry->doc, doc)) {
        clear_doc(&entry->doc);
        free(normalized);
        free(entry);
        return knurl_fail(error, KNURL_NOMEM, "openapi_doc",
                          "copying annotation");
    }
    *tail = entry;
    knurl_error_clear(error);
    return KNURL_OK;
}
static knurl_status collect(struct openapi_output *out,
                            knurl_openapi_schema schema, knurl_error *error)
{
    size_t i, capacity;
    knurl_openapi_schema *grown;
    if (schema.map == NULL)
        return KNURL_OK;
    if (schema.map->field_count != 0 && schema.map->fields == NULL)
        return knurl_fail(error, KNURL_INVALID, "openapi",
                          "schema fields are required");
    for (i = 0; i < out->count; ++i)
        if (!strcmp(knurl_openapi_name(schema),
                    knurl_openapi_name(out->schemas[i])))
            return schema.map == out->schemas[i].map
                       ? KNURL_OK
                       : knurl_fail(error, KNURL_CONFLICT, "openapi",
                                    "different maps share a component name");
    if (out->count == out->capacity) {
        capacity = out->capacity == 0 ? 8 : out->capacity * 2;
        if (capacity < out->capacity || capacity > (size_t)-1 / sizeof(*grown))
            return knurl_fail(error, KNURL_OVERFLOW, "openapi",
                              "schema vector overflow");
        grown = realloc(out->schemas, capacity * sizeof(*grown));
        if (grown == NULL)
            return knurl_fail(error, KNURL_NOMEM, "openapi",
                              "collecting schemas");
        out->schemas = grown;
        out->capacity = capacity;
    }
    out->schemas[out->count++] = schema;
    return KNURL_OK;
}
knurl_status knurl_router_openapi(const knurl_router *router,
                                  const knurl_openapi_document *document,
                                  knurl_openapi_format format,
                                  knurl_owned_bytes *result, knurl_error *error)
{
    knurl_openapi_document defaults;
    struct openapi_output *out;
    const struct knurl_openapi_entry *e;
    knurl_openapi_schema child;
    const lonejson_map *map;
    size_t i, j;
    knurl_status status = KNURL_OK;
    if (result != NULL)
        knurl_owned_bytes_init(result);
    if (router == NULL || result == NULL ||
        (format != KNURL_OPENAPI_JSON && format != KNURL_OPENAPI_YAML))
        return knurl_fail(error, KNURL_INVALID, "openapi",
                          "provide router, format and empty output");
    knurl_openapi_document_init(&defaults);
    if (document == NULL)
        document = &defaults;
    out = calloc(1, sizeof(*out));
    if (out == NULL)
        return knurl_fail(error, KNURL_NOMEM, "openapi",
                          "allocating document generation");
    out->buffer.bound =
        document->max_bytes != 0 ? document->max_bytes : (size_t)-1;
    for (e = router->impl->openapi; e != NULL && status == KNURL_OK;
         e = e->next) {
        status = collect(out, e->doc.request_schema, error);
        for (i = 0; i < e->doc.response_count && status == KNURL_OK; ++i)
            status = collect(out, e->doc.responses[i].schema, error);
    }
    /* Iterative traversal handles recursive maps without a C-stack walk. */
    for (i = 0; i < out->count && status == KNURL_OK; ++i) {
        map = out->schemas[i].map;
        for (j = 0; j < map->field_count && status == KNURL_OK; ++j) {
            if (map->fields[j].json_key == NULL) {
                status = knurl_fail(error, KNURL_INVALID, "openapi",
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
    if (status == KNURL_OK) {
        out->runtime = lonejson_new(NULL, &out->error);
        if (out->runtime == NULL)
            status = knurl_json_error(out->error.code, &out->error, error);
    }
    if (status == KNURL_OK) {
        out->writer = calloc(1, sizeof(*out->writer));
        if (out->writer == NULL)
            status = knurl_fail(error, KNURL_NOMEM, "openapi",
                                "allocating JSON writer");
    }
    if (status == KNURL_OK)
        status = format == KNURL_OPENAPI_JSON
                     ? knurl_openapi_json(router, document, out, error)
                     : knurl_openapi_yaml(router, document, out, error);
    if (out->buffer.failure != KNURL_OK)
        status = knurl_fail(
            error, out->buffer.failure, "openapi",
            "document output exceeds its bound or allocation failed");
    if (status == KNURL_OK) {
        *result = out->buffer.bytes;
        knurl_owned_bytes_init(&out->buffer.bytes);
        knurl_error_clear(error);
    }
    knurl_owned_bytes_cleanup(&out->buffer.bytes);
    free(out->schemas);
    free(out->writer);
    if (out->runtime != NULL)
        lonejson_free(out->runtime);
    free(out);
    return status;
}
