#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <rotmos/lonejson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "openapi %d: %s\n", __LINE__, #t);                 \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct record {
    char *name;
};
static const lonejson_field fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct record, name, "name")};
LONEJSON_MAP_DEFINE(record_map, struct record, fields);
static int called;
static rotmos_status never_called(rotmos_request *request, rotmos_reply *reply,
                                  void *data, rotmos_error *error)
{
    (void)request;
    (void)reply;
    (void)data;
    (void)error;
    ++called;
    return ROTMOS_STATE;
}
static char *cstr(rotmos_owned_bytes *bytes)
{
    char *out = malloc(bytes->length + 1);
    if (out != NULL) {
        memcpy(out, bytes->data, bytes->length);
        out[bytes->length] = 0;
    }
    return out;
}
static int metadata(void)
{
    rotmos_router *router;
    rotmos_route_config route;
    rotmos_openapi_route_doc doc;
    rotmos_openapi_document document;
    rotmos_openapi_response empty;
    rotmos_openapi_schema schema;
    rotmos_owned_bytes bytes;
    lonejson_json_value value;
    lonejson *runtime;
    lonejson_error native;
    rotmos_error error;
    char summary[] = "quoted \"summary\"\nline", tag[] = "test: #tag",
         component[] = "A/B~ \"";
    const char *tags[1];
    char *json, *yaml;
    CHECK(rotmos_router_new(&router, &error) == ROTMOS_OK);
    rotmos_route_config_init(&route);
    route.path = "/items/:id";
    route.path_kind = ROTMOS_PATH_PARAMS;
    route.handler = never_called;
    CHECK(rotmos_router_route(router, &route, NULL, &error) == ROTMOS_OK);
    CHECK(rotmos_router_route_count(router) == 1);
    rotmos_openapi_route_doc_init(&doc);
    tags[0] = tag;
    doc.summary = summary;
    doc.operation_id = "readItems";
    doc.tags = tags;
    doc.tag_count = 1;
    schema = rotmos_openapi_lonejson_schema(component, &record_map);
    CHECK(rotmos_openapi_request_json(&doc, schema, &error) == ROTMOS_OK);
    CHECK(rotmos_openapi_response_json(&doc, 200, "done\nnow", schema,
                                       &error) == ROTMOS_OK);
    CHECK(rotmos_openapi_response_json(&doc, 999, "bad", schema, &error) ==
          ROTMOS_INVALID);
    CHECK(rotmos_router_openapi_doc(
              router, ROTMOS_HTTP_METHODS_GET | ROTMOS_HTTP_METHODS_POST,
              "/items/:id?", &doc, &error) == ROTMOS_OK);
    CHECK(rotmos_router_openapi_doc(router, ROTMOS_HTTP_METHODS_GET,
                                    "/items/{id}", &doc,
                                    &error) == ROTMOS_CONFLICT);
    memset(summary, 'x', sizeof(summary) - 1);
    memset(tag, 'x', sizeof(tag) - 1);
    memset(component, 'x', sizeof(component) - 1);
    rotmos_openapi_route_doc_cleanup(&doc);
    rotmos_openapi_route_doc_init(&doc);
    memset(&empty, 0, sizeof(empty));
    empty.status_code = 204;
    empty.description = "empty";
    doc.responses = &empty;
    doc.response_count = 1;
    CHECK(rotmos_router_openapi_doc(router, ROTMOS_HTTP_METHODS_DELETE,
                                    "/items/:id", &doc, &error) == ROTMOS_OK);
    CHECK(rotmos_router_openapi_doc(router, 0, "/invalid", &doc, &error) ==
          ROTMOS_INVALID);
    CHECK(rotmos_router_openapi_doc(router,
                                    ROTMOS_HTTP_METHODS_ALL &
                                        ~(ROTMOS_HTTP_METHODS_GET |
                                          ROTMOS_HTTP_METHODS_POST |
                                          ROTMOS_HTTP_METHODS_DELETE),
                                    "/items/:id", &doc, &error) == ROTMOS_OK);
    CHECK(rotmos_router_route_count(router) == 1 && called == 0);
    rotmos_openapi_document_init(&document);
    document.title = "quoted \"API\"\n";
    document.version = "1.2";
    CHECK(rotmos_router_openapi(router, &document, ROTMOS_OPENAPI_JSON, &bytes,
                                &error) == ROTMOS_OK);
    runtime = lonejson_new(NULL, &native);
    CHECK(runtime != NULL);
    lonejson_json_value_init(runtime, &value);
    CHECK(lonejson_json_value_set_buffer(&value, bytes.data, bytes.length,
                                         &native) == LONEJSON_STATUS_OK);
    json = cstr(&bytes);
    CHECK(json != NULL);
    CHECK(strstr(json, "\"/items/{id}\"") != NULL &&
          strstr(json, "\"summary\":\"quoted \\\"summary\\\"\\nline\"") !=
              NULL);
    CHECK(strstr(json, "#/components/schemas/A~1B~0%20%22") != NULL &&
          strstr(json, "\"required\":[\"name\"]") != NULL);
    CHECK(strstr(json, "\"tags\":[\"test: #tag\"]") != NULL &&
          strstr(json, "\"204\":{\"description\":\"empty\"}") != NULL);
    CHECK(strstr(json, "\"propfind\"") != NULL &&
          strstr(json, "\"move\"") != NULL);
    free(json);
    rotmos_owned_bytes_cleanup(&bytes);
    lonejson_json_value_cleanup(&value);
    lonejson_free(runtime);
    CHECK(rotmos_router_openapi(router, &document, ROTMOS_OPENAPI_YAML, &bytes,
                                &error) == ROTMOS_OK);
    yaml = cstr(&bytes);
    CHECK(yaml != NULL);
    CHECK(strstr(yaml, "  \"/items/{id}\":\n") != NULL &&
          strstr(yaml, "summary: \"quoted \\\"summary\\\"\\nline\"") != NULL);
    CHECK(strstr(yaml, "\"#/components/schemas/A~1B~0%20%22\"") != NULL &&
          strstr(yaml, "required:\n        - \"name\"") != NULL);
    CHECK(strstr(yaml, "    propfind:\n") != NULL &&
          strstr(yaml, "    move:\n") != NULL);
    free(yaml);
    rotmos_owned_bytes_cleanup(&bytes);
    document.max_bytes = 1;
    CHECK(rotmos_router_openapi(router, &document, ROTMOS_OPENAPI_JSON, &bytes,
                                &error) == ROTMOS_LIMIT &&
          bytes.data == NULL && bytes.length == 0);
    CHECK(rotmos_router_openapi(router, &document, ROTMOS_OPENAPI_YAML, &bytes,
                                &error) == ROTMOS_LIMIT &&
          bytes.data == NULL && bytes.length == 0);
    CHECK(rotmos_router_openapi(router, NULL, (rotmos_openapi_format)3, &bytes,
                                &error) == ROTMOS_INVALID);
    CHECK(rotmos_router_destroy(router, &error) == ROTMOS_OK && called == 0);
    return 0;
}
static int graph(void)
{
    lonejson_map *maps;
    lonejson_field *members;
    char (*names)[20];
    rotmos_router *router;
    rotmos_openapi_route_doc doc;
    rotmos_owned_bytes bytes;
    rotmos_error error;
    size_t i;
    char *text;
    maps = calloc(512, sizeof(*maps));
    members = calloc(512, sizeof(*members));
    names = calloc(512, sizeof(*names));
    CHECK(maps != NULL && members != NULL && names != NULL);
    for (i = 0; i < 512; ++i) {
        snprintf(names[i], sizeof(names[i]), "node%lu", (unsigned long)i);
        maps[i].name = names[i];
        maps[i].struct_size = sizeof(void *);
        maps[i].fields = &members[i];
        maps[i].field_count = 1;
        members[i].json_key = "child";
        members[i].kind = LONEJSON_FIELD_KIND_OBJECT_ARRAY;
        members[i].submap = &maps[(i + 1) % 512];
    }
    CHECK(rotmos_router_new(&router, &error) == ROTMOS_OK);
    rotmos_openapi_route_doc_init(&doc);
    doc.request_schema = rotmos_openapi_lonejson_schema(NULL, &maps[0]);
    CHECK(rotmos_router_openapi_doc(router, ROTMOS_HTTP_METHODS_POST, "/graph",
                                    &doc, &error) == ROTMOS_OK);
    CHECK(rotmos_router_openapi(router, NULL, ROTMOS_OPENAPI_JSON, &bytes,
                                &error) == ROTMOS_OK);
    text = cstr(&bytes);
    CHECK(text != NULL && strstr(text, "\"node511\"") != NULL &&
          strstr(text, "\"items\":{\"$ref\":\"#/components/schemas/node0\"}") !=
              NULL);
    free(text);
    rotmos_owned_bytes_cleanup(&bytes);
    CHECK(rotmos_router_openapi(router, NULL, ROTMOS_OPENAPI_YAML, &bytes,
                                &error) == ROTMOS_OK);
    text = cstr(&bytes);
    CHECK(text != NULL &&
          strstr(text,
                 "items:\n            $ref: \"#/components/schemas/node0\"") !=
              NULL);
    free(text);
    rotmos_owned_bytes_cleanup(&bytes);
    doc.request_schema = rotmos_openapi_lonejson_schema("node0", &maps[1]);
    CHECK(rotmos_router_openapi_doc(router, ROTMOS_HTTP_METHODS_GET,
                                    "/conflict", &doc, &error) == ROTMOS_OK);
    CHECK(rotmos_router_openapi(router, NULL, ROTMOS_OPENAPI_JSON, &bytes,
                                &error) == ROTMOS_CONFLICT &&
          bytes.data == NULL);
    CHECK(rotmos_router_destroy(router, &error) == ROTMOS_OK);
    free(maps);
    free(members);
    free(names);
    return 0;
}
static void *on_default_stack(void *data)
{
    *(int *)data = graph();
    return NULL;
}
int main(void)
{
    pthread_t thread;
    int result = 1;
    CHECK(metadata() == 0);
    CHECK(pthread_create(&thread, NULL, on_default_stack, &result) == 0);
    CHECK(pthread_join(thread, NULL) == 0 && result == 0);
    return 0;
}
