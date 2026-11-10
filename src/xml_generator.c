#include "xml_mapping_internal.h"
#include "internal.h"
#include "reply_internal.h"
#include <libxml/xmlwriter.h>
#include <libxml/chvalid.h>
#include <libxml/xmlstring.h>
#include <float.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Each writer operation emits one name (<=4096 bytes), one bounded UTF-8
 * text unit (<=2048 bytes), or punctuation. Flush before accepting another
 * operation. libxml2 owns escaping and XML writer state, never the record. */
#define XML_UNIT 2048u
#define XML_PENDING 32768u
#define XML_NAME 4096u
struct xml_frame {
    struct xml_frame *parent;
    const lonejson_map *map;
    const void *value;
    const char *name;
    lonejson_field field;
    size_t index, depth;
    int kind, stage, boolean;
};
enum { FRAME_OBJECT, FRAME_ARRAY, FRAME_ELEMENT, FRAME_ATTRIBUTE, FRAME_TEXT };
struct knurl_xml_generator {
    knurl_xml_config config;
    lonejson_record_view record;
    xmlTextWriterPtr writer;
    struct xml_frame *frame;
    unsigned char pending[XML_PENDING];
    size_t offset, length;
    const unsigned char *text;
    size_t remaining;
    lonejson_spooled *spool;
    unsigned char unit[XML_UNIT + 5];
    size_t carry;
    char number[80];
    knurl_error failure;
    knurl_source_close_fn release;
    void *userdata;
    pid_t process;
    pthread_t thread;
    int busy, eof;
};
static knurl_status fail(knurl_error *error, knurl_status status,
                         const char *message)
{
    knurl_fail(error, status, "xml_generator", message);
    if (error != NULL)
        error->source = KNURL_ERROR_XML;
    return status;
}
static knurl_status owner(knurl_xml_generator *g, knurl_error *error)
{
    if (g == NULL)
        return fail(error, KNURL_INVALID, "missing XML generator");
    if (g->process != getpid() || !pthread_equal(g->thread, pthread_self()))
        return fail(error, KNURL_STATE,
                    "XML generator belongs to another process/thread");
    if (g->busy) {
        fail(&g->failure, KNURL_STATE, "recursive XML generator operation");
        if (error != NULL)
            *error = g->failure;
        return KNURL_STATE;
    }
    return KNURL_OK;
}
static int output(void *data, const char *bytes, int length)
{
    knurl_xml_generator *g = data;
    if (length < 0 || (size_t)length > XML_PENDING - g->length) {
        fail(&g->failure, KNURL_LIMIT,
             "XML writer exceeded bounded output unit");
        return -1;
    }
    if (length != 0)
        memcpy(g->pending + g->length, bytes, (size_t)length);
    g->length += (size_t)length;
    return length;
}
static knurl_status name_check(const char *name, knurl_error *error)
{
    size_t n;
    if (name == NULL || name[0] == 0)
        return fail(error, KNURL_INVALID,
                    "XML output requires an element/attribute name");
    for (n = 0; n <= XML_NAME && name[n] != 0; ++n) {
    }
    if (n > XML_NAME)
        return fail(error, KNURL_LIMIT, "XML output name exceeds 4096 bytes");
    if (xmlValidateName((const xmlChar *)name, 0) != 0)
        return fail(error, KNURL_INVALID, "invalid XML output name");
    return KNURL_OK;
}
static int array_kind(const lonejson_field *f)
{
    return f->kind == LONEJSON_FIELD_KIND_STRING_ARRAY ||
           f->kind == LONEJSON_FIELD_KIND_I64_ARRAY ||
           f->kind == LONEJSON_FIELD_KIND_U64_ARRAY ||
           f->kind == LONEJSON_FIELD_KIND_F64_ARRAY ||
           f->kind == LONEJSON_FIELD_KIND_BOOL_ARRAY ||
           f->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY;
}
static int attribute(const lonejson_field *f, const knurl_xml_config *c,
                     const char **name)
{
    size_t n = strlen(c->attribute_prefix);
    if (n == 0 || f->json_key == NULL ||
        strncmp(f->json_key, c->attribute_prefix, n) != 0 ||
        f->json_key[n] == 0)
        return 0;
    if (name != NULL)
        *name = f->json_key + n;
    return 1;
}
static int emit(const lonejson_field *f, const void *record)
{
    const char *base = (const char *)record + f->struct_offset;
    if ((f->flags & LONEJSON_FIELD_HAS_PRESENCE) &&
        !*(const int *)((const char *)record + f->presence_offset))
        return 0;
    if (f->kind == LONEJSON_FIELD_KIND_STRING &&
        f->storage == LONEJSON_STORAGE_DYNAMIC)
        return *(char *const *)base != NULL ||
               (f->flags & LONEJSON_FIELD_REQUIRED);
    if (f->kind == LONEJSON_FIELD_KIND_STRING_STREAM ||
        f->kind == LONEJSON_FIELD_KIND_BASE64_STREAM)
        return lonejson_spooled_size((const lonejson_spooled *)base) != 0 ||
               (f->flags & LONEJSON_FIELD_REQUIRED);
    if (array_kind(f)) {
        switch (f->kind) {
        case LONEJSON_FIELD_KIND_STRING_ARRAY:
            return ((const lonejson_string_array *)base)->count != 0;
        case LONEJSON_FIELD_KIND_I64_ARRAY:
            return ((const lonejson_i64_array *)base)->count != 0;
        case LONEJSON_FIELD_KIND_U64_ARRAY:
            return ((const lonejson_u64_array *)base)->count != 0;
        case LONEJSON_FIELD_KIND_F64_ARRAY:
            return ((const lonejson_f64_array *)base)->count != 0;
        case LONEJSON_FIELD_KIND_BOOL_ARRAY:
            return ((const lonejson_bool_array *)base)->count != 0;
        default:
            return ((const lonejson_object_array *)base)->count != 0;
        }
    }
    return 1;
}
static knurl_status push(knurl_xml_generator *g, int kind, const char *name,
                         const lonejson_map *map, const void *value,
                         const lonejson_field *field, size_t depth,
                         knurl_error *error)
{
    struct xml_frame *f;
    knurl_status status;
    if (depth > g->config.max_depth)
        return fail(error, KNURL_INVALID,
                    "XML output nesting exceeds max_depth");
    if (kind != FRAME_TEXT && kind != FRAME_ARRAY) {
        status = name_check(name, error);
        if (status != KNURL_OK)
            return status;
    }
    if (value == NULL || (kind == FRAME_OBJECT && map == NULL))
        return fail(error, KNURL_INVALID, "missing XML mapped value/submap");
    f = calloc(1, sizeof(*f));
    if (f == NULL)
        return fail(error, KNURL_NOMEM, "allocating XML traversal frame");
    f->parent = g->frame;
    f->kind = kind;
    f->name = name;
    f->map = map;
    f->value = value;
    f->depth = depth;
    if (field != NULL)
        f->field = *field;
    g->frame = f;
    return KNURL_OK;
}
static void pop(knurl_xml_generator *g)
{
    struct xml_frame *f = g->frame;
    g->frame = f->parent;
    free(f);
}
static void integer_text(char *out, lonejson_uint64 value, int negative)
{
    char reverse[24];
    size_t n = 0, i = 0;
    do {
        reverse[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    if (negative)
        out[i++] = '-';
    while (n != 0)
        out[i++] = reverse[--n];
    out[i] = 0;
}
static knurl_status scalar_init(knurl_xml_generator *g,
                                const struct xml_frame *f, knurl_error *error)
{
    const void *base = (const char *)f->value + f->field.struct_offset;
    lonejson_int64 signed_value;
    lonejson_uint64 magnitude;
    lonejson_error native;
    lonejson_status code;
    double number;
    int n;
    g->text = NULL;
    g->spool = NULL;
    g->remaining = 0;
    g->carry = 0;
    switch (f->field.kind) {
    case LONEJSON_FIELD_KIND_STRING:
        g->text = f->field.storage == LONEJSON_STORAGE_DYNAMIC
                      ? *(const unsigned char *const *)base
                      : base;
        if (g->text == NULL)
            g->text = (const unsigned char *)"";
        if (f->field.storage == LONEJSON_STORAGE_FIXED) {
            while (g->remaining < f->field.fixed_capacity &&
                   g->text[g->remaining] != 0)
                ++g->remaining;
            if (g->remaining == f->field.fixed_capacity)
                return fail(error, KNURL_INVALID,
                            "XML fixed string lacks a terminator");
        } else
            g->remaining = strlen((const char *)g->text);
        break;
    case LONEJSON_FIELD_KIND_STRING_STREAM:
        if (f->kind == FRAME_ATTRIBUTE)
            return fail(error, KNURL_INVALID,
                        "XML attributes must be scalar fields");
        g->spool = (lonejson_spooled *)base;
        g->remaining = lonejson_spooled_size(g->spool);
        code = lonejson_spooled_rewind(g->spool, &native);
        if (code != LONEJSON_STATUS_OK)
            return knurl_json_error(code, &native, error);
        break;
    case LONEJSON_FIELD_KIND_I64:
        signed_value = *(const lonejson_int64 *)base;
        magnitude = signed_value < 0
                        ? (lonejson_uint64)(-(signed_value + 1)) + 1u
                        : (lonejson_uint64)signed_value;
        integer_text(g->number, magnitude, signed_value < 0);
        break;
    case LONEJSON_FIELD_KIND_U64:
        integer_text(g->number, *(const lonejson_uint64 *)base, 0);
        break;
    case LONEJSON_FIELD_KIND_F64:
        number = *(const double *)base;
        if (number != number || number > DBL_MAX || number < -DBL_MAX)
            return fail(error, KNURL_INVALID, "non-finite XML scalar");
        n = snprintf(g->number, sizeof(g->number), "%.17g", number);
        if (n < 0 || (size_t)n >= sizeof(g->number))
            return fail(error, KNURL_STATE, "formatting XML scalar");
        break;
    case LONEJSON_FIELD_KIND_BOOL:
        strcpy(g->number, *(const int *)base ? "true" : "false");
        break;
    default:
        return fail(error, KNURL_INVALID, "unsupported XML scalar field");
    }
    if (g->text == NULL && g->spool == NULL) {
        g->text = (const unsigned char *)g->number;
        g->remaining = strlen(g->number);
    }
    if (g->remaining > g->config.max_text_bytes)
        return fail(error, KNURL_INVALID,
                    "XML output text exceeds max_text_bytes");
    return KNURL_OK;
}
/* Validate XML characters while retaining up to three split UTF-8 bytes. */
static knurl_status text_unit(knurl_xml_generator *g, knurl_error *error)
{
    size_t n, total, i, take;
    int width, cp;
    lonejson_read_result result;
    n = g->remaining < XML_UNIT - g->carry ? g->remaining : XML_UNIT - g->carry;
    if (g->spool != NULL) {
        result = lonejson_spooled_read(g->spool, g->unit + g->carry, n);
        if (result.error_code != 0)
            return fail(error, KNURL_IO, "reading XML spooled text");
        n = result.bytes_read;
        if (n == 0 && result.would_block)
            return KNURL_AGAIN;
        if (n == 0 && g->remaining != 0)
            return fail(error, KNURL_IO, "XML spooled text ended early");
    } else {
        memcpy(g->unit + g->carry, g->text, n);
        g->text += n;
    }
    g->remaining -= n;
    total = g->carry + n;
    take = 0;
    for (i = 0; i < total;) {
        unsigned char c = g->unit[i];
        width = c < 128                  ? 1
                : c >= 0xc2 && c <= 0xdf ? 2
                : c >= 0xe0 && c <= 0xef ? 3
                : c >= 0xf0 && c <= 0xf4 ? 4
                                         : 0;
        if (width == 0)
            return fail(error, KNURL_INVALID, "invalid XML UTF-8 text");
        if (total - i < (size_t)width)
            break;
        cp = xmlGetUTF8Char(g->unit + i, &width);
        if (cp < 0 || !xmlIsCharQ(cp))
            return fail(error, KNURL_INVALID, "invalid XML character");
        i += (size_t)width;
        take = i;
    }
    g->carry = total - take;
    if (g->remaining == 0 && g->carry != 0)
        return fail(error, KNURL_INVALID, "incomplete XML UTF-8 text");
    /* WriteString requires NUL: retain carry separately while emitting. */
    {
        unsigned char tail[4];
        memcpy(tail, g->unit + take, g->carry);
        g->unit[take] = 0;
        if (xmlTextWriterWriteString(g->writer, g->unit) < 0)
            return fail(error, KNURL_BACKEND, "writing XML text");
        memcpy(g->unit, tail, g->carry);
    }
    return KNURL_OK;
}
static knurl_status array_next(knurl_xml_generator *g, struct xml_frame *f,
                               knurl_error *error)
{
    const void *base = (const char *)f->value + f->field.struct_offset;
    const void *slot = NULL;
    size_t count = 0, stride = 0;
    const unsigned char *items = NULL;
    lonejson_field item = f->field;
    int kind = FRAME_ELEMENT;
    item.struct_offset = 0;
    item.flags = LONEJSON_FIELD_REQUIRED;
    item.storage = LONEJSON_STORAGE_FIXED;
    item.fixed_capacity = 0;
#define ARRAY(T, K)                                                            \
    do {                                                                       \
        const T *a = base;                                                     \
        count = a->count;                                                      \
        items = (const unsigned char *)a->items;                               \
        stride = sizeof(a->items[0]);                                          \
        item.kind = K;                                                         \
    } while (0)
    switch (f->field.kind) {
    case LONEJSON_FIELD_KIND_STRING_ARRAY:
        ARRAY(lonejson_string_array, LONEJSON_FIELD_KIND_STRING);
        item.storage = LONEJSON_STORAGE_DYNAMIC;
        break;
    case LONEJSON_FIELD_KIND_I64_ARRAY:
        ARRAY(lonejson_i64_array, LONEJSON_FIELD_KIND_I64);
        break;
    case LONEJSON_FIELD_KIND_U64_ARRAY:
        ARRAY(lonejson_u64_array, LONEJSON_FIELD_KIND_U64);
        break;
    case LONEJSON_FIELD_KIND_F64_ARRAY:
        ARRAY(lonejson_f64_array, LONEJSON_FIELD_KIND_F64);
        break;
    case LONEJSON_FIELD_KIND_BOOL_ARRAY:
        ARRAY(lonejson_bool_array, LONEJSON_FIELD_KIND_BOOL);
        break;
    case LONEJSON_FIELD_KIND_OBJECT_ARRAY: {
        const lonejson_object_array *a = base;
        count = a->count;
        items = a->items;
        if (f->field.submap == NULL)
            return fail(error, KNURL_INVALID,
                        "XML object array requires a submap");
        stride = f->field.elem_size ? f->field.elem_size
                                    : f->field.submap->struct_size;
        if (stride < f->field.submap->struct_size)
            return fail(error, KNURL_INVALID,
                        "XML object array stride is too small");
        kind = FRAME_OBJECT;
        break;
    }
    default:
        return fail(error, KNURL_INVALID, "unsupported XML array");
    }
#undef ARRAY
    if (f->index == count) {
        pop(g);
        return KNURL_OK;
    }
    if (items == NULL || stride == 0 || count > (size_t)-1 / stride)
        return fail(error, KNURL_INVALID, "invalid XML array storage");
    slot = items + stride * f->index++;
    if (item.kind == LONEJSON_FIELD_KIND_BOOL) {
        f->boolean = *(const bool *)slot != 0;
        slot = &f->boolean;
    }
    return push(g, kind, f->name, f->field.submap, slot, &item, f->depth,
                error);
}
static knurl_status step(knurl_xml_generator *g, knurl_error *error)
{
    struct xml_frame *f = g->frame;
    const lonejson_field *field;
    const char *name;
    knurl_status status;
    int kind;
    if (f == NULL) {
        g->eof = 1;
        return KNURL_EOF;
    }
    if (f->kind == FRAME_ARRAY)
        return array_next(g, f, error);
    if (f->kind == FRAME_OBJECT) {
        if (f->stage == 0) {
            if (xmlTextWriterStartElement(g->writer, (const xmlChar *)f->name) <
                0)
                goto backend;
            f->stage = 1;
            return KNURL_OK;
        }
        if (f->stage == 1 || f->stage == 2) {
            while (f->index < f->map->field_count) {
                field = &f->map->fields[f->index++];
                if (!emit(field, f->value))
                    continue;
                if (f->stage == 1) {
                    if (!attribute(field, &g->config, &name))
                        continue;
                    if (array_kind(field) ||
                        field->kind == LONEJSON_FIELD_KIND_OBJECT)
                        return fail(error, KNURL_INVALID,
                                    "XML attributes must be scalar fields");
                    return push(g, FRAME_ATTRIBUTE, name, NULL, f->value, field,
                                f->depth, error);
                }
                if (attribute(field, &g->config, NULL))
                    continue;
                kind = array_kind(field) ? FRAME_ARRAY
                       : field->kind == LONEJSON_FIELD_KIND_OBJECT
                           ? FRAME_OBJECT
                           : FRAME_ELEMENT;
                if (g->config.text_key[0] != 0 && field->json_key != NULL &&
                    strcmp(field->json_key, g->config.text_key) == 0)
                    kind = FRAME_TEXT;
                return push(g, kind, field->json_key, field->submap,
                            kind == FRAME_OBJECT
                                ? (const char *)f->value + field->struct_offset
                                : f->value,
                            field, f->depth + (kind != FRAME_TEXT), error);
            }
            if (f->stage == 1) {
                f->stage = 2;
                f->index = 0;
                return KNURL_OK;
            }
            f->stage = 3;
        }
        if (xmlTextWriterFullEndElement(g->writer) < 0)
            goto backend;
        pop(g);
        return KNURL_OK;
    }
    if (f->stage == 0) {
        status = scalar_init(g, f, error);
        if (status != KNURL_OK)
            return status;
        if (f->kind == FRAME_ELEMENT &&
            xmlTextWriterStartElement(g->writer, (const xmlChar *)f->name) < 0)
            goto backend;
        if (f->kind == FRAME_ATTRIBUTE &&
            xmlTextWriterStartAttribute(g->writer, (const xmlChar *)f->name) <
                0)
            goto backend;
        f->stage = 1;
        return KNURL_OK;
    }
    if (g->remaining != 0 || g->carry != 0)
        return text_unit(g, error);
    if (f->kind == FRAME_ELEMENT && xmlTextWriterFullEndElement(g->writer) < 0)
        goto backend;
    if (f->kind == FRAME_ATTRIBUTE && xmlTextWriterEndAttribute(g->writer) < 0)
        goto backend;
    pop(g);
    return KNURL_OK;
backend:
    return fail(error, KNURL_BACKEND, "libxml2 XML writer operation failed");
}
void knurl_xml_source_config_init(knurl_xml_source_config *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        knurl_xml_config_init(&config->xml);
    }
}
knurl_status knurl_xml_generator_new(const knurl_xml_source_config *config,
                                     knurl_xml_generator **out,
                                     knurl_error *error)
{
    knurl_xml_generator *g;
    xmlOutputBufferPtr buffer;
    knurl_status status;
    const char *name;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || config->record == NULL || out == NULL ||
        config->record->size != sizeof(*config->record) ||
        config->record->abi_version != LONEJSON_VIEW_ABI_VERSION ||
        config->record->record == NULL)
        return fail(error, KNURL_INVALID,
                    "provide a public record view and generator output");
    status = knurl_json_schema_check(&config->record->schema, error);
    if (status != KNURL_OK)
        return status;
    g = calloc(1, sizeof(*g));
    if (g == NULL)
        return fail(error, KNURL_NOMEM, "allocating XML generator");
    g->process = getpid();
    g->thread = pthread_self();
    g->record = *config->record;
    status = knurl_xml_effective_config(&config->xml, &g->config, error);
    if (status == KNURL_OK) {
        name = g->config.root_element != NULL ? g->config.root_element
                                              : g->record.schema.map->name;
        status = push(g, FRAME_OBJECT, name, g->record.schema.map,
                      g->record.record, NULL, 1, error);
    }
    if (status == KNURL_OK) {
        buffer = xmlOutputBufferCreateIO(output, NULL, g, NULL);
        if (buffer == NULL)
            status = fail(error, KNURL_NOMEM, "allocating XML output buffer");
        else {
            g->writer = xmlNewTextWriter(buffer);
            if (g->writer == NULL) {
                xmlOutputBufferClose(buffer);
                status = fail(error, KNURL_NOMEM, "allocating XML writer");
            }
        }
    }
    if (status != KNURL_OK) {
        knurl_xml_generator_destroy(g, NULL);
        return status;
    }
    g->release = config->release;
    g->userdata = config->userdata;
    *out = g;
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_xml_generator_read(knurl_xml_generator *g, void *bytes,
                                      size_t capacity, size_t *produced,
                                      knurl_error *error)
{
    size_t n;
    knurl_status status;
    if (produced != NULL)
        *produced = 0;
    status = owner(g, error);
    if (status != KNURL_OK)
        return status;
    if (bytes == NULL || capacity == 0 || produced == NULL)
        return fail(error, KNURL_INVALID,
                    "provide a nonempty XML output buffer and count");
    if (g->failure.status != KNURL_OK) {
        if (error != NULL)
            *error = g->failure;
        return g->failure.status;
    }
    g->busy = 1;
    status = KNURL_OK;
    while (*produced < capacity) {
        if (g->offset < g->length) {
            n = g->length - g->offset;
            if (n > capacity - *produced)
                n = capacity - *produced;
            memcpy((unsigned char *)bytes + *produced, g->pending + g->offset,
                   n);
            *produced += n;
            g->offset += n;
            continue;
        }
        g->offset = g->length = 0;
        if (g->eof) {
            status = KNURL_EOF;
            break;
        }
        status = step(g, &g->failure);
        if (status == KNURL_OK || status == KNURL_EOF) {
            if (xmlTextWriterFlush(g->writer) < 0 &&
                g->failure.status == KNURL_OK)
                fail(&g->failure, KNURL_BACKEND, "flushing XML output unit");
            if (g->failure.status != KNURL_OK) {
                status = g->failure.status;
                break;
            }
            status = KNURL_OK;
        } else
            break;
    }
    g->busy = 0;
    if (status != KNURL_OK && status != KNURL_EOF && status != KNURL_AGAIN) {
        if (error != NULL)
            *error = g->failure;
        *produced = 0;
        return status;
    }
    knurl_error_clear(error);
    return *produced != 0 ? KNURL_OK : status;
}
knurl_status knurl_xml_generator_destroy(knurl_xml_generator *g,
                                         knurl_error *error)
{
    knurl_status status, outcome;
    if (g == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    status = owner(g, error);
    if (status != KNURL_OK)
        return status;
    outcome = g->failure.status != KNURL_OK ? g->failure.status
              : g->eof                      ? KNURL_OK
                                            : KNURL_CANCELLED;
    g->busy = 1;
    /* Writer destruction may flush its bounded punctuation; no user callback.
     */
    g->offset = g->length = 0;
    if (g->writer != NULL)
        xmlFreeTextWriter(g->writer);
    while (g->frame != NULL)
        pop(g);
    if (g->release != NULL)
        g->release(g->userdata, outcome);
    free(g);
    knurl_error_clear(error);
    return KNURL_OK;
}
static knurl_status source_read(void *bytes, size_t capacity, size_t *produced,
                                void *data, knurl_error *error)
{
    return knurl_xml_generator_read(data, bytes, capacity, produced, error);
}
static void source_close(void *data, knurl_status outcome)
{
    knurl_xml_generator *g = data;
    if (outcome != KNURL_OK)
        fail(&g->failure, outcome, "XML response consumption ended");
    knurl_xml_generator_destroy(g, NULL);
}
static knurl_status make_source(const knurl_xml_source_config *config,
                                knurl_source **out, knurl_xml_generator **state,
                                knurl_error *error)
{
    knurl_xml_source_config borrowed;
    knurl_source_config source;
    knurl_xml_generator *g;
    knurl_status status;
    if (config == NULL)
        return fail(error, KNURL_INVALID, "missing XML source configuration");
    borrowed = *config;
    borrowed.release = NULL;
    status = knurl_xml_generator_new(&borrowed, &g, error);
    if (status != KNURL_OK)
        return status;
    knurl_source_config_init(&source);
    source.read = source_read;
    source.close = source_close;
    source.userdata = g;
    status = knurl_source_new(&source, out, error);
    if (status != KNURL_OK)
        knurl_xml_generator_destroy(g, NULL);
    else
        *state = g;
    return status;
}
knurl_status knurl_source_xml(const knurl_xml_source_config *config,
                              knurl_source **out, knurl_error *error)
{
    knurl_xml_generator *g;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    status = make_source(config, out, &g, error);
    if (status == KNURL_OK) {
        g->release = config->release;
        g->userdata = config->userdata;
    }
    return status;
}
knurl_status knurl_reply_xml(knurl_reply *reply, int code,
                             const knurl_xml_source_config *config,
                             knurl_error *error)
{
    knurl_xml_generator *g;
    knurl_source *source;
    knurl_status status;
    status = make_source(config, &source, &g, error);
    if (status != KNURL_OK)
        return status;
    status =
        knurl_reply_source_take(reply, code, "application/xml", source, error);
    if (status == KNURL_OK) {
        g->release = config->release;
        g->userdata = config->userdata;
    } else
        knurl_source_destroy(source, NULL);
    return status;
}
knurl_status knurl_xml_write_sink(const lonejson_record_view *record,
                                  const knurl_xml_config *config,
                                  knurl_xml_sink_fn sink, void *userdata,
                                  knurl_error *error)
{
    knurl_xml_source_config source;
    knurl_xml_generator *g;
    knurl_status status;
    unsigned char bytes[8192];
    size_t n;
    if (sink == NULL)
        return fail(error, KNURL_INVALID, "missing XML sink callback");
    knurl_xml_source_config_init(&source);
    source.record = record;
    if (config != NULL)
        source.xml = *config;
    status = knurl_xml_generator_new(&source, &g, error);
    if (status != KNURL_OK)
        return status;
    for (;;) {
        status = knurl_xml_generator_read(g, bytes, sizeof(bytes), &n, error);
        if (status == KNURL_EOF) {
            status = KNURL_OK;
            break;
        }
        if (status != KNURL_OK)
            break;
        status = sink(bytes, n, userdata, error);
        if (status == KNURL_AGAIN || status == KNURL_EOF || status < KNURL_OK ||
            status > KNURL_BACKEND)
            status = fail(
                error, KNURL_INVALID,
                "synchronous XML sink must accept the complete chunk or fail");
        if (status != KNURL_OK)
            break;
    }
    knurl_xml_generator_destroy(g, NULL);
    return status;
}
struct materialized {
    knurl_owned_bytes bytes;
    size_t capacity, bound;
};
static knurl_status collect(const void *bytes, size_t n, void *data,
                            knurl_error *error)
{
    struct materialized *m = data;
    size_t capacity;
    void *next;
    if (n > m->bound - m->bytes.length)
        return fail(error, KNURL_LIMIT, "materialized XML exceeds max_bytes");
    if (n > m->capacity - m->bytes.length) {
        capacity = m->capacity ? m->capacity : 4096;
        while (capacity < m->bytes.length + n) {
            if (capacity > m->bound / 2) {
                capacity = m->bound;
                break;
            }
            capacity *= 2;
        }
        if (capacity > m->bound)
            capacity = m->bound;
        next = realloc(m->bytes.data, capacity);
        if (next == NULL)
            return fail(error, KNURL_NOMEM, "collecting materialized XML");
        m->bytes.data = next;
        m->capacity = capacity;
    }
    memcpy((unsigned char *)m->bytes.data + m->bytes.length, bytes, n);
    m->bytes.length += n;
    return KNURL_OK;
}
knurl_status
knurl_xml_serialize_materialized(const lonejson_record_view *record,
                                 const knurl_xml_config *config, size_t bound,
                                 knurl_owned_bytes *out, knurl_error *error)
{
    struct materialized m;
    knurl_status status;
    if (out == NULL || out->data != NULL || out->length != 0)
        return fail(error, KNURL_INVALID, "empty XML byte output required");
    memset(&m, 0, sizeof(m));
    m.bound = bound ? bound : (size_t)-1;
    status = knurl_xml_write_sink(record, config, collect, &m, error);
    if (status == KNURL_OK)
        *out = m.bytes;
    else
        knurl_owned_bytes_cleanup(&m.bytes);
    return status;
}
knurl_status knurl_reply_xml_materialized(knurl_reply *reply, int code,
                                          const lonejson_record_view *record,
                                          const knurl_xml_config *config,
                                          size_t bound, knurl_error *error)
{
    knurl_owned_bytes bytes;
    knurl_bytes empty;
    knurl_status status;
    memset(&bytes, 0, sizeof(bytes));
    status =
        knurl_xml_serialize_materialized(record, config, bound, &bytes, error);
    if (status != KNURL_OK)
        return status;
    empty.data = NULL;
    empty.length = 0;
    status = knurl_reply_bytes(reply, code, "application/xml", empty, error);
    if (status == KNURL_OK) {
        free(reply->body);
        reply->body = bytes.data;
        reply->body_length = bytes.length;
    } else
        knurl_owned_bytes_cleanup(&bytes);
    return status;
}
