#include <rotmos/lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            (void)fprintf(stderr, "JSON %d: %s\n", __LINE__, #t);              \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct record {
    char *message;
};
static const lonejson_field fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct record, message, "message")};
LONEJSON_MAP_DEFINE(map, struct record, fields);
/* A declared record size is a schema property, not an HTTP/output limit. */
struct large_record {
    char value[10485761UL];
};
static const lonejson_field large_fields[] = {LONEJSON_FIELD_STRING_FIXED_REQ(
    struct large_record, value, "value", LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(large_map, struct large_record, large_fields);
struct lifetime {
    int closed;
    rotmos_status outcome;
};
static void closed(void *data, rotmos_status outcome)
{
    struct lifetime *l;
    l = data;
    ++l->closed;
    l->outcome = outcome;
}
struct array_input {
    const char *text;
    size_t position;
    int ended, closed;
    rotmos_status fail;
};
static rotmos_status array_read(void *buffer, size_t capacity, size_t *produced,
                                void *data, rotmos_error *error)
{
    struct array_input *input = data;
    size_t count = strlen(input->text) - input->position;
    (void)error;
    *produced = 0;
    if (input->fail != ROTMOS_OK)
        return input->fail;
    if (count == 0) {
        input->ended = 1;
        return ROTMOS_EOF;
    }
    if (count > 7)
        count = 7;
    if (count > capacity)
        count = capacity;
    memcpy(buffer, input->text + input->position, count);
    input->position += count;
    *produced = count;
    return ROTMOS_OK;
}
static void array_close(void *data, rotmos_status status)
{
    struct array_input *input = data;
    (void)status;
    ++input->closed;
}
struct items {
    struct record *storage;
    struct array_input *input;
    size_t count;
    int fail;
};
static rotmos_status item(size_t index, const lonejson_record_view *view,
                          void *data, rotmos_error *error)
{
    struct items *items;
    struct record *record;
    items = data;
    record = view->record;
    if (record != items->storage || record->message == NULL ||
        view->schema.map != &map || index != items->count)
        return ROTMOS_INVALID;
    if (index == 0 && items->input != NULL && items->input->ended)
        return ROTMOS_STATE;
    ++items->count;
    if (items->fail && index == 1) {
        rotmos_error_clear(error);
        error->status = ROTMOS_STATE;
        strcpy(error->message, "array callback stopped");
        return ROTMOS_STATE;
    }
    return ROTMOS_OK;
}
static int selected_items(void)
{
    static const char document[] = "{\"items\":[{\"message\":\"first\"},"
                                   "{\"message\":\"second\"}]}";
    lonejson_schema_view schema;
    struct record storage;
    struct items items;
    rotmos_source *source;
    rotmos_source_config config;
    struct array_input input;
    rotmos_bytes bytes;
    rotmos_error error;
    int scenario;
    CHECK(rotmos_json_schema_from_map(NULL, &map, &schema, &error) ==
          ROTMOS_OK);
    for (scenario = 0; scenario < 3; ++scenario) {
        memset(&items, 0, sizeof(items));
        memset(&storage, 0, sizeof(storage));
        items.storage = &storage;
        items.fail = scenario == 1;
        bytes.data = document;
        bytes.length = sizeof(document) - 1 - (scenario == 2 ? 5 : 0);
        CHECK(rotmos_source_bytes(bytes, &source, &error) == ROTMOS_OK);
        CHECK(rotmos_json_array_each_source(source, "items", &schema, &storage,
                                            item, &items, &error) ==
              (scenario == 0   ? ROTMOS_OK
               : scenario == 1 ? ROTMOS_STATE
                               : ROTMOS_INVALID));
        CHECK(items.count == (scenario == 2 ? 1 : 2) &&
              storage.message == NULL);
        if (scenario == 1)
            CHECK(strcmp(error.message, "array callback stopped") == 0);
        CHECK(rotmos_source_reset(source, &error) == ROTMOS_OK);
        CHECK(rotmos_json_array_each_source(source, "items", &schema, NULL,
                                            item, &items,
                                            &error) == ROTMOS_INVALID);
        CHECK(rotmos_json_array_each_source(source, "items", &schema, &storage,
                                            NULL, &items,
                                            &error) == ROTMOS_INVALID);
        CHECK(rotmos_source_destroy(source, &error) == ROTMOS_OK);
    }
    for (scenario = 0; scenario < 3; ++scenario) {
        memset(&input, 0, sizeof(input));
        input.text = document;
        input.fail = scenario == 0   ? ROTMOS_OK
                     : scenario == 1 ? ROTMOS_IO
                                     : ROTMOS_AGAIN;
        memset(&items, 0, sizeof(items));
        items.input = &input;
        items.storage = &storage;
        rotmos_source_config_init(&config);
        config.read = array_read;
        config.close = array_close;
        config.userdata = &input;
        CHECK(rotmos_source_new(&config, &source, &error) == ROTMOS_OK);
        CHECK(rotmos_json_array_each_source(source, "items", &schema, &storage,
                                            item, &items, &error) ==
              (scenario == 0   ? ROTMOS_OK
               : scenario == 1 ? ROTMOS_IO
                               : ROTMOS_STATE));
        CHECK(input.closed == 0 && storage.message == NULL &&
              items.count == (scenario == 0 ? 2 : 0));
        CHECK(rotmos_source_destroy(source, &error) == ROTMOS_OK &&
              input.closed == 1);
    }
    return 0;
}
int main(void)
{
    struct record record;
    struct lifetime lifetime;
    lonejson_schema_view schema;
    rotmos_json_source_config config;
    rotmos_source *source;
    rotmos_reply *reply;
    rotmos_owned_bytes bytes;
    rotmos_error error;
    rotmos_status status;
    char buffer[7];
    char *collected;
    size_t count;
    size_t length;
    CHECK(selected_items() == 0);
    CHECK(rotmos_json_schema_from_map(NULL, &large_map, &schema, &error) ==
              ROTMOS_OK &&
          schema.record_size == sizeof(struct large_record));
    memset(&lifetime, 0, sizeof(lifetime));
    record.message = malloc(131073);
    collected = malloc(131100);
    CHECK(record.message != NULL && collected != NULL);
    memset(record.message, 'x', 131072);
    record.message[131072] = 0;
    CHECK(rotmos_json_schema_from_map(NULL, &map, &schema, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_json_serialize_materialized(&schema, &record, 16, &bytes,
                                             &error) == ROTMOS_LIMIT &&
          bytes.data == NULL);
    CHECK(rotmos_json_serialize_materialized(&schema, &record, 0, &bytes,
                                             &error) == ROTMOS_OK &&
          bytes.length > 131072);
    rotmos_json_source_config_init(&config);
    config.record.schema = schema;
    config.record.record = &record;
    config.release = closed;
    config.userdata = &lifetime;
    CHECK(rotmos_source_json(&config, &source, &error) == ROTMOS_OK &&
          lifetime.closed == 0);
    config.record.record = NULL;
    length = 0;
    for (;;) {
        status =
            rotmos_source_read(source, buffer, sizeof(buffer), &count, &error);
        if (status == ROTMOS_EOF)
            break;
        CHECK(status == ROTMOS_OK && count > 0 && count <= sizeof(buffer) &&
              length + count <= 131100);
        memcpy(collected + length, buffer, count);
        length += count;
    }
    CHECK(length == bytes.length && memcmp(collected, bytes.data, length) == 0);
    rotmos_owned_bytes_cleanup(&bytes);
    CHECK(rotmos_source_destroy(source, &error) == ROTMOS_OK &&
          lifetime.closed == 1 && lifetime.outcome == ROTMOS_OK);
    config.record.record = &record;
    CHECK(rotmos_source_json(&config, &source, &error) == ROTMOS_OK);
    CHECK(rotmos_source_destroy(source, &error) == ROTMOS_OK &&
          lifetime.closed == 2 && lifetime.outcome == ROTMOS_CANCELLED);
    CHECK(rotmos_reply_new(&reply, &error) == ROTMOS_OK);
    CHECK(rotmos_reply_json(reply, 99, &config, &error) == ROTMOS_INVALID &&
          lifetime.closed == 2);
    CHECK(rotmos_reply_json(reply, 200, &config, &error) == ROTMOS_OK);
    CHECK(rotmos_reply_status(reply, 204, &error) == ROTMOS_OK);
    CHECK(rotmos_reply_destroy(reply, &error) == ROTMOS_OK &&
          lifetime.closed == 3 && lifetime.outcome == ROTMOS_CANCELLED);
    schema.record_size++;
    CHECK(rotmos_json_serialize_materialized(&schema, &record, 0, &bytes,
                                             &error) == ROTMOS_INVALID);
    rotmos_owned_bytes_cleanup(&bytes);
    free(collected);
    free(record.message);
    return 0;
}
