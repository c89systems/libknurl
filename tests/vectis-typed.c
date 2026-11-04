#include "vectis-forward.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "typed declarations %d: %s\n", __LINE__, #x);      \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct record {
    char value[8];
};
static const lonejson_field fields[] = {LONEJSON_FIELD_STRING_FIXED_REQ(
    struct record, value, "value", LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(map, struct record, fields);
static reference_status ordinary(struct reference_app *app,
                                 struct reference_request *request, void *input,
                                 void *output, void *data,
                                 reference_error *error)
{
    (void)app;
    (void)request;
    (void)input;
    (void)output;
    (void)data;
    (void)error;
    return REFERENCE_OK;
}
static reference_status typed(struct reference_app *app,
                              struct reference_request *request, void *input,
                              struct reference_response *reply, void *data,
                              reference_error *error)
{
    (void)app;
    (void)request;
    (void)input;
    (void)data;
    return reference_json_reply_status(reply, 204, error);
}
int main(void)
{
    struct reference_app app;
    struct reference_typed_route host;
    struct reference_json_config json;
    struct reference_json_typed_config dynamic;
    struct reference_xml_config xml;
    rotmos_server_config config;
    rotmos_error native;
    reference_error error;
    lonejson_map large = map;
    memset(&app, 0, sizeof(app));
    memset(&host, 0, sizeof(host));
    host.app = &app;
    rotmos_server_config_init(&config);
    config.workers.count = 1;
    CHECK(rotmos_server_new(&config, &app.server, &native) == ROTMOS_OK);
    memset(&json, 0, sizeof(json));
    json.path = "/json";
    json.method = ROTMOS_HTTP_POST;
    json.body.mode = ROTMOS_BODY_BUFFERED;
    json.body.memory_buffer_limit_bytes = 262144UL;
    json.input_map = json.output_map = &map;
    json.input_size = json.output_size = sizeof(struct record);
    json.handler = ordinary;
    json.input_size = 0;
    CHECK(reference_json_register(&host, &json, &error) == REFERENCE_INVALID &&
          error.source == 1 &&
          !strcmp(error.message, "json route input_size is invalid"));
    json.input_size = sizeof(struct record) + 1;
    CHECK(reference_json_register(&host, &json, &error) == REFERENCE_INVALID &&
          strstr(error.message, "does not match") != NULL);
    large.struct_size = 10485761UL;
    json.input_map = &large;
    json.input_size = large.struct_size;
    CHECK(reference_json_register(&host, &json, &error) == REFERENCE_INVALID);
    json.input_map = &map;
    json.input_size = sizeof(struct record);
    json.output_size = 0;
    CHECK(reference_json_register(&host, &json, &error) == REFERENCE_INVALID &&
          strstr(error.message, "output") != NULL);
    json.output_size = sizeof(struct record);
    CHECK(reference_json_register(&host, &json, &error) == REFERENCE_OK);
    CHECK(reference_json_register(&host, &json, &error) == REFERENCE_CONFLICT);
    json.path = "/absent";
    json.input_map = json.output_map = NULL;
    json.input_size = json.output_size = (size_t)-1;
    CHECK(reference_json_register(&host, &json, &error) == REFERENCE_OK);
    memset(&dynamic, 0, sizeof(dynamic));
    dynamic.path = "/dynamic";
    dynamic.method = ROTMOS_HTTP_POST;
    dynamic.body = json.body;
    dynamic.input_map = &map;
    dynamic.input_size = sizeof(struct record) + 1;
    dynamic.handler = typed;
    CHECK(reference_json_typed_register(&host, &dynamic, &error) ==
          REFERENCE_INVALID);
    dynamic.input_size = sizeof(struct record);
    CHECK(reference_json_typed_register(&host, &dynamic, &error) ==
          REFERENCE_OK);
    memset(&xml, 0, sizeof(xml));
    xml.path = "/xml";
    xml.method = ROTMOS_HTTP_POST;
    xml.body.mode = ROTMOS_BODY_STREAMING_UPLOAD;
    xml.body.memory_buffer_limit_bytes = 262144UL;
    xml.handler = typed;
    rotmos_xml_config_init(&xml.config);
    CHECK(reference_xml_register(&host, &xml, &error) == REFERENCE_INVALID &&
          strstr(error.message, "map is required") != NULL);
    xml.input_map = &map;
    CHECK(reference_xml_register(&host, &xml, &error) == REFERENCE_INVALID);
    xml.input_size = sizeof(struct record) + 1;
    CHECK(reference_xml_register(&host, &xml, &error) == REFERENCE_INVALID);
    xml.input_map = &large;
    xml.input_size = large.struct_size;
    CHECK(reference_xml_register(&host, &xml, &error) == REFERENCE_INVALID);
    xml.input_map = &map;
    xml.input_size = sizeof(struct record);
    CHECK(reference_xml_register(&host, &xml, &error) == REFERENCE_OK);
    CHECK(rotmos_server_destroy(app.server, &native) == ROTMOS_OK);
    return 0;
}
