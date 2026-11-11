#include <knurl/lonejson.h>
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
struct fixed_record {
    lonejson_i64_array nums;
    char *message;
};
static const lonejson_field fixed_fields[] = {
    LONEJSON_FIELD_I64_ARRAY(struct fixed_record, nums, "nums",
                             LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ALLOC(struct fixed_record, message, "message")};
LONEJSON_MAP_DEFINE(fixed_map, struct fixed_record, fixed_fields);
struct fixed_items {
    struct fixed_record *storage;
    lonejson_int64 *backing;
    size_t count;
    int fail;
};
static knurl_status fixed_item(size_t index, const lonejson_record_view *view,
                               void *data, knurl_error *error)
{
    struct fixed_items *items = data;
    struct fixed_record *record = view->record;
    (void)error;
    if (record != items->storage || record->nums.items != items->backing ||
        record->nums.capacity != 1 ||
        record->nums.flags != LONEJSON_ARRAY_FIXED_CAPACITY ||
        index != items->count || index > 2)
        return KNURL_STATE;
    if (index < 2 && (record->nums.count != 1 ||
                      record->nums.items[0] != (lonejson_int64)(index + 1) ||
                      record->message == NULL))
        return KNURL_STATE;
    if (index == 2 && (record->nums.count != 0 || record->message != NULL))
        return KNURL_STATE;
    ++items->count;
    return items->fail ? KNURL_CANCELLED : KNURL_OK;
}
static int fixed_items(void)
{
    static const char *documents[] = {
        ("[{\"nums\":[1],\"message\":\"one\"},"
         "{\"nums\":[2],\"message\":\"two\"},{}]"),
        "[{\"message\":\"allocated\",\"nums\":[1,2]}]",
        "[{\"nums\":[1],\"message\":\"one\"}]",
        "[{\"message\":\"allocated\",\"nums\":[1,"};
    static const knurl_status statuses[] = {KNURL_OK, KNURL_LIMIT,
                                            KNURL_CANCELLED, KNURL_INVALID};
    struct fixed_record storage;
    struct fixed_items items;
    lonejson_int64 backing[1];
    lonejson_schema_view schema;
    lonejson_config config;
    lonejson *runtime;
    lonejson_error native;
    knurl_source *source;
    knurl_bytes bytes;
    knurl_error error;
    int supplied, scenario;
    for (supplied = 0; supplied < 3; ++supplied) {
        config = lonejson_default_config();
        config.clear_destination_by_default = supplied != 2;
        runtime = supplied ? lonejson_new(&config, &native) : NULL;
        CHECK(!supplied || runtime != NULL);
        CHECK(knurl_json_schema_from_map(runtime, &fixed_map, &schema,
                                         &error) == KNURL_OK);
        if (lonejson_init(runtime, &fixed_map, &storage, NULL) !=
            LONEJSON_STATUS_OK)
            abort();
        storage.nums.items = backing;
        storage.nums.capacity = 1;
        storage.nums.flags = LONEJSON_ARRAY_FIXED_CAPACITY;
        for (scenario = 0; scenario < 4; ++scenario) {
            memset(&items, 0, sizeof(items));
            items.storage = &storage;
            items.backing = backing;
            items.fail = scenario == 2;
            bytes.data = documents[scenario];
            bytes.length = strlen(documents[scenario]);
            CHECK(knurl_source_bytes(bytes, &source, &error) == KNURL_OK);
            CHECK(knurl_json_array_each_source(source, NULL, &schema, &storage,
                                               fixed_item, &items,
                                               &error) == statuses[scenario]);
            CHECK(items.count == (scenario == 0 ? 3 : scenario == 2 ? 1 : 0));
            CHECK(storage.nums.items == backing && storage.nums.count == 0 &&
                  storage.nums.capacity == 1 &&
                  storage.nums.flags == LONEJSON_ARRAY_FIXED_CAPACITY &&
                  storage.message == NULL);
            CHECK(knurl_source_destroy(source, &error) == KNURL_OK);
        }
        if (lonejson_cleanup(&fixed_map, &storage, NULL) != LONEJSON_STATUS_OK)
            abort();
        if (supplied)
            lonejson_free(runtime);
    }
    return 0;
}
/* A declared record size is a schema property, not an HTTP/output limit. */
struct large_record {
    char value[10485761UL];
};
static const lonejson_field large_fields[] = {LONEJSON_FIELD_STRING_FIXED_REQ(
    struct large_record, value, "value", LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(large_map, struct large_record, large_fields);
struct lifetime {
    int closed;
    knurl_status outcome;
};
static void closed(void *data, knurl_status outcome)
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
    knurl_status fail;
};
static knurl_status array_read(void *buffer, size_t capacity, size_t *produced,
                               void *data, knurl_error *error)
{
    struct array_input *input = data;
    size_t count = strlen(input->text) - input->position;
    (void)error;
    *produced = 0;
    if (input->fail != KNURL_OK)
        return input->fail;
    if (count == 0) {
        input->ended = 1;
        return KNURL_EOF;
    }
    if (count > 7)
        count = 7;
    if (count > capacity)
        count = capacity;
    memcpy(buffer, input->text + input->position, count);
    input->position += count;
    *produced = count;
    return KNURL_OK;
}
static void array_close(void *data, knurl_status status)
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
static knurl_status item(size_t index, const lonejson_record_view *view,
                         void *data, knurl_error *error)
{
    struct items *items;
    struct record *record;
    items = data;
    record = view->record;
    if (record != items->storage || record->message == NULL ||
        view->schema.map != &map || index != items->count)
        return KNURL_INVALID;
    if (index == 0 && items->input != NULL && items->input->ended)
        return KNURL_STATE;
    ++items->count;
    if (items->fail && index == 1) {
        knurl_error_clear(error);
        error->status = KNURL_STATE;
        strcpy(error->message, "array callback stopped");
        return KNURL_STATE;
    }
    return KNURL_OK;
}
static int selected_items(void)
{
    static const char document[] = "{\"items\":[{\"message\":\"first\"},"
                                   "{\"message\":\"second\"}]}";
    lonejson_schema_view schema;
    struct record storage;
    struct items items;
    knurl_source *source;
    knurl_source_config config;
    struct array_input input;
    knurl_bytes bytes;
    knurl_error error;
    int scenario;
    CHECK(knurl_json_schema_from_map(NULL, &map, &schema, &error) == KNURL_OK);
    for (scenario = 0; scenario < 3; ++scenario) {
        memset(&items, 0, sizeof(items));
        memset(&storage, 0, sizeof(storage));
        items.storage = &storage;
        items.fail = scenario == 1;
        bytes.data = document;
        bytes.length = sizeof(document) - 1 - (scenario == 2 ? 5 : 0);
        CHECK(knurl_source_bytes(bytes, &source, &error) == KNURL_OK);
        CHECK(knurl_json_array_each_source(source, "items", &schema, &storage,
                                           item, &items, &error) ==
              (scenario == 0   ? KNURL_OK
               : scenario == 1 ? KNURL_STATE
                               : KNURL_INVALID));
        CHECK(items.count == (scenario == 2 ? 1 : 2) &&
              storage.message == NULL);
        if (scenario == 1)
            CHECK(strcmp(error.message, "array callback stopped") == 0);
        CHECK(knurl_source_reset(source, &error) == KNURL_OK);
        CHECK(knurl_json_array_each_source(source, "items", &schema, NULL, item,
                                           &items, &error) == KNURL_INVALID);
        CHECK(knurl_json_array_each_source(source, "items", &schema, &storage,
                                           NULL, &items,
                                           &error) == KNURL_INVALID);
        CHECK(knurl_source_destroy(source, &error) == KNURL_OK);
    }
    for (scenario = 0; scenario < 3; ++scenario) {
        memset(&input, 0, sizeof(input));
        input.text = document;
        input.fail = scenario == 0   ? KNURL_OK
                     : scenario == 1 ? KNURL_IO
                                     : KNURL_AGAIN;
        memset(&items, 0, sizeof(items));
        items.input = &input;
        items.storage = &storage;
        knurl_source_config_init(&config);
        config.read = array_read;
        config.close = array_close;
        config.userdata = &input;
        CHECK(knurl_source_new(&config, &source, &error) == KNURL_OK);
        CHECK(knurl_json_array_each_source(source, "items", &schema, &storage,
                                           item, &items, &error) ==
              (scenario == 0   ? KNURL_OK
               : scenario == 1 ? KNURL_IO
                               : KNURL_STATE));
        CHECK(input.closed == 0 && storage.message == NULL &&
              items.count == (scenario == 0 ? 2 : 0));
        CHECK(knurl_source_destroy(source, &error) == KNURL_OK &&
              input.closed == 1);
    }
    return 0;
}
static int parse_sources(void)
{
    struct array_input input;
    struct record record;
    lonejson_schema_view schema;
    knurl_source_config config;
    knurl_source *source;
    knurl_error error;
    int scenario;
    CHECK(knurl_json_schema_from_map(NULL, &map, &schema, &error) == KNURL_OK);
    for (scenario = 0; scenario < 4; ++scenario) {
        memset(&input, 0, sizeof(input));
        memset(&record, 0, sizeof(record));
        input.text = scenario == 1 ? "{\"message\":\"broken\""
                                   : "{\"message\":\"parsed\"}";
        input.fail = scenario == 2   ? KNURL_IO
                     : scenario == 3 ? KNURL_AGAIN
                                     : KNURL_OK;
        knurl_source_config_init(&config);
        config.read = array_read;
        config.close = array_close;
        config.userdata = &input;
        CHECK(knurl_source_new(&config, &source, &error) == KNURL_OK);
        CHECK(knurl_json_parse_source(source, &schema, &record, &error) ==
              (scenario == 0   ? KNURL_OK
               : scenario == 1 ? KNURL_INVALID
               : scenario == 2 ? KNURL_IO
                               : KNURL_STATE));
        CHECK(input.closed == 0);
        if (scenario == 0)
            CHECK(input.ended && record.message != NULL &&
                  !strcmp(record.message, "parsed"));
        if (lonejson_cleanup(&map, &record, NULL) != LONEJSON_STATUS_OK)
            abort();
        CHECK(knurl_source_destroy(source, &error) == KNURL_OK &&
              input.closed == 1);
    }
    return 0;
}
struct sink_fixture {
    char *buffer;
    size_t length, calls;
    int fail;
};
static knurl_status write_sink(const void *bytes, size_t length, void *data,
                               knurl_error *error)
{
    struct sink_fixture *sink = data;
    ++sink->calls;
    if (sink->fail == 2)
        return KNURL_AGAIN;
    if (length > 4096 || sink->length + length > 131100)
        return KNURL_LIMIT;
    if (sink->fail && sink->calls == 2) {
        knurl_error_clear(error);
        error->status = KNURL_IO;
        error->native_code = 17;
        strcpy(error->message, "sink stopped");
        return KNURL_IO;
    }
    memcpy(sink->buffer + sink->length, bytes, length);
    sink->length += length;
    return KNURL_OK;
}
int main(void)
{
    struct record record;
    struct lifetime lifetime;
    lonejson_schema_view schema;
    knurl_json_source_config config;
    knurl_source *source;
    knurl_reply *reply;
    knurl_owned_bytes bytes;
    knurl_error error;
    knurl_status status;
    char buffer[7];
    char *collected;
    size_t count;
    size_t length;
    struct sink_fixture sink;
    CHECK(selected_items() == 0);
    CHECK(fixed_items() == 0);
    CHECK(parse_sources() == 0);
    CHECK(knurl_json_schema_from_map(NULL, &large_map, &schema, &error) ==
              KNURL_OK &&
          schema.record_size == sizeof(struct large_record));
    memset(&lifetime, 0, sizeof(lifetime));
    record.message = malloc(131073);
    collected = malloc(131100);
    CHECK(record.message != NULL && collected != NULL);
    memset(record.message, 'x', 131072);
    record.message[131072] = 0;
    CHECK(knurl_json_schema_from_map(NULL, &map, &schema, &error) == KNURL_OK);
    CHECK(knurl_json_serialize_materialized(&schema, &record, 16, &bytes,
                                            &error) == KNURL_LIMIT &&
          bytes.data == NULL);
    CHECK(knurl_json_serialize_materialized(&schema, &record, 0, &bytes,
                                            &error) == KNURL_OK &&
          bytes.length > 131072);
    memset(&sink, 0, sizeof(sink));
    sink.buffer = collected;
    CHECK(knurl_json_write_sink(&schema, &record, write_sink, &sink, &error) ==
              KNURL_OK &&
          sink.length == bytes.length && sink.calls > 32 &&
          memcmp(collected, bytes.data, sink.length) == 0);
    sink.length = sink.calls = 0;
    sink.fail = 1;
    CHECK(knurl_json_write_sink(&schema, &record, write_sink, &sink, &error) ==
              KNURL_IO &&
          sink.calls == 2 && error.native_code == 17 &&
          !strcmp(error.message, "sink stopped"));
    sink.calls = 0;
    sink.fail = 2;
    CHECK(knurl_json_write_sink(&schema, &record, write_sink, &sink, &error) ==
              KNURL_INVALID &&
          sink.calls == 1 && strstr(error.message, "synchronous sink") != NULL);
    knurl_json_source_config_init(&config);
    config.record.schema = schema;
    config.record.record = &record;
    config.release = closed;
    config.userdata = &lifetime;
    CHECK(knurl_source_json(&config, &source, &error) == KNURL_OK &&
          lifetime.closed == 0);
    config.record.record = NULL;
    length = 0;
    for (;;) {
        status =
            knurl_source_read(source, buffer, sizeof(buffer), &count, &error);
        if (status == KNURL_EOF)
            break;
        CHECK(status == KNURL_OK && count > 0 && count <= sizeof(buffer) &&
              length + count <= 131100);
        memcpy(collected + length, buffer, count);
        length += count;
    }
    CHECK(length == bytes.length && memcmp(collected, bytes.data, length) == 0);
    knurl_owned_bytes_cleanup(&bytes);
    CHECK(knurl_source_destroy(source, &error) == KNURL_OK &&
          lifetime.closed == 1 && lifetime.outcome == KNURL_OK);
    config.record.record = &record;
    CHECK(knurl_source_json(&config, &source, &error) == KNURL_OK);
    CHECK(knurl_source_destroy(source, &error) == KNURL_OK &&
          lifetime.closed == 2 && lifetime.outcome == KNURL_CANCELLED);
    CHECK(knurl_reply_new(&reply, &error) == KNURL_OK);
    CHECK(knurl_reply_json(reply, 99, &config, &error) == KNURL_INVALID &&
          lifetime.closed == 2);
    CHECK(knurl_reply_json(reply, 200, &config, &error) == KNURL_OK);
    CHECK(knurl_reply_status(reply, 204, &error) == KNURL_OK);
    CHECK(knurl_reply_destroy(reply, &error) == KNURL_OK &&
          lifetime.closed == 3 && lifetime.outcome == KNURL_CANCELLED);
    schema.record_size++;
    CHECK(knurl_json_serialize_materialized(&schema, &record, 0, &bytes,
                                            &error) == KNURL_INVALID);
    knurl_owned_bytes_cleanup(&bytes);
    free(collected);
    free(record.message);
    return 0;
}
