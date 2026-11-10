#include <knurl/knurl.h>
#include <knurl/lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "materialized output %d: %s\n", __LINE__, #t);     \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct record {
    char *text;
};
static const lonejson_field fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct record, text, "text")};
LONEJSON_MAP_DEFINE(map, struct record, fields);
int main(void)
{
    struct record record;
    lonejson_schema_view schema;
    lonejson_record_view view;
    knurl_xml_config xml;
    knurl_owned_bytes bytes;
    knurl_error error;
    size_t length = 10485761UL;
    memset(&view, 0, sizeof(view));
    record.text = malloc(length + 1);
    CHECK(record.text != NULL);
    memset(record.text, 'x', length);
    record.text[length] = 0;
    CHECK(knurl_json_schema_from_map(NULL, &map, &schema, &error) == KNURL_OK);
    view.size = sizeof(view);
    view.abi_version = LONEJSON_VIEW_ABI_VERSION;
    view.schema = schema;
    view.record = &record;
    knurl_owned_bytes_init(&bytes);
    CHECK(knurl_json_serialize_materialized(&schema, &record, 16, &bytes,
                                            &error) == KNURL_LIMIT &&
          !bytes.data);
    CHECK(knurl_json_serialize_materialized(&schema, &record, 0, &bytes,
                                            &error) == KNURL_OK &&
          bytes.length > length);
    CHECK(!memcmp(bytes.data, "{\"text\":\"", 9));
    knurl_owned_bytes_cleanup(&bytes);
    knurl_xml_config_init(&xml);
    xml.root_element = "record";
    CHECK(knurl_xml_serialize_materialized(&view, &xml, 16, &bytes, &error) ==
              KNURL_LIMIT &&
          !bytes.data);
    CHECK(knurl_xml_serialize_materialized(&view, &xml, 0, &bytes, &error) ==
              KNURL_OK &&
          bytes.length > length);
    CHECK(!memcmp(bytes.data, "<record>", 8) &&
          !memcmp((const char *)bytes.data + bytes.length - 9, "</record>", 9));
    knurl_owned_bytes_cleanup(&bytes);
    free(record.text);
    return 0;
}
