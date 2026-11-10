#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
static void *checked_malloc(size_t);
static void *checked_calloc(size_t, size_t);
static void *checked_realloc(void *, size_t);
static void checked_free(void *);
#define malloc checked_malloc
#define calloc checked_calloc
#define realloc checked_realloc
#define free checked_free
#include "../src/xml_mapping.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
#include "xml-stack-fixture.h"
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "XML stack %d: %s\n", __LINE__, #t);               \
            abort();                                                           \
        }                                                                      \
    } while (0)
static void *live[1024];
static size_t attempts, fail_at, outstanding, callocs, mallocs, reallocs;
static size_t sites[32], site_count, last_calloc;
static int tracking;
static int allocation(char kind)
{
    if (!tracking)
        return 1;
    ++attempts;
    if (kind == 'c') {
        ++callocs;
        last_calloc = attempts;
    } else {
        CHECK(site_count < sizeof(sites) / sizeof(sites[0]));
        sites[site_count++] = attempts;
        if (kind == 'm')
            ++mallocs;
        else
            ++reallocs;
    }
    return attempts != fail_at;
}
static void remember(void *pointer)
{
    size_t i;
    if (!tracking || pointer == NULL)
        return;
    for (i = 0; i < sizeof(live) / sizeof(live[0]); ++i)
        if (live[i] == NULL) {
            live[i] = pointer;
            ++outstanding;
            return;
        }
    CHECK(0);
}
static void forget(void *pointer)
{
    size_t i;
    if (!tracking || pointer == NULL)
        return;
    for (i = 0; i < sizeof(live) / sizeof(live[0]); ++i)
        if (live[i] == pointer) {
            live[i] = NULL;
            --outstanding;
            return;
        }
    CHECK(0);
}
static void *checked_malloc(size_t size)
{
    void *p = allocation('m') ? malloc(size) : NULL;
    remember(p);
    return p;
}
static void *checked_calloc(size_t count, size_t size)
{
    void *p = allocation('c') ? calloc(count, size) : NULL;
    remember(p);
    return p;
}
static void *checked_realloc(void *old, size_t size)
{
    void *p;
    if (!allocation('r'))
        return NULL;
    /* On success the old allocation is no longer a live object. */
    forget(old);
    p = realloc(old, size);
    if (p == NULL)
        remember(old);
    else
        remember(p);
    return p;
}
static void checked_free(void *p)
{
    forget(p);
    free(p);
}
static void begin(size_t failure)
{
    CHECK(outstanding == 0);
    attempts = callocs = mallocs = reallocs = site_count = last_calloc = 0;
    fail_at = failure;
    tracking = 1;
}
static void end(void)
{
    CHECK(outstanding == 0);
    tracking = 0;
}
struct array_record {
    lonejson_object_array children;
    char text[16];
};
static void nested_arrays(void)
{
    static const lonejson_field array = LONEJSON_FIELD_OBJECT_ARRAY(
        struct array_record, children, "n", struct array_record, NULL,
        LONEJSON_OVERFLOW_FAIL);
    static const lonejson_field text = LONEJSON_FIELD_STRING_FIXED(
        struct array_record, text, "text", LONEJSON_OVERFLOW_FAIL);
    lonejson_field fields[2];
    lonejson_map map;
    lonejson_schema_view schema;
    struct array_record record, *item;
    knurl_xml_config config;
    knurl_source *source;
    knurl_bytes bytes;
    knurl_error error;
    char document[512], *p = document;
    size_t i, growth_failure;
    memset(&map, 0, sizeof(map));
    fields[0] = array;
    fields[1] = text;
    fields[0].submap = &map;
    map.name = "recursive-array";
    map.struct_size = sizeof(record);
    map.fields = fields;
    map.field_count = 2;
    CHECK(knurl_json_schema_from_map(NULL, &map, &schema, &error) == KNURL_OK);
    lonejson_init(NULL, &map, &record);
    memcpy(p, "<r>", 3);
    p += 3;
    for (i = 0; i < 32; ++i) {
        memcpy(p, "<n>", 3);
        p += 3;
    }
    memcpy(p, "<text>payload</text>", 20);
    p += 20;
    for (i = 0; i < 32; ++i) {
        memcpy(p, "</n>", 4);
        p += 4;
    }
    strcpy(p, "<n><text>second</text></n></r>");
    bytes.data = document;
    bytes.length = strlen(document);
    knurl_xml_config_init(&config);
    config.max_depth = 34;
    CHECK(knurl_source_bytes(bytes, &source, &error) == KNURL_OK);
    begin(0);
    CHECK(knurl_xml_parse_source(source, &schema, &config, &record, &error) ==
          KNURL_OK);
    CHECK(site_count > 0);
    growth_failure = sites[0];
    end();
    CHECK(knurl_source_destroy(source, &error) == KNURL_OK);
    CHECK(record.children.count == 2);
    item = record.children.items;
    CHECK(!strcmp(item[1].text, "second"));
    for (i = 1; i < 32; ++i) {
        CHECK(item->children.count == 1);
        item = item->children.items;
    }
    CHECK(!strcmp(item->text, "payload"));
    CHECK(knurl_source_bytes(bytes, &source, &error) == KNURL_OK);
    begin(growth_failure);
    CHECK(knurl_xml_parse_source(source, &schema, &config, &record, &error) ==
              KNURL_NOMEM &&
          record.children.count == 0);
    end();
    CHECK(knurl_source_destroy(source, &error) == KNURL_OK);
    config.max_depth = 33;
    CHECK(knurl_source_bytes(bytes, &source, &error) == KNURL_OK);
    begin(0);
    CHECK(knurl_xml_parse_source(source, &schema, &config, &record, &error) ==
              KNURL_INVALID &&
          record.children.count == 0);
    end();
    CHECK(knurl_source_destroy(source, &error) == KNURL_OK);
    lonejson_cleanup(&map, &record);
}
static void inherited_space(void)
{
    struct xml_stack_fixture f;
    knurl_error error;
    char *p;
    size_t i;
    int reset;
    CHECK(xml_stack_fixture_new(&f, 32) == 0);
    free(f.input);
    f.input = malloc(512);
    CHECK(f.input != NULL);
    for (reset = 0; reset < 2; ++reset) {
        p = f.input;
        strcpy(p, "<r xml:space='preserve'>");
        p += strlen(p);
        for (i = 0; i < 32; ++i) {
            strcpy(p, reset && i == 16 ? "<n xml:space='default'>" : "<n>");
            p += strlen(p);
        }
        strcpy(p, "  <ignored/>  ");
        p += strlen(p);
        for (i = 0; i < 32; ++i) {
            memcpy(p, "</n>", 4);
            p += 4;
        }
        strcpy(p, "</r>");
        f.length = strlen(f.input);
        begin(0);
        CHECK(xml_stack_parse(&f, 34, &error) ==
              (reset ? KNURL_INVALID : KNURL_OK));
        end();
        CHECK(reset || !strcmp(f.record.text, "    "));
    }
    xml_stack_fixture_destroy(&f);
}
static void *worker(void *unused)
{
#if defined(KNURL_MUSL_STACK_CHECK) || defined(KNURL_DEBUG_TEST_THREADS)
    pthread_attr_t attributes;
    size_t stack_size;
#endif
    struct xml_stack_fixture f;
    knurl_error error;
    size_t failures[34], count, i;
    (void)unused;
#if defined(KNURL_MUSL_STACK_CHECK) || defined(KNURL_DEBUG_TEST_THREADS)
    CHECK(pthread_getattr_np(pthread_self(), &attributes) == 0);
    CHECK(pthread_attr_getstacksize(&attributes, &stack_size) == 0 &&
          stack_size > 0 && stack_size < 256u * 1024u);
#ifdef KNURL_DEBUG_TEST_THREADS
    CHECK(stack_size == 128u * 1024u);
#endif
    CHECK(pthread_attr_destroy(&attributes) == 0);
#endif
    nested_arrays();
    inherited_space();
    CHECK(xml_stack_fixture_new(&f, 3) == 0);
    begin(0);
    CHECK(xml_stack_parse(&f, 5, &error) == KNURL_OK);
    CHECK(!strcmp(f.record.text, "payload"));
    /* One existing field-state allocation per object, one text builder;
     * shallow traversal adds no allocation to the pre-refactor cost. */
    CHECK(callocs <= 4 && mallocs == 0 && reallocs <= 1);
    end();
    xml_stack_fixture_destroy(&f);
    CHECK(xml_stack_fixture_new(&f, 512) == 0);
    f.chunk = 7;
    begin(0);
    CHECK(xml_stack_parse(&f, 514, &error) == KNURL_OK);
    CHECK(!strcmp(f.record.text, "payload") && f.reads > 100);
    CHECK(callocs <= 513 && mallocs <= 1 && reallocs <= 8);
    count = site_count;
    memcpy(failures, sites, count * sizeof(sites[0]));
    failures[count++] = 1;
    failures[count++] = last_calloc;
    end();
    for (i = 0; i < count; ++i) {
        begin(failures[i]);
        CHECK(xml_stack_parse(&f, 514, &error) == KNURL_NOMEM);
        CHECK(f.record.text[0] == '\0');
        end();
        CHECK(xml_stack_parse(&f, 514, &error) == KNURL_OK &&
              !strcmp(f.record.text, "payload"));
    }
    begin(1);
    CHECK(xml_stack_parse(&f, 514, NULL) == KNURL_NOMEM);
    end();
    begin(0);
    CHECK(xml_stack_parse(&f, 513, &error) == KNURL_INVALID &&
          strstr(error.message, "max_depth") != NULL);
    end();
    f.fail_after = f.length / 2u;
    begin(0);
    CHECK(xml_stack_parse(&f, 514, &error) == KNURL_IO &&
          f.record.text[0] == '\0');
    end();
    f.fail_after = 0;
    f.input[f.length - 2u] = 'x';
    begin(0);
    CHECK(xml_stack_parse(&f, 514, &error) == KNURL_INVALID);
    end();
    f.input[f.length - 2u] = 'r';
    CHECK(xml_stack_parse(&f, 514, &error) == KNURL_OK);
    xml_stack_fixture_destroy(&f);
    return NULL;
}
int main(void)
{
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, worker, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    return 0;
}
