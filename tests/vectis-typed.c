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
struct array_source {
    struct reference_source source;
    const char *text;
    size_t position, count;
    int resets, closed, fail;
    void *item;
};
static size_t read_array(struct reference_source *source, void *buffer,
                         size_t capacity, struct reference_source_error *error)
{
    struct array_source *input = source->impl;
    size_t count = strlen(input->text) - input->position;
    if (input->fail == 2) {
        error->code = 3;
        error->message = "input failed";
        return 0;
    }
    if (count > capacity)
        count = capacity;
    if (count > 7)
        count = 7;
    memcpy(buffer, input->text + input->position, count);
    input->position += count;
    return count;
}
static int reset_array(struct reference_source *source,
                       struct reference_source_error *error)
{
    struct array_source *input = source->impl;
    (void)error;
    ++input->resets;
    input->position = 0;
    return 0;
}
static void close_array(struct reference_source *source)
{
    struct array_source *input = source->impl;
    ++input->closed;
}
static reference_status array_item(void *data, size_t index, void *item,
                                   reference_error *error)
{
    struct array_source *input = data;
    struct record *record = item;
    if (item != input->item || index != input->count ||
        strcmp(record->value, index == 0 ? "one" : "two") ||
        (index == 0 && input->position >= strlen(input->text)))
        return REFERENCE_STATE;
    ++input->count;
    if (input->fail) {
        reference_error_clear(error);
        error->code = REFERENCE_TIMEOUT;
        error->source = 1;
        error->dependency_code = 19;
        strcpy(error->message, "callback timeout");
        return REFERENCE_TIMEOUT;
    }
    return REFERENCE_OK;
}
static int array_sources(void)
{
    struct reference_input_source source;
    struct array_source input;
    struct record item;
    reference_error error;
    int scenario;
    for (scenario = 0; scenario < 3; ++scenario) {
        memset(&source, 0, sizeof(source));
        memset(&input, 0, sizeof(input));
        memset(&item, 0, sizeof(item));
        input.text = "skip[{\"value\":\"one\"},{\"value\":\"two\"}]";
        input.position = 4;
        input.source.read = read_array;
        input.source.reset = reset_array;
        input.source.close = close_array;
        input.source.impl = &input;
        input.item = &item;
        input.fail = scenario;
        source.source = &input.source;
        source.path = "must-not-open";
        source.memory = "not selected";
        source.memory_size = 12;
        CHECK(reference_array_each_source(&source, NULL, &map, &item,
                                          array_item, &input, &error) ==
              (scenario == 0   ? REFERENCE_OK
               : scenario == 1 ? REFERENCE_TIMEOUT
                               : REFERENCE_INVALID));
        CHECK(input.resets == 0 && input.closed == 0 &&
              input.count == (scenario == 0   ? 2
                              : scenario == 1 ? 1
                                              : 0));
        if (scenario == 1)
            CHECK(error.dependency_code == 19 &&
                  !strcmp(error.message, "callback timeout"));
        if (scenario == 2)
            CHECK(error.source == 4 &&
                  error.dependency_code == LONEJSON_STATUS_INVALID_JSON &&
                  strstr(error.message, "input failed") != NULL);
    }
    return 0;
}
struct rewrite {
    struct reference_sink sink;
    struct array_source *input;
    char bytes[128];
    size_t length, items;
    int mode, progress, closed;
};
static int rewrite_write(struct reference_sink *sink, const void *bytes,
                         size_t length, struct reference_source_error *error)
{
    struct rewrite *output = sink->impl;
    if (output->mode == 2) {
        error->code = 3;
        error->message = "intentional sink failure";
        return 0;
    }
    if (length >= sizeof(output->bytes) - output->length)
        return 0;
    memcpy(output->bytes + output->length, bytes, length);
    output->length += length;
    output->bytes[output->length] = 0;
    if (strstr(output->bytes, "one") != NULL &&
        output->input->position < strlen(output->input->text))
        output->progress = 1;
    return 1;
}
static void rewrite_close(struct reference_sink *sink)
{
    struct rewrite *output = sink->impl;
    ++output->closed;
}
/* Baseline KEEP/DROP and failure cases from Vectis's SDK-surface fixture. */
static lonejson_status
rewrite_item(void *data, const lonejson_array_rewrite_context *context,
             void *item, lonejson_array_rewrite_result *result,
             lonejson_error *error)
{
    struct rewrite *output = data;
    struct record *record = item;
    (void)context;
    ++output->items;
    if (output->mode == 1) {
        lonejson_error_init(error);
        error->code = LONEJSON_STATUS_CALLBACK_FAILED;
        strcpy(error->message, "rewrite callback stopped");
        return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    result->action = !strcmp(record->value, "one")
                         ? LONEJSON_ARRAY_REWRITE_KEEP
                         : LONEJSON_ARRAY_REWRITE_DROP;
    return LONEJSON_STATUS_OK;
}
static int rewriting(void)
{
    struct rewrite output;
    struct array_source input;
    struct reference_input_source source;
    struct record item;
    lonejson_array_rewrite_options options;
    reference_error error;
    int scenario;
    for (scenario = 0; scenario < 3; ++scenario) {
        memset(&output, 0, sizeof(output));
        memset(&input, 0, sizeof(input));
        memset(&source, 0, sizeof(source));
        memset(&item, 0, sizeof(item));
        memset(&options, 0, sizeof(options));
        input.text = "skip[{\"value\":\"one\"},{\"value\":\"two\"}]";
        input.position = 4;
        input.source.read = read_array;
        input.source.reset = reset_array;
        input.source.close = close_array;
        input.source.impl = &input;
        source.source = &input.source;
        output.sink.write = rewrite_write;
        output.sink.close = rewrite_close;
        output.sink.impl = &output;
        output.input = &input;
        output.mode = scenario;
        options.item_map = &map;
        options.item_dst = &item;
        options.item = rewrite_item;
        options.user = &output;
        CHECK(reference_array_rewrite_source(&source, "", &output.sink,
                                             &options, &error) ==
              (scenario ? REFERENCE_INVALID : REFERENCE_OK));
        CHECK(input.resets == 0 && input.closed == 0 && output.closed == 0);
        if (scenario == 0)
            CHECK(output.items == 2 && output.progress &&
                  !strcmp(output.bytes, "[{\"value\":\"one\"}]"));
        else
            CHECK(error.source == 4 &&
                  strstr(error.message,
                         scenario == 1 ? "rewrite callback stopped"
                                       : "intentional sink failure") != NULL);
        lonejson_cleanup(&map, &item);
    }
    return 0;
}
struct xml_output {
    struct reference_sink sink;
    char bytes[128];
    size_t length;
    int fail, closed;
};
static int xml_write(struct reference_sink *sink, const void *bytes,
                     size_t length, struct reference_source_error *error)
{
    struct xml_output *out = sink->impl;
    if (out->fail) {
        error->code = 3;
        error->message = "XML sink failed";
        return 0;
    }
    if (length >= sizeof(out->bytes) - out->length)
        return 0;
    memcpy(out->bytes + out->length, bytes, length);
    out->length += length;
    out->bytes[out->length] = 0;
    return 1;
}
static void xml_close(struct reference_sink *sink)
{
    struct xml_output *out = sink->impl;
    ++out->closed;
}
static int xml_helpers(void)
{
    struct array_source input;
    struct reference_input_source source;
    struct xml_output output;
    struct reference_mutable_bytes bytes;
    reference_xml_options options;
    struct record record;
    reference_error error;
    memset(&input, 0, sizeof(input));
    memset(&source, 0, sizeof(source));
    memset(&output, 0, sizeof(output));
    reference_xml_options_init(&options);
    options.root_element = "item";
    options.trim_text = 1;
    input.text = "skip<item><value> one </value><unknown>x</unknown></item>";
    input.position = 4;
    input.source.read = read_array;
    input.source.reset = reset_array;
    input.source.close = close_array;
    input.source.impl = &input;
    source.source = &input.source;
    source.path = "must-not-open";
    source.memory = "not selected";
    source.memory_size = 12;
    memset(&record, 0xa5, sizeof(record));
    CHECK(reference_xml_parse_source(&source, &map, &options, &record,
                                     &error) == REFERENCE_OK &&
          !strcmp(record.value, "one"));
    CHECK(input.closed == 0 && input.resets == 0);
    output.sink.write = xml_write;
    output.sink.close = xml_close;
    output.sink.impl = &output;
    CHECK(reference_xml_write(&output.sink, &map, &options, &record, &error) ==
          REFERENCE_OK);
    CHECK(!strcmp(output.bytes, "<item><value>one</value></item>") &&
          output.closed == 0);
    memset(&bytes, 0, sizeof(bytes));
    CHECK(reference_xml_bytes(&map, &options, &record, &bytes, &error) ==
          REFERENCE_OK);
    CHECK(bytes.size == output.length &&
          !memcmp(bytes.data, output.bytes, bytes.size + 1));
    reference_mutable_bytes_cleanup(&bytes);
    lonejson_cleanup(&map, &record);
    input.position = 4;
    options.skip_unknown_disabled = 1;
    CHECK(reference_xml_parse(&input.source, &map, &options, &record, &error) ==
              REFERENCE_INVALID &&
          error.source == 1);
    lonejson_cleanup(&map, &record);
    input.text = "<item><value>one</value></item>";
    input.position = 0;
    input.fail = 2;
    CHECK(reference_xml_parse(&input.source, &map, &options, &record, &error) ==
              REFERENCE_STATE &&
          strstr(error.message, "input failed"));
    CHECK(input.closed == 0 && input.resets == 0);
    lonejson_cleanup(&map, &record);
    source.source = NULL;
    source.path = NULL;
    source.memory = input.text;
    source.memory_size = strlen(input.text);
    CHECK(reference_xml_parse_source(&source, &map, &options, &record,
                                     &error) == REFERENCE_OK);
    output.fail = 1;
    CHECK(reference_xml_write(&output.sink, &map, &options, &record, &error) ==
              REFERENCE_STATE &&
          error.source == 1 && !strcmp(error.message, "XML sink failed") &&
          output.closed == 0);
    CHECK(reference_xml_write(NULL, &map, &options, &record, &error) ==
          REFERENCE_INVALID);
    CHECK(reference_xml_bytes(NULL, &options, &record, &bytes, &error) ==
              REFERENCE_INVALID &&
          bytes.data == NULL && bytes.size == 0);
    lonejson_cleanup(&map, &record);
    return 0;
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
    CHECK(array_sources() == 0);
    CHECK(rewriting() == 0);
    CHECK(xml_helpers() == 0);
    CHECK(reference_json_validate_cstr("{\"ok\":true}", &error) ==
              REFERENCE_OK &&
          error.code == REFERENCE_OK);
    CHECK(reference_json_validate_cstr("{\"ok\":", &error) ==
              REFERENCE_INVALID &&
          error.source == 4 && error.dependency_code != 0 &&
          strstr(error.message, "invalid json at line") != NULL);
    CHECK(reference_json_validate_cstr("true\0ignored", &error) ==
          REFERENCE_OK);
    CHECK(reference_json_validate_cstr(NULL, &error) == REFERENCE_INVALID &&
          !strcmp(error.message, "json is required"));
    CHECK(reference_json_validate_cstr("", NULL) == REFERENCE_INVALID);
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
