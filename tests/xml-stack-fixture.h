#ifndef KNURL_TEST_XML_STACK_FIXTURE_H
#define KNURL_TEST_XML_STACK_FIXTURE_H
#include <knurl/xml.h>
#include <knurl/lonejson.h>
#include <stdlib.h>
#include <string.h>
struct xml_stack_record {
    char text[32];
};
struct xml_stack_fixture {
    lonejson_map *maps;
    lonejson_field *fields;
    lonejson *runtime;
    lonejson_schema_view schema;
    struct xml_stack_record record;
    char *input;
    size_t depth, length, offset, chunk, reads, fail_after;
};
static knurl_status xml_stack_read(void *bytes, size_t capacity, size_t *count,
                                   void *data, knurl_error *error)
{
    struct xml_stack_fixture *f = data;
    (void)error;
    ++f->reads;
    *count = 0;
    if (f->fail_after && f->offset >= f->fail_after)
        return KNURL_IO;
    if (f->offset == f->length)
        return KNURL_EOF;
    *count = f->length - f->offset;
    if (*count > capacity)
        *count = capacity;
    if (*count > f->chunk)
        *count = f->chunk;
    memcpy(bytes, f->input + f->offset, *count);
    f->offset += *count;
    return KNURL_OK;
}
static int xml_stack_fixture_new(struct xml_stack_fixture *f, size_t depth)
{
    static const lonejson_field object =
        LONEJSON_FIELD_OBJECT_REQ(struct xml_stack_record, text, "n", NULL);
    static const lonejson_field text = LONEJSON_FIELD_STRING_FIXED_REQ(
        struct xml_stack_record, text, "text", LONEJSON_OVERFLOW_FAIL);
    lonejson_config config;
    lonejson_error native;
    knurl_error error;
    char *p;
    size_t i;
    memset(f, 0, sizeof(*f));
    f->depth = depth;
    f->chunk = 127;
    f->maps = calloc(depth + 1u, sizeof(*f->maps));
    f->fields = calloc(depth + 1u, sizeof(*f->fields));
    f->input = malloc(depth * 7u + 64u);
    if (!f->maps || !f->fields || !f->input)
        return 1;
    for (i = 0; i <= depth; ++i) {
        f->maps[i].name = "stack";
        f->maps[i].struct_size = sizeof(f->record);
        f->maps[i].fields = &f->fields[i];
        f->maps[i].field_count = 1;
        f->fields[i] = i == depth ? text : object;
        if (i < depth)
            f->fields[i].submap = &f->maps[i + 1u];
    }
    config = lonejson_default_config();
    config.max_depth = depth + 4u;
    f->runtime = lonejson_new(&config, &native);
    if (!f->runtime || knurl_json_schema_from_map(
                           f->runtime, f->maps, &f->schema, &error) != KNURL_OK)
        return 1;
    if (lonejson_init(f->runtime, f->maps, &f->record, NULL) !=
        LONEJSON_STATUS_OK)
        abort();
    p = f->input;
    memcpy(p, "<r>", 3);
    p += 3;
    for (i = 0; i < depth; ++i) {
        memcpy(p, "<n>", 3);
        p += 3;
    }
    memcpy(p, "<text>payload</text>", 20);
    p += 20;
    for (i = 0; i < depth; ++i) {
        memcpy(p, "</n>", 4);
        p += 4;
    }
    memcpy(p, "</r>", 5);
    f->length = (size_t)(p - f->input) + 4u;
    return 0;
}
static knurl_status xml_stack_parse(struct xml_stack_fixture *f, size_t limit,
                                    knurl_error *error)
{
    knurl_source_config input;
    knurl_xml_config config;
    knurl_source *source;
    knurl_status status;
    f->offset = f->reads = 0;
    knurl_source_config_init(&input);
    input.read = xml_stack_read;
    input.userdata = f;
    status = knurl_source_new(&input, &source, error);
    if (status != KNURL_OK)
        return status;
    knurl_xml_config_init(&config);
    config.max_depth = limit;
    status =
        knurl_xml_parse_source(source, &f->schema, &config, &f->record, error);
    knurl_source_destroy(source, NULL);
    return status;
}
static void xml_stack_fixture_destroy(struct xml_stack_fixture *f)
{
    if (lonejson_cleanup(f->maps, &f->record, NULL) != LONEJSON_STATUS_OK)
        abort();
    lonejson_free(f->runtime);
    free(f->input);
    free(f->fields);
    free(f->maps);
}
#endif
