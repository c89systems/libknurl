/* Vectis's map-to-OpenAPI projection, using the public LoneJSON writer. */
#include "openapi_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define J(call)                                                                \
    do {                                                                       \
        if (o->code == LONEJSON_STATUS_OK)                                     \
            o->code = (call);                                                  \
    } while (0)
static void key(struct openapi_output *o, const char *name)
{
    J(lonejson_writer_key(o->writer, name, strlen(name), &o->error));
}
static void text(struct openapi_output *o, const char *value)
{
    if (value == NULL)
        value = "";
    J(lonejson_writer_string(o->writer, value, strlen(value), &o->error));
}
static void object(struct openapi_output *o)
{
    J(lonejson_writer_begin_object(o->writer, &o->error));
}
static void end_object(struct openapi_output *o)
{
    J(lonejson_writer_end_object(o->writer, &o->error));
}
const char *knurl_openapi_method(knurl_http_method method)
{
    static const char *const names[] = {"",         "get",    "post", "put",
                                        "patch",    "delete", "head", "options",
                                        "propfind", "mkcol",  "copy", "move"};
    return method >= KNURL_HTTP_GET && method <= KNURL_HTTP_MOVE ? names[method]
                                                                 : "";
}
const char *knurl_openapi_type(const lonejson_field *f)
{
    switch (f->kind) {
    case LONEJSON_FIELD_KIND_I64:
    case LONEJSON_FIELD_KIND_U64:
    case LONEJSON_FIELD_KIND_I64_ARRAY:
    case LONEJSON_FIELD_KIND_U64_ARRAY:
        return "integer";
    case LONEJSON_FIELD_KIND_F64:
    case LONEJSON_FIELD_KIND_F64_ARRAY:
        return "number";
    case LONEJSON_FIELD_KIND_BOOL:
    case LONEJSON_FIELD_KIND_BOOL_ARRAY:
        return "boolean";
    case LONEJSON_FIELD_KIND_OBJECT:
    case LONEJSON_FIELD_KIND_OBJECT_ARRAY:
        return "object";
    default:
        return "string";
    }
}
int knurl_openapi_array(const lonejson_field *f)
{
    return f->kind == LONEJSON_FIELD_KIND_STRING_ARRAY ||
           f->kind == LONEJSON_FIELD_KIND_I64_ARRAY ||
           f->kind == LONEJSON_FIELD_KIND_U64_ARRAY ||
           f->kind == LONEJSON_FIELD_KIND_F64_ARRAY ||
           f->kind == LONEJSON_FIELD_KIND_BOOL_ARRAY ||
           f->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY;
}
char *knurl_openapi_ref(knurl_openapi_schema schema)
{
    static const char prefix[] = "#/components/schemas/",
                      hex[] = "0123456789ABCDEF";
    const unsigned char *name =
        (const unsigned char *)knurl_openapi_name(schema);
    size_t n = strlen((const char *)name);
    char *out, *p;
    if (n > ((size_t)-1 - sizeof(prefix)) / 3)
        return NULL;
    out = malloc(sizeof(prefix) + n * 3);
    if (out == NULL)
        return NULL;
    memcpy(out, prefix, sizeof(prefix) - 1);
    p = out + sizeof(prefix) - 1;
    for (; *name != 0; ++name) {
        unsigned char c = *name;
        if (c == '~' || c == '/') {
            *p++ = '~';
            *p++ = c == '~' ? '0' : '1';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')
            *p++ = (char)c;
        else {
            *p++ = '%';
            *p++ = hex[c >> 4];
            *p++ = hex[c & 15];
        }
    }
    *p = 0;
    return out;
}
static void reference(struct openapi_output *o, knurl_openapi_schema schema)
{
    char *ref;
    if (o->code != LONEJSON_STATUS_OK)
        return;
    ref = knurl_openapi_ref(schema);
    if (ref == NULL) {
        o->buffer.failure = KNURL_NOMEM;
        o->code = LONEJSON_STATUS_ALLOCATION_FAILED;
        return;
    }
    object(o);
    key(o, "$ref");
    text(o, ref);
    end_object(o);
    free(ref);
}
static void field(struct openapi_output *o, const lonejson_field *f)
{
    knurl_openapi_schema child;
    child.name = f->submap != NULL ? f->submap->name : NULL;
    child.map = f->submap;
    if (f->kind == LONEJSON_FIELD_KIND_OBJECT && f->submap != NULL) {
        reference(o, child);
        return;
    }
    object(o);
    key(o, "type");
    text(o, knurl_openapi_array(f) ? "array" : knurl_openapi_type(f));
    if (knurl_openapi_array(f)) {
        key(o, "items");
        if (f->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY && f->submap != NULL)
            reference(o, child);
        else {
            object(o);
            key(o, "type");
            text(o, knurl_openapi_type(f));
            end_object(o);
        }
    }
    end_object(o);
}
static void schema(struct openapi_output *o, const lonejson_map *map)
{
    size_t i, required = 0;
    object(o);
    key(o, "type");
    text(o, "object");
    key(o, "properties");
    object(o);
    for (i = 0; i < map->field_count; ++i) {
        key(o, map->fields[i].json_key);
        field(o, &map->fields[i]);
        if (map->fields[i].flags & LONEJSON_FIELD_REQUIRED)
            ++required;
    }
    end_object(o);
    if (required != 0) {
        key(o, "required");
        J(lonejson_writer_begin_array(o->writer, &o->error));
        for (i = 0; i < map->field_count; ++i)
            if (map->fields[i].flags & LONEJSON_FIELD_REQUIRED)
                text(o, map->fields[i].json_key);
        J(lonejson_writer_end_array(o->writer, &o->error));
    }
    end_object(o);
}
static void content(struct openapi_output *o, knurl_openapi_schema value)
{
    key(o, "content");
    object(o);
    key(o, "application/json");
    object(o);
    key(o, "schema");
    reference(o, value);
    end_object(o);
    end_object(o);
}
static void operation(struct openapi_output *o,
                      const knurl_openapi_route_doc *doc)
{
    size_t i;
    char status[4];
    object(o);
    if (doc->summary != NULL) {
        key(o, "summary");
        text(o, doc->summary);
    }
    if (doc->operation_id != NULL) {
        key(o, "operationId");
        text(o, doc->operation_id);
    }
    if (doc->tag_count != 0) {
        key(o, "tags");
        J(lonejson_writer_begin_array(o->writer, &o->error));
        for (i = 0; i < doc->tag_count; ++i)
            text(o, doc->tags[i]);
        J(lonejson_writer_end_array(o->writer, &o->error));
    }
    if (doc->request_schema.map != NULL) {
        key(o, "requestBody");
        object(o);
        key(o, "required");
        J(lonejson_writer_bool(o->writer, 1, &o->error));
        content(o, doc->request_schema);
        end_object(o);
    }
    key(o, "responses");
    object(o);
    for (i = 0; i < doc->response_count; ++i) {
        snprintf(status, sizeof(status), "%d", doc->responses[i].status_code);
        key(o, status);
        object(o);
        key(o, "description");
        text(o, doc->responses[i].description);
        if (doc->responses[i].schema.map != NULL)
            content(o, doc->responses[i].schema);
        end_object(o);
    }
    end_object(o);
    end_object(o);
}
int knurl_openapi_path_seen(const struct knurl_openapi_entry *head,
                            const struct knurl_openapi_entry *entry)
{
    for (; head != entry; head = head->next)
        if (!strcmp(head->path, entry->path))
            return 1;
    return 0;
}
knurl_status knurl_openapi_json(const knurl_router *router,
                                const knurl_openapi_document *document,
                                struct openapi_output *o, knurl_error *error)
{
    const struct knurl_openapi_entry *entry, *same;
    knurl_http_method method;
    size_t i;
    o->code = lonejson_writer_init_sink(
        o->runtime, o->writer, knurl_json_buffer_write, &o->buffer, &o->error);
    object(o);
    key(o, "openapi");
    text(o, "3.1.0");
    key(o, "info");
    object(o);
    key(o, "title");
    text(o, document->title);
    key(o, "version");
    text(o, document->version);
    end_object(o);
    key(o, "paths");
    object(o);
    for (entry = router->impl->openapi;
         entry != NULL && o->code == LONEJSON_STATUS_OK; entry = entry->next) {
        if (knurl_openapi_path_seen(router->impl->openapi, entry))
            continue;
        key(o, entry->path);
        object(o);
        for (same = router->impl->openapi; same != NULL; same = same->next)
            if (!strcmp(same->path, entry->path))
                for (method = KNURL_HTTP_GET; method <= KNURL_HTTP_MOVE;
                     ++method)
                    if (same->methods & KNURL_HTTP_METHOD_MASK(method)) {
                        key(o, knurl_openapi_method(method));
                        operation(o, &same->doc);
                    }
        end_object(o);
    }
    end_object(o);
    key(o, "components");
    object(o);
    key(o, "schemas");
    object(o);
    for (i = 0; i < o->count && o->code == LONEJSON_STATUS_OK; ++i) {
        key(o, knurl_openapi_name(o->schemas[i]));
        schema(o, o->schemas[i].map);
    }
    end_object(o);
    end_object(o);
    end_object(o);
    J(lonejson_writer_finish(o->writer, &o->error));
    lonejson_writer_cleanup(o->writer);
    return knurl_json_error(o->code, &o->error, error);
}
#undef J
