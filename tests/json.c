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
struct items {
    rotmos_json_array *parser;
    size_t count;
    int reenter;
};
static rotmos_status item(const lonejson_record_view *view, void *data,
                          rotmos_error *error)
{
    struct items *items;
    struct record *record;
    items = data;
    record = view->record;
    if (record == NULL || record->message == NULL || view->schema.map != &map)
        return ROTMOS_INVALID;
    ++items->count;
    if (items->reenter &&
        rotmos_json_array_destroy(items->parser, error) != ROTMOS_STATE)
        return ROTMOS_INVALID;
    return ROTMOS_OK;
}
static int selected_items(void)
{
    static const char first[] = "{\"items\":[{\"message\":\"first\"},";
    static const char last[] = "{\"message\":\"second\"}]}";
    rotmos_json_array_config config;
    struct items items;
    rotmos_error error;
    rotmos_json_array_config_init(&config);
    CHECK(rotmos_json_schema_from_map(NULL, &map, &config.schema, &error) ==
          ROTMOS_OK);
    memset(&items, 0, sizeof(items));
    config.selector = "items";
    config.item = item;
    config.userdata = &items;
    CHECK(rotmos_json_array_new(&config, &items.parser, &error) == ROTMOS_OK);
    CHECK(rotmos_json_array_feed(items.parser, first, sizeof(first) - 1,
                                 &error) == ROTMOS_OK &&
          items.count == 1);
    CHECK(rotmos_json_array_feed(items.parser, last, sizeof(last) - 1,
                                 &error) == ROTMOS_OK &&
          items.count == 2);
    CHECK(rotmos_json_array_finish(items.parser, &error) == ROTMOS_OK);
    CHECK(rotmos_json_array_finish(items.parser, &error) == ROTMOS_OK);
    CHECK(rotmos_json_array_feed(items.parser, "", 0, &error) == ROTMOS_STATE);
    CHECK(rotmos_json_array_destroy(items.parser, &error) == ROTMOS_OK);
    items.count = 0;
    items.reenter = 1;
    CHECK(rotmos_json_array_new(&config, &items.parser, &error) == ROTMOS_OK);
    CHECK(rotmos_json_array_feed(items.parser, first, sizeof(first) - 1,
                                 &error) == ROTMOS_STATE &&
          items.count == 1);
    CHECK(rotmos_json_array_finish(items.parser, &error) == ROTMOS_STATE);
    CHECK(rotmos_json_array_destroy(items.parser, &error) == ROTMOS_OK);
    items.count = 0;
    items.reenter = 0;
    CHECK(rotmos_json_array_new(&config, &items.parser, &error) == ROTMOS_OK);
    CHECK(rotmos_json_array_feed(items.parser, first, sizeof(first) - 1,
                                 &error) == ROTMOS_OK);
    CHECK(rotmos_json_array_finish(items.parser, &error) == ROTMOS_INVALID);
    CHECK(rotmos_json_array_destroy(items.parser, &error) == ROTMOS_OK);
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
    CHECK(rotmos_reply_json(reply, 101, &config, &error) == ROTMOS_INVALID &&
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
