/* OpenAPI-specific YAML layout lifted from Vectis; strings are LoneJSON-owned.
 */
#include "openapi_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void append(struct openapi_output *o, const char *text)
{
    if (o->code == LONEJSON_STATUS_OK)
        o->code =
            knurl_json_buffer_write(&o->buffer, text, strlen(text), &o->error);
}
static void quote(struct openapi_output *o, const char *text)
{
    if (o->code != LONEJSON_STATUS_OK)
        return;
    if (text == NULL)
        text = "";
    o->code = lonejson_writer_init_sink(
        o->runtime, o->writer, knurl_json_buffer_write, &o->buffer, &o->error);
    if (o->code == LONEJSON_STATUS_OK)
        o->code =
            lonejson_writer_string(o->writer, text, strlen(text), &o->error);
    if (o->code == LONEJSON_STATUS_OK)
        o->code = lonejson_writer_finish(o->writer, &o->error);
    lonejson_writer_cleanup(o->writer);
}
static void pair(struct openapi_output *o, const char *prefix,
                 const char *value)
{
    append(o, prefix);
    quote(o, value);
    append(o, "\n");
}
static void reference(struct openapi_output *o, const char *prefix,
                      knurl_openapi_schema schema)
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
    pair(o, prefix, ref);
    free(ref);
}
static void operation(struct openapi_output *o,
                      const knurl_openapi_route_doc *doc)
{
    size_t i;
    char status[4];
    if (doc->summary != NULL)
        pair(o, "      summary: ", doc->summary);
    if (doc->operation_id != NULL)
        pair(o, "      operationId: ", doc->operation_id);
    if (doc->tag_count != 0) {
        append(o, "      tags:\n");
        for (i = 0; i < doc->tag_count; ++i)
            pair(o, "        - ", doc->tags[i]);
    }
    if (doc->request_schema.map != NULL) {
        append(o,
               "      requestBody:\n        required: true\n        content:\n "
               "         application/json:\n            schema:\n");
        reference(o, "              $ref: ", doc->request_schema);
    }
    append(o, doc->response_count != 0 ? "      responses:\n"
                                       : "      responses: {}\n");
    for (i = 0; i < doc->response_count; ++i) {
        snprintf(status, sizeof(status), "%d", doc->responses[i].status_code);
        append(o, "        ");
        quote(o, status);
        append(o, ":\n");
        pair(o, "          description: ", doc->responses[i].description);
        if (doc->responses[i].schema.map != NULL) {
            append(o, "          content:\n            application/json:\n     "
                      "         schema:\n");
            reference(o, "                $ref: ", doc->responses[i].schema);
        }
    }
}
static void schema(struct openapi_output *o, knurl_openapi_schema value)
{
    const lonejson_map *map = value.map;
    const lonejson_field *f;
    knurl_openapi_schema child;
    size_t i, required = 0;
    append(o, "    ");
    quote(o, knurl_openapi_name(value));
    append(o, ":\n      type: object\n");
    append(o, map->field_count != 0 ? "      properties:\n"
                                    : "      properties: {}\n");
    for (i = 0; i < map->field_count; ++i) {
        f = &map->fields[i];
        child.map = f->submap;
        child.name = f->submap != NULL ? f->submap->name : NULL;
        append(o, "        ");
        quote(o, f->json_key);
        append(o, ":\n");
        if (f->flags & LONEJSON_FIELD_REQUIRED)
            ++required;
        if (f->kind == LONEJSON_FIELD_KIND_OBJECT && f->submap != NULL) {
            reference(o, "          $ref: ", child);
            continue;
        }
        append(o, "          type: ");
        append(o, knurl_openapi_array(f) ? "array" : knurl_openapi_type(f));
        append(o, "\n");
        if (knurl_openapi_array(f)) {
            append(o, "          items:\n");
            if (f->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY &&
                f->submap != NULL)
                reference(o, "            $ref: ", child);
            else {
                append(o, "            type: ");
                append(o, knurl_openapi_type(f));
                append(o, "\n");
            }
        }
    }
    if (required != 0) {
        append(o, "      required:\n");
        for (i = 0; i < map->field_count; ++i)
            if (map->fields[i].flags & LONEJSON_FIELD_REQUIRED)
                pair(o, "        - ", map->fields[i].json_key);
    }
}
knurl_status knurl_openapi_yaml(const knurl_router *router,
                                const knurl_openapi_document *document,
                                struct openapi_output *o, knurl_error *error)
{
    const struct knurl_openapi_entry *entry, *same;
    knurl_http_method method;
    size_t i;
    append(o, "openapi: 3.1.0\n");
    pair(o, "info:\n  title: ", document->title);
    pair(o, "  version: ", document->version);
    append(o, router->openapi != NULL ? "paths:\n" : "paths: {}\n");
    for (entry = router->openapi;
         entry != NULL && o->code == LONEJSON_STATUS_OK; entry = entry->next) {
        if (knurl_openapi_path_seen(router->openapi, entry))
            continue;
        append(o, "  ");
        quote(o, entry->path);
        append(o, ":\n");
        for (same = router->openapi; same != NULL; same = same->next)
            if (!strcmp(same->path, entry->path))
                for (method = KNURL_HTTP_GET; method <= KNURL_HTTP_MOVE;
                     ++method)
                    if (same->methods & KNURL_HTTP_METHOD_MASK(method)) {
                        append(o, "    ");
                        append(o, knurl_openapi_method(method));
                        append(o, ":\n");
                        operation(o, &same->doc);
                    }
    }
    append(o, o->count != 0 ? "components:\n  schemas:\n"
                            : "components:\n  schemas: {}\n");
    for (i = 0; i < o->count && o->code == LONEJSON_STATUS_OK; ++i)
        schema(o, o->schemas[i]);
    return knurl_json_error(o->code, &o->error, error);
}
