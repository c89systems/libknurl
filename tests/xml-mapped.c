#include <knurl/knurl.h>
#include <knurl/lonejson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "XML mapping %d: %s (%s)\n", __LINE__, #t,         \
                    error.message);                                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct line {
    char sku[32];
    lonejson_int64 quantity;
};
struct amount {
    char currency[8];
    double text;
};
struct invoice {
    char *id;
    struct amount amount;
    lonejson_object_array line;
    lonejson_string_array tag;
    int active;
};
struct blob {
    lonejson_spooled body;
};
struct mixed {
    char id[16];
    char text[32];
    lonejson_string_array item;
};
struct numbers {
    lonejson_i64_array i;
    lonejson_u64_array u;
    lonejson_f64_array d;
    lonejson_bool_array b;
};
static const lonejson_field line_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(struct line, sku, "sku",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(struct line, quantity, "quantity")};
static const lonejson_field amount_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(struct amount, currency, "currency",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_F64_REQ(struct amount, text, "text")};
LONEJSON_MAP_DEFINE(line_map, struct line, line_fields);
LONEJSON_MAP_DEFINE(amount_map, struct amount, amount_fields);
static const lonejson_field invoice_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct invoice, id, "id"),
    LONEJSON_FIELD_OBJECT_REQ(struct invoice, amount, "amount", &amount_map),
    LONEJSON_FIELD_OBJECT_ARRAY(struct invoice, line, "line", struct line,
                                &line_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ARRAY(struct invoice, tag, "tag",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_BOOL_REQ(struct invoice, active, "active")};
static const lonejson_field blob_fields[] = {
    LONEJSON_FIELD_STRING_STREAM_REQ(struct blob, body, "body")};
static const lonejson_field mixed_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(struct mixed, id, "@id",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(struct mixed, text, "text",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ARRAY(struct mixed, item, "item",
                                LONEJSON_OVERFLOW_FAIL)};
static const lonejson_field number_fields[] = {
    LONEJSON_FIELD_I64_ARRAY(struct numbers, i, "i", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64_ARRAY(struct numbers, u, "u", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_F64_ARRAY(struct numbers, d, "d", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_BOOL_ARRAY(struct numbers, b, "b", LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(invoice_map, struct invoice, invoice_fields);
LONEJSON_MAP_DEFINE(blob_map, struct blob, blob_fields);
LONEJSON_MAP_DEFINE(mixed_map, struct mixed, mixed_fields);
LONEJSON_MAP_DEFINE(number_map, struct numbers, number_fields);
struct input {
    const char *data;
    size_t size, offset, chunk;
    int again, fail;
};
static knurl_status read_input(void *bytes, size_t cap, size_t *n, void *data,
                               knurl_error *error)
{
    struct input *s = data;
    (void)error;
    *n = 0;
    if (s->offset == s->size)
        return s->again ? KNURL_AGAIN : s->fail ? KNURL_IO : KNURL_EOF;
    *n = s->size - s->offset;
    if (*n > cap)
        *n = cap;
    if (*n > s->chunk)
        *n = s->chunk;
    memcpy(bytes, s->data + s->offset, *n);
    s->offset += *n;
    return KNURL_OK;
}
static knurl_status parse(const char *text, const lonejson_schema_view *schema,
                          const knurl_xml_config *config, void *record,
                          int mode, knurl_error *error)
{
    struct input data;
    knurl_source_config input;
    knurl_source *source;
    knurl_status status;
    memset(&data, 0, sizeof(data));
    data.data = text;
    data.size = strlen(text);
    data.chunk = 1;
    data.again = mode == 1;
    data.fail = mode == 2;
    knurl_source_config_init(&input);
    input.read = read_input;
    input.userdata = &data;
    status = knurl_source_new(&input, &source, error);
    if (status != KNURL_OK)
        return status;
    status = source->xml_parse(source, schema, config, record, error);
    source->destroy(source, NULL);
    return status;
}
static void view(lonejson_record_view *r, const lonejson_schema_view *schema,
                 void *record)
{
    memset(r, 0, sizeof(*r));
    r->size = sizeof(*r);
    r->abi_version = LONEJSON_VIEW_ABI_VERSION;
    r->schema = *schema;
    r->record = record;
}
static int contains(const knurl_owned_bytes *b, const char *text)
{
    size_t i, n = strlen(text);
    if (n > b->length)
        return 0;
    for (i = 0; i <= b->length - n; ++i)
        if (memcmp((const char *)b->data + i, text, n) == 0)
            return 1;
    return 0;
}
static int mapped(void)
{
    static const char xml[] =
        "<x:invoice xmlns:x='urn:invoice' id=' inv-001 '><amount "
        "currency='SEK'>123.5</amount><line><sku>A-1</"
        "sku><quantity>-9223372036854775808</quantity></line><line><sku>B-2</"
        "sku><quantity>3</quantity></line><tag>finance</tag><tag>xml</"
        "tag><active>TRUE</active></x:invoice>";
    knurl_xml_config c;
    lonejson_schema_view schema;
    lonejson_record_view r;
    struct invoice doc, copy;
    struct mixed mixed;
    struct numbers numbers;
    knurl_owned_bytes bytes;
    knurl_error error;
    knurl_xml_config_init(&c);
    CHECK(c.max_depth == 64 && c.max_text_bytes == 67108864u);
    c.root_element = "invoice";
    CHECK(knurl_json_schema_from_map(NULL, &invoice_map, &schema, &error) ==
          KNURL_OK);
    if (lonejson_init(NULL, &invoice_map, &doc, NULL) != LONEJSON_STATUS_OK)
        abort();
    if (lonejson_init(NULL, &invoice_map, &copy, NULL) != LONEJSON_STATUS_OK)
        abort();
    memset(&error, 0xa5, sizeof(error));
    CHECK(parse(xml, &schema, &c, &doc, 0, &error) == KNURL_OK &&
          error.status == KNURL_OK);
    CHECK(strcmp(doc.id, " inv-001 ") == 0 && doc.line.count == 2 &&
          doc.tag.count == 2 && doc.active == 1);
    CHECK(strcmp(((struct line *)doc.line.items)[0].sku, "A-1") == 0);
    CHECK(((struct line *)doc.line.items)[0].quantity <
          -(lonejson_int64)((~(lonejson_uint64)0u) >> 1u));
    view(&r, &schema, &doc);
    knurl_owned_bytes_init(&bytes);
    CHECK(knurl_xml_serialize_materialized(&r, &c, 0, &bytes, &error) ==
          KNURL_OK);
    CHECK(contains(&bytes, "<quantity>-9223372036854775808</quantity>"));
    {
        char *s = malloc(bytes.length + 1);
        CHECK(s != NULL);
        memcpy(s, bytes.data, bytes.length);
        s[bytes.length] = 0;
        CHECK(parse(s, &schema, &c, &copy, 0, &error) == KNURL_OK);
        free(s);
    }
    CHECK(copy.line.count == 2 && strcmp(copy.id, doc.id) == 0);
    knurl_owned_bytes_cleanup(&bytes);
    CHECK(parse(xml, &schema, &c, &doc, 1, &error) == KNURL_STATE &&
          doc.id == NULL && doc.line.count == 0);
    CHECK(parse(xml, &schema, &c, &doc, 2, &error) == KNURL_IO &&
          doc.id == NULL);
    c.max_depth = 2;
    CHECK(parse(xml, &schema, &c, &doc, 0, &error) == KNURL_INVALID);
    if (lonejson_cleanup(&invoice_map, &copy, NULL) != LONEJSON_STATUS_OK)
        abort();
    if (lonejson_cleanup(&invoice_map, &doc, NULL) != LONEJSON_STATUS_OK)
        abort();
    knurl_xml_config_init(&c);
    c.root_element = "item";
    c.attribute_prefix = "@";
    CHECK(knurl_json_schema_from_map(NULL, &mixed_map, &schema, &error) ==
          KNURL_OK);
    if (lonejson_init(NULL, &mixed_map, &mixed, NULL) != LONEJSON_STATUS_OK)
        abort();
    CHECK(parse("<item id='split'>\n <text>hello</text>\n</item>", &schema, &c,
                &mixed, 0, &error) == KNURL_OK &&
          strcmp(mixed.text, "hello") == 0);
    CHECK(parse("<item id='split'><text>hello</text><![CDATA[ ]]></item>",
                &schema, &c, &mixed, 0, &error) == KNURL_INVALID);
    CHECK(parse("<item id='a'><item>a</item>\n <item>b</item></item>", &schema,
                &c, &mixed, 0, &error) == KNURL_OK &&
          mixed.item.count == 2);
    CHECK(
        parse("<item id='a'><item>a</item><text>x</text><item>b</item></item>",
              &schema, &c, &mixed, 0, &error) == KNURL_INVALID &&
        mixed.item.count == 0);
    c.trim_text = 1;
    CHECK(parse("<item id='  a '>  hello  </item>", &schema, &c, &mixed, 0,
                &error) == KNURL_OK &&
          strcmp(mixed.text, "hello") == 0 && strcmp(mixed.id, "a") == 0);
    c.skip_unknown_disabled = 1;
    CHECK(parse("<item id='a'><unknown/></item>", &schema, &c, &mixed, 0,
                &error) == KNURL_INVALID);
    c.skip_unknown_disabled = 0;
    c.max_depth = 2;
    CHECK(parse("<item id='a'><unknown><deep/></unknown></item>", &schema, &c,
                &mixed, 0, &error) == KNURL_INVALID);
    c.max_depth = 64;
    c.max_text_bytes = 4;
    CHECK(parse("<item id='a'>     </item>", &schema, &c, &mixed, 0, &error) ==
          KNURL_INVALID);
    c.max_text_bytes = 0;
    CHECK(parse("<!DOCTYPE item [<!ENTITY x 'entity'>]><item id='a'>&x;</item>",
                &schema, &c, &mixed, 0, &error) == KNURL_INVALID);
    CHECK(parse("<!DOCTYPE item SYSTEM 'file:///no-such-knurl-file'><item "
                "id='a'/>",
                &schema, &c, &mixed, 0, &error) == KNURL_INVALID);
    CHECK(parse("<item id='a'>allocated</item><item/>", &schema, &c, &mixed, 0,
                &error) == KNURL_INVALID &&
          mixed.item.count == 0);
    if (lonejson_cleanup(&mixed_map, &mixed, NULL) != LONEJSON_STATUS_OK)
        abort();
    knurl_xml_config_init(&c);
    c.root_element = "numbers";
    CHECK(knurl_json_schema_from_map(NULL, &number_map, &schema, &error) ==
          KNURL_OK);
    if (lonejson_init(NULL, &number_map, &numbers, NULL) != LONEJSON_STATUS_OK)
        abort();
    CHECK(parse("<numbers><i>-1</i><i>2</i><u>18446744073709551615</u><d>1.25</"
                "d><b>true</b><b>0</b></numbers>",
                &schema, &c, &numbers, 0, &error) == KNURL_OK);
    CHECK(numbers.i.count == 2 && numbers.i.items[0] == -1 &&
          numbers.u.items[0] == ~(lonejson_uint64)0 &&
          numbers.d.items[0] == 1.25 && numbers.b.count == 2 &&
          numbers.b.items[0] && !numbers.b.items[1]);
    view(&r, &schema, &numbers);
    CHECK(knurl_xml_serialize_materialized(&r, &c, 0, &bytes, &error) ==
              KNURL_OK &&
          contains(&bytes, "<b>true</b><b>false</b>") &&
          contains(&bytes, "<u>18446744073709551615</u>"));
    knurl_owned_bytes_cleanup(&bytes);
    CHECK(parse("<numbers><i>9223372036854775808</i></numbers>", &schema, &c,
                &numbers, 0, &error) == KNURL_INVALID &&
          numbers.i.count == 0);
    CHECK(parse("<numbers><d>NaN</d></numbers>", &schema, &c, &numbers, 0,
                &error) == KNURL_INVALID);
    if (lonejson_cleanup(&number_map, &numbers, NULL) != LONEJSON_STATUS_OK)
        abort();
    return 0;
}
struct lifetime {
    int count;
    knurl_status outcome;
    knurl_xml_generator *generator;
    int reenter;
};
static void release(void *data, knurl_status outcome)
{
    struct lifetime *l = data;
    l->count++;
    l->outcome = outcome;
    if (l->reenter && l->generator->destroy(l->generator, NULL) != KNURL_STATE)
        l->count = 99;
}
static int generation(void)
{
    struct blob blob;
    lonejson_schema_view schema;
    lonejson_record_view r;
    knurl_xml_source_config c;
    knurl_xml_generator *g;
    knurl_source *source;
    knurl_reply *reply;
    knurl_owned_bytes bytes;
    knurl_error error;
    lonejson_error native;
    lonejson_config policy;
    lonejson *runtime;
    struct lifetime l;
    char chunk[2051], out[1];
    char *collected;
    size_t n, total, i;
    knurl_status status;
    memset(&l, 0, sizeof(l));
    policy = lonejson_default_config();
    policy.spool_default.memory_limit = 8;
    runtime = lonejson_new(&policy, &native);
    CHECK(runtime != NULL);
    if (lonejson_init(runtime, &blob_map, &blob, NULL) != LONEJSON_STATUS_OK)
        abort();
    memset(chunk, 'a', sizeof(chunk));
    chunk[2047] = (char)0xe2;
    chunk[2048] = (char)0x82;
    chunk[2049] = (char)0xac;
    chunk[2050] = '&';
    for (i = 0; i < 40; ++i)
        CHECK(lonejson_spooled_append(&blob.body, chunk, sizeof(chunk),
                                      &native) == LONEJSON_STATUS_OK);
    CHECK(lonejson_spooled_spilled(&blob.body));
    CHECK(knurl_json_schema_from_map(runtime, &blob_map, &schema, &error) ==
          KNURL_OK);
    view(&r, &schema, &blob);
    knurl_xml_source_config_init(&c);
    c.record = &r;
    c.xml.root_element = "blob";
    c.release = release;
    c.userdata = &l;
    CHECK(knurl_xml_generator_new(&c, &g, &error) == KNURL_OK && l.count == 0);
    l.generator = g;
    collected = malloc(90000);
    CHECK(collected != NULL);
    total = 0;
    for (;;) {
        status = g->read(g, out, sizeof(out), &n, &error);
        if (status == KNURL_EOF)
            break;
        CHECK(status == KNURL_OK && n == 1 && total < 90000);
        collected[total++] = out[0];
    }
    CHECK(total == 82226 && memcmp(collected, "<blob><body>", 12) == 0 &&
          memcmp(collected + total - 14, "</body></blob>", 14) == 0);
    CHECK(g->read(g, out, sizeof(out), &n, &error) == KNURL_EOF && n == 0);
    l.reenter = 1;
    CHECK(g->destroy(g, &error) == KNURL_OK && l.count == 1 &&
          l.outcome == KNURL_OK);
    l.reenter = 0;
    CHECK(knurl_xml_generator_new(&c, &g, &error) == KNURL_OK);
    CHECK(g->read(g, out, sizeof(out), &n, &error) == KNURL_OK);
    CHECK(g->destroy(g, &error) == KNURL_OK && l.count == 2 &&
          l.outcome == KNURL_CANCELLED);
    CHECK(knurl_source_xml(&c, &source, &error) == KNURL_OK);
    CHECK(source->destroy(source, &error) == KNURL_OK && l.count == 3 &&
          l.outcome == KNURL_CANCELLED);
    CHECK(knurl_reply_new(&reply, &error) == KNURL_OK);
    CHECK(reply->xml(reply, 200, &c, &error) == KNURL_OK);
    CHECK(reply->text(reply, 201, "text/plain", "replace", &error) ==
              KNURL_OK &&
          l.count == 4 && l.outcome == KNURL_CANCELLED);
    CHECK(reply->xml(reply, 99, &c, &error) == KNURL_INVALID && l.count == 4);
    CHECK(reply->destroy(reply, &error) == KNURL_OK);
    knurl_owned_bytes_init(&bytes);
    CHECK(knurl_xml_serialize_materialized(&r, &c.xml, 32, &bytes, &error) ==
              KNURL_LIMIT &&
          bytes.data == NULL);
    CHECK(knurl_xml_serialize_materialized(&r, &c.xml, 0, &bytes, &error) ==
              KNURL_OK &&
          bytes.length == total && memcmp(bytes.data, collected, total) == 0);
    knurl_owned_bytes_cleanup(&bytes);
    /* Invalid UTF-8 at the end is discovered on a later read, after prefix
     * output. */
    CHECK(lonejson_spooled_append(&blob.body, "\xff", 1, &native) ==
          LONEJSON_STATUS_OK);
    CHECK(knurl_xml_generator_new(&c, &g, &error) == KNURL_OK);
    CHECK(g->read(g, out, 1, &n, &error) == KNURL_OK && out[0] == '<');
    do {
        char buffer[4096];
        status = g->read(g, buffer, sizeof(buffer), &n, &error);
    } while (status == KNURL_OK);
    CHECK(status == KNURL_INVALID);
    CHECK(g->read(g, out, 1, &n, &error) == KNURL_INVALID && n == 0);
    CHECK(g->destroy(g, &error) == KNURL_OK && l.outcome == KNURL_INVALID);
    free(collected);
    if (lonejson_cleanup(&blob_map, &blob, NULL) != LONEJSON_STATUS_OK)
        abort();
    lonejson_free(runtime);
    return 0;
}
static int limits_and_ownership(void)
{
    struct blob blob;
    struct numbers numbers;
    struct mixed mixed;
    lonejson_schema_view schema;
    lonejson_record_view r;
    lonejson_int64 backing[1];
    knurl_xml_config config;
    knurl_xml_source_config output;
    knurl_xml_generator *g;
    knurl_source *source;
    knurl_bytes input;
    knurl_error error;
    knurl_owned_bytes materialized;
    size_t length = 12u * 1024u * 1024u, n, i;
    char *large, byte;
    static const unsigned char invalid[][4] = {{0xe0, 0x80, 0xa0, 0},
                                               {0xed, 0xa0, 0x80, 0},
                                               {0xef, 0xbf, 0xbe, 0},
                                               {1, 0, 0, 0}};
    knurl_xml_config_init(&config);
    config.root_element = "doc";
    CHECK(knurl_json_schema_from_map(NULL, &blob_map, &schema, &error) ==
          KNURL_OK);
    if (lonejson_init(NULL, &blob_map, &blob, NULL) != LONEJSON_STATUS_OK)
        abort();
    large = malloc(length + 25);
    CHECK(large != NULL);
    memcpy(large, "<doc><body>", 11);
    memset(large + 11, 'x', length);
    memcpy(large + 11 + length, "</body></doc>", 14);
    input.data = large;
    input.length = length + 24;
    CHECK(knurl_source_bytes(input, &source, &error) == KNURL_OK);
    CHECK(source->xml_parse(source, &schema, &config, &blob, &error) ==
              KNURL_OK &&
          lonejson_spooled_size(&blob.body) == length);
    CHECK(source->destroy(source, &error) == KNURL_OK);
    config.trim_text = 1;
    CHECK(parse("<doc><body>abc</body></doc>", &schema, &config, &blob, 0,
                &error) == KNURL_INVALID);
    config.trim_text = 0;
    config.max_text_bytes = 2;
    CHECK(parse("<doc><body>abc</body></doc>", &schema, &config, &blob, 0,
                &error) == KNURL_INVALID &&
          lonejson_spooled_size(&blob.body) == 0);
    if (lonejson_cleanup(&blob_map, &blob, NULL) != LONEJSON_STATUS_OK)
        abort();
    free(large);
    CHECK(knurl_json_schema_from_map(NULL, &number_map, &schema, &error) ==
          KNURL_OK);
    if (lonejson_init(NULL, &number_map, &numbers, NULL) != LONEJSON_STATUS_OK)
        abort();
    numbers.i.items = backing;
    numbers.i.capacity = 1;
    numbers.i.flags = LONEJSON_ARRAY_FIXED_CAPACITY;
    knurl_xml_config_init(&config);
    config.root_element = "numbers";
    CHECK(parse("<numbers><i>1</i><i>2</i></numbers>", &schema, &config,
                &numbers, 0, &error) == KNURL_INVALID &&
          numbers.i.count == 0 && numbers.i.items == backing);
    if (lonejson_cleanup(&number_map, &numbers, NULL) != LONEJSON_STATUS_OK)
        abort();
    CHECK(knurl_json_schema_from_map(NULL, &mixed_map, &schema, &error) ==
          KNURL_OK);
    if (lonejson_init(NULL, &mixed_map, &mixed, NULL) != LONEJSON_STATUS_OK)
        abort();
    strcpy(mixed.id, "a");
    strcpy(mixed.text, "hello");
    view(&r, &schema, &mixed);
    knurl_xml_source_config_init(&output);
    output.record = &r;
    output.xml.root_element = "item";
    output.xml.attribute_prefix = "@";
    CHECK(knurl_xml_generator_new(&output, &g, &error) == KNURL_OK);
    {
        pid_t child = fork();
        int result;
        CHECK(child >= 0);
        if (child == 0)
            _exit(g->read(g, &byte, 1, &n, &error) == KNURL_STATE &&
                          g->destroy(g, &error) == KNURL_STATE
                      ? 0
                      : 1);
        CHECK(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
              WEXITSTATUS(result) == 0);
    }
    CHECK(g->destroy(g, &error) == KNURL_OK);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memcpy(mixed.text, invalid[i], 4);
        knurl_owned_bytes_init(&materialized);
        CHECK(knurl_xml_serialize_materialized(
                  &r, &output.xml, 0, &materialized, &error) == KNURL_INVALID &&
              materialized.data == NULL);
    }
    if (lonejson_cleanup(&mixed_map, &mixed, NULL) != LONEJSON_STATUS_OK)
        abort();
    return 0;
}
static void *run(void *data)
{
    int *result = data;
    *result = mapped();
    if (*result == 0)
        *result = limits_and_ownership();
    if (*result == 0)
        *result = generation();
    return NULL;
}
int main(void)
{
    pthread_t thread;
    int result = 1;
    knurl_error error;
    memset(&error, 0, sizeof(error));
    CHECK(pthread_create(&thread, NULL, run, &result) == 0 &&
          pthread_join(thread, NULL) == 0);
    return result;
}
