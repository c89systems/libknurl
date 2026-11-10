/* Mapped XML reader lifted from Vectis; record ownership uses public LoneJSON
 * APIs. */
#include "xml_mapping_internal.h"
#include "internal.h"
#include <libxml/xmlreader.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif
typedef struct knurl_string_builder {
    char *data;
    size_t size, capacity;
} knurl_string_builder;
static knurl_status xml_set_error(knurl_error *error, knurl_status code,
                                  const char *message)
{
    knurl_fail(error, code, "xml_mapping", message);
    if (error != NULL)
        error->source = KNURL_ERROR_XML;
    return code;
}
static void xml_set_errorf(knurl_error *error, knurl_status code,
                           const char *format, ...)
{
    char message[192];
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);
    xml_set_error(error, code, message);
}
static void knurl_string_builder_cleanup(knurl_string_builder *s)
{
    free(s->data);
    memset(s, 0, sizeof(*s));
}
static knurl_status knurl_string_builder_append_n(knurl_string_builder *s,
                                                  const char *p, size_t n,
                                                  knurl_error *error)
{
    size_t cap;
    char *next;
    if (n >= SIZE_MAX - s->size)
        return xml_set_error(error, KNURL_NOMEM, "XML text size overflow");
    if (s->size + n + 1 > s->capacity) {
        cap = s->capacity ? s->capacity : 256;
        while (cap < s->size + n + 1) {
            if (cap > SIZE_MAX / 2) {
                cap = s->size + n + 1;
                break;
            }
            cap *= 2;
        }
        next = realloc(s->data, cap);
        if (next == NULL)
            return xml_set_error(error, KNURL_NOMEM, "allocating XML text");
        s->data = next;
        s->capacity = cap;
    }
    if (n)
        memcpy(s->data + s->size, p, n);
    s->size += n;
    s->data[s->size] = 0;
    return KNURL_OK;
}
static knurl_status knurl_string_builder_append(knurl_string_builder *s,
                                                const char *p, knurl_error *e)
{
    return knurl_string_builder_append_n(s, p, strlen(p), e);
}
typedef struct knurl_xml_field_state {
    size_t count;
    knurl_string_builder text;
    lonejson_spooled deferred_spool;
    int array, array_open, array_closed, text_pending, text_deferred,
        text_deferred_overflow, text_has_content, deferred_spool_initialized;
} knurl_xml_field_state;
void knurl_xml_config_init(knurl_xml_config *config)
{
    if (config == NULL) {
        return;
    }
    config->root_element = NULL;
    config->text_key = "text";
    config->attribute_prefix = "";
    config->trim_text = 0;
    config->skip_unknown_disabled = 0;
    config->max_depth = 64u;
    config->max_text_bytes = 67108864u;
}

knurl_status knurl_xml_effective_config(const knurl_xml_config *config,
                                        knurl_xml_config *out,
                                        knurl_error *error)
{
    knurl_xml_config defaults;

    knurl_xml_config_init(&defaults);
    *out = config != NULL ? *config : defaults;
    if (out->max_depth == 0u) {
        out->max_depth = defaults.max_depth;
    }
    if (out->max_text_bytes == 0u) {
        out->max_text_bytes = defaults.max_text_bytes;
    }
    if (out->text_key == NULL) {
        out->text_key = defaults.text_key;
    }
    if (out->attribute_prefix == NULL) {
        out->attribute_prefix = defaults.attribute_prefix;
    }
    if (out->root_element != NULL && out->root_element[0] == '\0') {
        xml_set_error(error, KNURL_INVALID,
                      "XML root_element must not be empty");
        return KNURL_INVALID;
    }
    return KNURL_OK;
}

lonejson *knurl_xml_lonejson_new(const knurl_xml_config *config,
                                 knurl_error *error)
{
    lonejson_config json_config;
    lonejson_error json_error;
    lonejson *runtime;

    json_config = lonejson_default_config();
    json_config.spool_default.max_bytes = config->max_text_bytes;
    json_config.spool_blob.max_bytes = config->max_text_bytes;
    json_config.spool_large_text.max_bytes = config->max_text_bytes;
    runtime = lonejson_new(&json_config, &json_error);
    if (runtime == NULL) {
        xml_set_errorf(error, KNURL_NOMEM,
                       "failed to initialize XML lonejson runtime: %s",
                       json_error.message);
        if (error != NULL) {
            error->source = KNURL_ERROR_LONEJSON;
            error->native_code = (long)LONEJSON_STATUS_ALLOCATION_FAILED;
        }
    }
    return runtime;
}

static int knurl_parse_lonejson_u64_text(const char *text,
                                         lonejson_uint64 *value)
{
    lonejson_uint64 limit;
    lonejson_uint64 parsed;

    while (isspace((unsigned char)*text)) {
        text++;
    }
    if (*text == '+') {
        text++;
    } else if (*text == '-') {
        return 0;
    }
    if (!isdigit((unsigned char)*text)) {
        return 0;
    }
    limit = ~(lonejson_uint64)0u;
    parsed = 0u;
    do {
        lonejson_uint64 digit;

        digit = (lonejson_uint64)(*text - '0');
        if (parsed > (limit - digit) / 10u) {
            return 0;
        }
        parsed = (lonejson_uint64)(parsed * 10u + digit);
        text++;
    } while (isdigit((unsigned char)*text));
    if (*text != '\0') {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int knurl_parse_lonejson_i64_text(const char *text,
                                         lonejson_int64 *value)
{
    lonejson_uint64 signed_max;
    lonejson_uint64 limit;
    lonejson_uint64 parsed;
    int negative;

    while (isspace((unsigned char)*text)) {
        text++;
    }
    negative = 0;
    if (*text == '-') {
        negative = 1;
        text++;
    } else if (*text == '+') {
        text++;
    }
    if (!isdigit((unsigned char)*text)) {
        return 0;
    }
    signed_max = (~(lonejson_uint64)0u) >> 1u;
    limit = negative ? signed_max + 1u : signed_max;
    parsed = 0u;
    do {
        lonejson_uint64 digit;

        digit = (lonejson_uint64)(*text - '0');
        if (parsed > (limit - digit) / 10u) {
            return 0;
        }
        parsed = (lonejson_uint64)(parsed * 10u + digit);
        text++;
    } while (isdigit((unsigned char)*text));
    if (*text != '\0') {
        return 0;
    }
    if (negative) {
        if (parsed == signed_max + 1u) {
            *value = (lonejson_int64)(-((lonejson_int64)signed_max) - 1);
        } else {
            *value = (lonejson_int64)(-((lonejson_int64)parsed));
        }
    } else {
        *value = (lonejson_int64)parsed;
    }
    return 1;
}
static const char *knurl_xml_reader_local_name(xmlTextReaderPtr reader)
{
    const xmlChar *name;

    name = xmlTextReaderConstLocalName(reader);
    if (name == NULL) {
        name = xmlTextReaderConstName(reader);
    }
    return (const char *)name;
}

static int knurl_xml_name_matches(const char *actual, const char *expected)
{
    return actual != NULL && expected != NULL && strcmp(actual, expected) == 0;
}

static const lonejson_field *knurl_xml_find_field(const lonejson_map *map,
                                                  const char *name)
{
    size_t i;
    if (map == NULL || name == NULL)
        return NULL;
    for (i = 0; i < map->field_count; ++i)
        if (map->fields[i].json_key != NULL &&
            strcmp(map->fields[i].json_key, name) == 0)
            return &map->fields[i];
    return NULL;
}

static const lonejson_field *
knurl_xml_find_attribute_field(const lonejson_map *map,
                               const knurl_xml_config *config, const char *name,
                               knurl_string_builder *key, knurl_error *error)
{
    const lonejson_field *field;

    if (config->attribute_prefix[0] == '\0') {
        return knurl_xml_find_field(map, name);
    }
    key->size = 0u;
    if (key->data != NULL) {
        key->data[0] = '\0';
    }
    if (knurl_string_builder_append(key, config->attribute_prefix, error) !=
            KNURL_OK ||
        knurl_string_builder_append(key, name, error) != KNURL_OK) {
        return NULL;
    }
    field = knurl_xml_find_field(map, key->data);
    return field != NULL ? field : knurl_xml_find_field(map, name);
}

static int knurl_xml_field_is_array(const lonejson_field *field)
{
    return field != NULL && (field->kind == LONEJSON_FIELD_KIND_STRING_ARRAY ||
                             field->kind == LONEJSON_FIELD_KIND_I64_ARRAY ||
                             field->kind == LONEJSON_FIELD_KIND_U64_ARRAY ||
                             field->kind == LONEJSON_FIELD_KIND_F64_ARRAY ||
                             field->kind == LONEJSON_FIELD_KIND_BOOL_ARRAY ||
                             field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY);
}

static void knurl_xml_trim_span(const char **data, size_t *size)
{
    const char *start;
    const char *end;

    start = *data;
    end = start + *size;
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    *data = start;
    *size = (size_t)(end - start);
}

static int knurl_xml_text_exceeds_limit(size_t accumulated, size_t size,
                                        size_t limit)
{
    return limit > 0u && (accumulated > limit || size > limit - accumulated);
}

static knurl_status knurl_xml_reject_entity_reference(knurl_error *error)
{
    xml_set_error(error, KNURL_INVALID,
                  "XML entity references are not supported");
    return KNURL_INVALID;
}

static int knurl_xml_field_is_direct_text_stream(const lonejson_field *field)
{
    return field != NULL && field->kind == LONEJSON_FIELD_KIND_STRING_STREAM;
}

static knurl_status knurl_xml_set_lonejson_string_field(
    lonejson *runtime, const lonejson_map *map, const lonejson_field *field,
    void *value, const char *text, size_t size, knurl_error *error)
{
    lonejson_error native;
    lonejson_status code;
    code = lonejson_record_assign_string(runtime, map, value, field, text, size,
                                         &native);
    return knurl_json_error(code, &native, error);
}

static knurl_status knurl_xml_set_lonejson_scalar_field(
    lonejson *runtime, const lonejson_map *map, const lonejson_field *field,
    void *value, const char *data, size_t size, const knurl_xml_config *config,
    knurl_error *error)
{
    const char *text;
    size_t text_size;
    char buffer[128];
    char *end;
    lonejson_int64 i64_value;
    lonejson_uint64 u64_value;

    text = data;
    text_size = size;
    if (config->trim_text) {
        knurl_xml_trim_span(&text, &text_size);
    }
    if (field->flags & LONEJSON_FIELD_HAS_PRESENCE) {
        *(int *)((unsigned char *)value + field->presence_offset) = 1;
    }
    if (field->kind == LONEJSON_FIELD_KIND_STRING) {
        return knurl_xml_set_lonejson_string_field(runtime, map, field, value,
                                                   text, text_size, error);
    }
    if (text_size == 0u) {
        xml_set_error(error, KNURL_INVALID,
                      "XML scalar field must not be empty");
        return KNURL_INVALID;
    }
    if (text_size >= sizeof(buffer)) {
        xml_set_error(error, KNURL_INVALID, "XML scalar field is too large");
        return KNURL_INVALID;
    }
    memcpy(buffer, text, text_size);
    buffer[text_size] = '\0';
    errno = 0;
    switch (field->kind) {
    case LONEJSON_FIELD_KIND_I64:
        if (!knurl_parse_lonejson_i64_text(buffer, &i64_value)) {
            xml_set_error(error, KNURL_INVALID, "XML scalar field is invalid");
            return KNURL_INVALID;
        }
        *(lonejson_int64 *)((unsigned char *)value + field->struct_offset) =
            i64_value;
        return KNURL_OK;
    case LONEJSON_FIELD_KIND_U64:
        if (!knurl_parse_lonejson_u64_text(buffer, &u64_value)) {
            xml_set_error(error, KNURL_INVALID, "XML scalar field is invalid");
            return KNURL_INVALID;
        }
        *(lonejson_uint64 *)((unsigned char *)value + field->struct_offset) =
            u64_value;
        return KNURL_OK;
    case LONEJSON_FIELD_KIND_F64:
        *(double *)((unsigned char *)value + field->struct_offset) =
            strtod(buffer, &end);
        break;
    case LONEJSON_FIELD_KIND_BOOL:
        if ((text_size == 4u && strncasecmp(text, "true", 4u) == 0) ||
            (text_size == 1u && text[0] == '1')) {
            *(int *)((unsigned char *)value + field->struct_offset) = 1;
            return KNURL_OK;
        } else if ((text_size == 5u && strncasecmp(text, "false", 5u) == 0) ||
                   (text_size == 1u && text[0] == '0')) {
            *(int *)((unsigned char *)value + field->struct_offset) = 0;
            return KNURL_OK;
        }
        xml_set_error(error, KNURL_INVALID,
                      "XML boolean value must be true, false, 1, or 0");
        return KNURL_INVALID;
    default:
        xml_set_error(error, KNURL_INVALID,
                      "XML lonejson field kind is not supported by direct XML "
                      "mapping");
        return KNURL_INVALID;
    }
    if (errno != 0 || end == buffer || *end != '\0' ||
        *(double *)((unsigned char *)value + field->struct_offset) !=
            *(double *)((unsigned char *)value + field->struct_offset) ||
        *(double *)((unsigned char *)value + field->struct_offset) > DBL_MAX ||
        *(double *)((unsigned char *)value + field->struct_offset) < -DBL_MAX) {
        xml_set_error(error, KNURL_INVALID, "XML scalar field is invalid");
        return KNURL_INVALID;
    }
    return KNURL_OK;
}

static knurl_status knurl_xml_lonejson_error(knurl_error *error,
                                             lonejson_status status,
                                             const lonejson_error *json_error,
                                             const char *message)
{
    if (json_error != NULL && json_error->message[0] != '\0') {
        return knurl_json_error(status, json_error, error);
    }
    xml_set_error(error, KNURL_INVALID, message);
    if (error != NULL) {
        error->source = KNURL_ERROR_LONEJSON;
        error->native_code = (long)status;
    }
    return KNURL_INVALID;
}

static knurl_status knurl_xml_set_spooled_field(const lonejson_field *field,
                                                void *value, const char *data,
                                                size_t size, knurl_error *error)
{
    lonejson_error json_error;
    lonejson_status json_status;
    lonejson_spooled *spooled;

    spooled =
        (lonejson_spooled *)((unsigned char *)value + field->struct_offset);
    json_status = lonejson_spooled_append(spooled, data, size, &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
        return knurl_xml_lonejson_error(error, json_status, &json_error,
                                        "failed to append XML spooled field");
    }
    return KNURL_OK;
}

static knurl_status knurl_xml_set_array_scalar_field(
    lonejson *runtime, const lonejson_map *map, void *record,
    const lonejson_field *field, void *array_value, const char *data,
    size_t size, const knurl_xml_config *config, knurl_error *error)
{
    lonejson_field item;
    lonejson_error native;
    lonejson_status code;
    knurl_status status;
    union {
        lonejson_int64 i;
        lonejson_uint64 u;
        double d;
        int b;
    } value;
    const char *text = data;
    if (config->trim_text)
        knurl_xml_trim_span(&text, &size);
    if (field->kind == LONEJSON_FIELD_KIND_STRING_ARRAY) {
        code = lonejson_record_array_append_string(
            runtime, map, record, field, (lonejson_string_array *)array_value,
            text, size, &native);
        return knurl_json_error(code, &native, error);
    }
    item = *field;
    item.flags = 0u;
    item.struct_offset = 0u;
    switch (field->kind) {
    case LONEJSON_FIELD_KIND_I64_ARRAY:
        item.kind = LONEJSON_FIELD_KIND_I64;
        break;
    case LONEJSON_FIELD_KIND_U64_ARRAY:
        item.kind = LONEJSON_FIELD_KIND_U64;
        break;
    case LONEJSON_FIELD_KIND_F64_ARRAY:
        item.kind = LONEJSON_FIELD_KIND_F64;
        break;
    case LONEJSON_FIELD_KIND_BOOL_ARRAY:
        item.kind = LONEJSON_FIELD_KIND_BOOL;
        break;
    default:
        return knurl_fail(error, KNURL_INVALID, "xml_mapping",
                          "unsupported XML array");
    }
    status = knurl_xml_set_lonejson_scalar_field(runtime, map, &item, &value,
                                                 text, size, config, error);
    if (status != KNURL_OK)
        return status;
    switch (item.kind) {
    case LONEJSON_FIELD_KIND_I64:
        code = lonejson_record_array_append_i64(
            runtime, map, record, field, (lonejson_i64_array *)array_value,
            value.i, &native);
        break;
    case LONEJSON_FIELD_KIND_U64:
        code = lonejson_record_array_append_u64(
            runtime, map, record, field, (lonejson_u64_array *)array_value,
            value.u, &native);
        break;
    case LONEJSON_FIELD_KIND_F64:
        code = lonejson_record_array_append_f64(
            runtime, map, record, field, (lonejson_f64_array *)array_value,
            value.d, &native);
        break;
    default:
        code = lonejson_record_array_append_bool(
            runtime, map, record, field, (lonejson_bool_array *)array_value,
            value.b, &native);
        break;
    }
    return knurl_json_error(code, &native, error);
}
static knurl_status
knurl_xml_append_object_array_item(lonejson *runtime, const lonejson_map *map,
                                   void *record, const lonejson_field *field,
                                   void *array_value, void **out,
                                   knurl_error *error)
{
    lonejson_error native;
    size_t stride;
    if (field->submap == NULL)
        return xml_set_error(error, KNURL_INVALID,
                             "XML object array needs a submap");
    stride =
        field->elem_size != 0 ? field->elem_size : field->submap->struct_size;
    if (stride < field->submap->struct_size)
        return xml_set_error(error, KNURL_INVALID,
                             "XML object array stride is too small");
    *out = lonejson_record_object_array_append_slot(
        runtime, map, record, field, (lonejson_object_array *)array_value,
        &native);
    if (*out == NULL)
        return knurl_json_error(native.code, &native, error);
    memset(*out, 0, stride);
    lonejson_init(runtime, field->submap, *out);
    return KNURL_OK;
}

static knurl_status knurl_xml_skip_element(xmlTextReaderPtr reader,
                                           size_t max_depth, knurl_error *error)
{
    int start_depth;
    int rc;
    int type;

    start_depth = xmlTextReaderDepth(reader);
    if (start_depth < 0 || (size_t)start_depth >= max_depth) {
        xml_set_error(error, KNURL_INVALID, "XML nesting exceeds max_depth");
        return KNURL_INVALID;
    }
    if (xmlTextReaderIsEmptyElement(reader)) {
        return KNURL_OK;
    }
    for (;;) {
        rc = xmlTextReaderRead(reader);
        if (rc != 1) {
            if (rc == 0) {
                xml_set_error(error, KNURL_INVALID,
                              "unexpected end of XML while skipping element");
            } else {
                xml_set_error(error, KNURL_INVALID,
                              "libxml2 failed while skipping XML element");
            }
            return KNURL_INVALID;
        }
        type = xmlTextReaderNodeType(reader);
        if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
             type == XML_READER_TYPE_DOCUMENT_TYPE)) {
            return knurl_xml_reject_entity_reference(error);
        }
        if (type == XML_READER_TYPE_ELEMENT &&
            (xmlTextReaderDepth(reader) < 0 ||
             (size_t)xmlTextReaderDepth(reader) >= max_depth)) {
            xml_set_error(error, KNURL_INVALID,
                          "XML nesting exceeds max_depth");
            return KNURL_INVALID;
        }
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth(reader) == start_depth) {
            return KNURL_OK;
        }
    }
}

static knurl_status knurl_xml_finish_array(knurl_xml_field_state *states,
                                           size_t *open_index)
{
    if (*open_index == SIZE_MAX) {
        return KNURL_OK;
    }
    states[*open_index].array_open = 0;
    states[*open_index].array_closed = 1;
    *open_index = SIZE_MAX;
    return KNURL_OK;
}

static knurl_status knurl_xml_begin_field(knurl_xml_field_state *states,
                                          size_t *open_index, size_t index,
                                          const lonejson_field *field,
                                          knurl_error *error)
{
    knurl_status status;

    if (!states[index].array && states[index].count > 0u) {
        xml_set_errorf(error, KNURL_INVALID,
                       "duplicate XML value for field '%s'", field->json_key);
        return KNURL_INVALID;
    }
    if (states[index].array && states[index].array_closed) {
        xml_set_errorf(error, KNURL_INVALID,
                       "non-contiguous XML array field '%s'", field->json_key);
        return KNURL_INVALID;
    }
    if (states[index].array && *open_index != SIZE_MAX &&
        *open_index == index) {
        states[index].count++;
        return KNURL_OK;
    }
    status = knurl_xml_finish_array(states, open_index);
    if (status != KNURL_OK) {
        return status;
    }
    if (states[index].array) {
        states[index].array_open = 1;
        *open_index = index;
    }
    states[index].count++;
    return KNURL_OK;
}

static void knurl_xml_discard_deferred_text(knurl_xml_field_state *states,
                                            size_t index,
                                            const lonejson_field *field,
                                            void *out)
{
    if (states == NULL || field == NULL || out == NULL ||
        !states[index].text_deferred) {
        return;
    }
    if (knurl_xml_field_is_direct_text_stream(field)) {
        lonejson_spooled_reset(&states[index].deferred_spool);
    } else {
        if (states[index].text.data != NULL) {
            states[index].text.data[0] = '\0';
        }
    }
    states[index].text.size = 0u;
    states[index].text_deferred = 0;
    states[index].text_deferred_overflow = 0;
}

static knurl_status knurl_xml_append_deferred_text(
    knurl_xml_field_state *state, const lonejson_field *field, const char *text,
    size_t text_size, size_t max_text_bytes, knurl_error *error)
{
    lonejson_error json_error;
    lonejson_status json_status;
    size_t current_size;
    size_t accepted;

    if (state == NULL || field == NULL || text == NULL) {
        xml_set_error(error, KNURL_INVALID,
                      "XML deferred text state is required");
        return KNURL_INVALID;
    }
    if (state->text_deferred_overflow) {
        return KNURL_OK;
    }
    current_size = knurl_xml_field_is_direct_text_stream(field)
                       ? lonejson_spooled_size(&state->deferred_spool)
                       : state->text.size;
    if (current_size >= max_text_bytes) {
        state->text_deferred_overflow = text_size > 0u;
        return KNURL_OK;
    }
    accepted = max_text_bytes - current_size;
    if (accepted > text_size) {
        accepted = text_size;
    }
    if (accepted > 0u) {
        if (knurl_xml_field_is_direct_text_stream(field)) {
            lonejson_error_init(&json_error);
            json_status = lonejson_spooled_append(&state->deferred_spool, text,
                                                  accepted, &json_error);
            if (json_status != LONEJSON_STATUS_OK) {
                return knurl_xml_lonejson_error(
                    error, json_status, &json_error,
                    "failed to spool deferred XML text");
            }
        } else if (knurl_string_builder_append_n(&state->text, text, accepted,
                                                 error) != KNURL_OK) {
            return error != NULL ? error->status : KNURL_NOMEM;
        }
    }
    if (accepted < text_size) {
        state->text_deferred_overflow = 1;
    }
    return KNURL_OK;
}

static knurl_status knurl_xml_commit_deferred_text_stream(
    knurl_xml_field_state *state, const lonejson_field *field, void *out,
    const knurl_xml_config *config, knurl_error *error)
{
    unsigned char buffer[8192];
    lonejson_error json_error;
    lonejson_read_result read_result;
    lonejson_status json_status;
    size_t deferred_size;
    knurl_status status;

    if (state == NULL || field == NULL || out == NULL || config == NULL) {
        xml_set_error(error, KNURL_INVALID,
                      "XML deferred stream state is required");
        return KNURL_INVALID;
    }
    deferred_size = lonejson_spooled_size(&state->deferred_spool);
    if (state->text_deferred_overflow ||
        knurl_xml_text_exceeds_limit(state->text.size, deferred_size,
                                     config->max_text_bytes)) {
        xml_set_error(error, KNURL_INVALID, "XML text exceeds max_text_bytes");
        return KNURL_INVALID;
    }
    lonejson_error_init(&json_error);
    json_status = lonejson_spooled_rewind(&state->deferred_spool, &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
        return knurl_xml_lonejson_error(error, json_status, &json_error,
                                        "failed to rewind deferred XML text");
    }
    for (;;) {
        read_result = lonejson_spooled_read(&state->deferred_spool, buffer,
                                            sizeof(buffer));
        if (read_result.error_code != 0) {
            xml_set_error(error, KNURL_STATE,
                          "failed to read deferred XML text");
            return KNURL_STATE;
        }
        if (read_result.bytes_read > 0u) {
            status =
                knurl_xml_set_spooled_field(field, out, (const char *)buffer,
                                            read_result.bytes_read, error);
            if (status != KNURL_OK) {
                return status;
            }
            state->text.size += read_result.bytes_read;
        }
        if (read_result.eof) {
            break;
        }
        if (read_result.would_block || read_result.bytes_read == 0u) {
            xml_set_error(error, KNURL_STATE,
                          "deferred XML text source did not complete");
            return KNURL_STATE;
        }
    }
    lonejson_spooled_reset(&state->deferred_spool);
    state->text_deferred = 0;
    state->text_deferred_overflow = 0;
    return KNURL_OK;
}

static knurl_status knurl_xml_stream_scalar_content(
    lonejson *runtime, const lonejson_map *map, xmlTextReaderPtr reader,
    const lonejson_field *field, const knurl_xml_config *config, void *dst,
    knurl_error *error)
{
    knurl_string_builder text;
    const xmlChar *xml_value;
    const char *chunk;
    size_t chunk_size;
    int start_depth;
    int type;
    int rc;
    knurl_status status;

    text.data = NULL;
    text.size = 0u;
    text.capacity = 0u;
    status = KNURL_OK;
    if (knurl_xml_field_is_direct_text_stream(field) && config->trim_text) {
        xml_set_error(error, KNURL_INVALID,
                      "XML spooled lonejson fields require trim_text=0 for "
                      "true streaming");
        return KNURL_INVALID;
    }
    if (xmlTextReaderIsEmptyElement(reader)) {
        status = knurl_xml_field_is_direct_text_stream(field)
                     ? knurl_xml_set_spooled_field(field, dst, "", 0u, error)
                     : knurl_xml_set_lonejson_scalar_field(
                           runtime, map, field, dst, "", 0u, config, error);
        knurl_string_builder_cleanup(&text);
        return status;
    }
    start_depth = xmlTextReaderDepth(reader);
    for (;;) {
        rc = xmlTextReaderRead(reader);
        if (rc != 1) {
            xml_set_error(error, KNURL_INVALID,
                          rc == 0 ? "unexpected end of XML while reading scalar"
                                  : "libxml2 failed while reading XML scalar");
            status = KNURL_INVALID;
            break;
        }
        type = xmlTextReaderNodeType(reader);
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth(reader) == start_depth) {
            break;
        }
        if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
             type == XML_READER_TYPE_DOCUMENT_TYPE)) {
            status = knurl_xml_reject_entity_reference(error);
            break;
        }
        if (type == XML_READER_TYPE_ELEMENT) {
            xml_set_error(error, KNURL_INVALID,
                          "XML scalar field contains nested elements");
            status = KNURL_INVALID;
            break;
        }
        if (type == XML_READER_TYPE_TEXT || type == XML_READER_TYPE_CDATA ||
            type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
            type == XML_READER_TYPE_WHITESPACE) {
            xml_value = xmlTextReaderConstValue(reader);
            if (xml_value != NULL) {
                chunk = (const char *)xml_value;
                chunk_size = strlen(chunk);
                if (knurl_xml_text_exceeds_limit(text.size, chunk_size,
                                                 config->max_text_bytes)) {
                    xml_set_error(error, KNURL_INVALID,
                                  "XML text exceeds max_text_bytes");
                    status = KNURL_INVALID;
                    break;
                }
                if (knurl_xml_field_is_direct_text_stream(field)) {
                    text.size += chunk_size;
                    status = knurl_xml_set_spooled_field(field, dst, chunk,
                                                         chunk_size, error);
                    if (status != KNURL_OK) {
                        break;
                    }
                } else {
                    if (knurl_string_builder_append_n(&text, chunk, chunk_size,
                                                      error) != KNURL_OK) {
                        status = error != NULL ? error->status : KNURL_NOMEM;
                        break;
                    }
                }
            }
        }
    }
    if (!knurl_xml_field_is_direct_text_stream(field) && status == KNURL_OK) {
        status = knurl_xml_set_lonejson_scalar_field(
            runtime, map, field, dst, text.data != NULL ? text.data : "",
            text.size, config, error);
    }
    knurl_string_builder_cleanup(&text);
    return status;
}

static knurl_status knurl_xml_stream_array_scalar_content(
    lonejson *runtime, const lonejson_map *map, xmlTextReaderPtr reader,
    const lonejson_field *field, const knurl_xml_config *config, void *out,
    knurl_error *error)
{
    knurl_string_builder text;
    const xmlChar *xml_value;
    const char *chunk;
    size_t chunk_size;
    int start_depth;
    int type;
    int rc;
    knurl_status status;

    text.data = NULL;
    text.size = 0u;
    text.capacity = 0u;
    status = KNURL_OK;
    if (xmlTextReaderIsEmptyElement(reader)) {
        status = knurl_xml_set_array_scalar_field(
            runtime, map, out, field,
            (unsigned char *)out + field->struct_offset, "", 0u, config, error);
        knurl_string_builder_cleanup(&text);
        return status;
    }
    start_depth = xmlTextReaderDepth(reader);
    for (;;) {
        rc = xmlTextReaderRead(reader);
        if (rc != 1) {
            xml_set_error(error, KNURL_INVALID,
                          rc == 0 ? "unexpected end of XML while reading scalar"
                                  : "libxml2 failed while reading XML scalar");
            status = KNURL_INVALID;
            break;
        }
        type = xmlTextReaderNodeType(reader);
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth(reader) == start_depth) {
            break;
        }
        if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
             type == XML_READER_TYPE_DOCUMENT_TYPE)) {
            status = knurl_xml_reject_entity_reference(error);
            break;
        }
        if (type == XML_READER_TYPE_ELEMENT) {
            xml_set_error(error, KNURL_INVALID,
                          "XML scalar field contains nested elements");
            status = KNURL_INVALID;
            break;
        }
        if (type == XML_READER_TYPE_TEXT || type == XML_READER_TYPE_CDATA ||
            type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
            type == XML_READER_TYPE_WHITESPACE) {
            xml_value = xmlTextReaderConstValue(reader);
            if (xml_value != NULL) {
                chunk = (const char *)xml_value;
                chunk_size = strlen(chunk);
                if (knurl_xml_text_exceeds_limit(text.size, chunk_size,
                                                 config->max_text_bytes)) {
                    xml_set_error(error, KNURL_INVALID,
                                  "XML text exceeds max_text_bytes");
                    status = KNURL_INVALID;
                    break;
                }
                if (knurl_string_builder_append_n(&text, chunk, chunk_size,
                                                  error) != KNURL_OK) {
                    status = error != NULL ? error->status : KNURL_NOMEM;
                    break;
                }
            }
        }
    }
    if (status == KNURL_OK) {
        status = knurl_xml_set_array_scalar_field(
            runtime, map, out, field,
            (unsigned char *)out + field->struct_offset,
            text.data != NULL ? text.data : "", text.size, config, error);
    }
    knurl_string_builder_cleanup(&text);
    return status;
}

static knurl_status knurl_xml_stream_field_value(
    lonejson *runtime, const lonejson_map *map, xmlTextReaderPtr reader,
    const lonejson_field *field, const knurl_xml_config *config, void *out,
    size_t depth, const lonejson_map **child_map, void **child_out,
    knurl_error *error)
{
    void *field_value;
    void *item;

    if (depth > config->max_depth) {
        xml_set_error(error, KNURL_INVALID, "XML nesting exceeds max_depth");
        return KNURL_INVALID;
    }
    field_value = (unsigned char *)out + field->struct_offset;
    if (knurl_xml_field_is_direct_text_stream(field) &&
        (field->flags & LONEJSON_FIELD_HAS_PRESENCE) != 0u) {
        xml_set_error(error, KNURL_INVALID,
                      "XML spooled lonejson fields do not support presence "
                      "flags");
        return KNURL_INVALID;
    }
    if (field->kind == LONEJSON_FIELD_KIND_OBJECT) {
        if (field->submap == NULL) {
            xml_set_error(error, KNURL_INVALID,
                          "XML object field requires a submap");
            return KNURL_INVALID;
        }
        *child_map = field->submap;
        *child_out = field_value;
        return KNURL_OK;
    }
    if (field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY) {
        if (knurl_xml_append_object_array_item(runtime, map, out, field,
                                               field_value, &item,
                                               error) != KNURL_OK) {
            return error != NULL ? error->status : KNURL_INVALID;
        }
        *child_map = field->submap;
        *child_out = item;
        return KNURL_OK;
    }
    if (knurl_xml_field_is_array(field)) {
        return knurl_xml_stream_array_scalar_content(runtime, map, reader,
                                                     field, config, out, error);
    }
    return knurl_xml_stream_scalar_content(runtime, map, reader, field, config,
                                           out, error);
}

static knurl_status knurl_xml_stream_text_field(
    lonejson *runtime, const lonejson_map *map, knurl_xml_field_state *states,
    size_t *open_index, const knurl_xml_config *config, void *out,
    int has_element_children, int space_preserve, int node_type,
    const char *data, size_t size, knurl_error *error)
{
    const lonejson_field *field;
    void *field_value;
    const char *text;
    const char *trimmed_text;
    size_t text_size;
    size_t trimmed_size;
    size_t index;
    int text_is_cdata;
    int text_whitespace_only;
    knurl_status status;

    if (config->text_key == NULL || config->text_key[0] == '\0') {
        return KNURL_OK;
    }
    text = data;
    text_size = size;
    text_is_cdata = node_type == XML_READER_TYPE_CDATA;
    field = knurl_xml_find_field(map, config->text_key);
    if (field == NULL) {
        if (!config->skip_unknown_disabled) {
            return KNURL_OK;
        }
        trimmed_text = text;
        trimmed_size = text_size;
        knurl_xml_trim_span(&trimmed_text, &trimmed_size);
        if (trimmed_size == 0u && !text_is_cdata && !space_preserve) {
            return KNURL_OK;
        }
        xml_set_errorf(error, KNURL_INVALID, "unknown XML text field '%s'",
                       config->text_key);
        return KNURL_INVALID;
    }
    trimmed_text = text;
    trimmed_size = text_size;
    knurl_xml_trim_span(&trimmed_text, &trimmed_size);
    text_whitespace_only = trimmed_size == 0u;
    if (knurl_xml_field_is_direct_text_stream(field) &&
        (field->flags & LONEJSON_FIELD_HAS_PRESENCE) != 0u) {
        xml_set_error(error, KNURL_INVALID,
                      "XML spooled lonejson fields do not support presence "
                      "flags");
        return KNURL_INVALID;
    }
    if (knurl_xml_field_is_direct_text_stream(field) && config->trim_text) {
        xml_set_error(error, KNURL_INVALID,
                      "XML spooled lonejson fields require trim_text=0 for "
                      "true streaming");
        return KNURL_INVALID;
    }
    if (config->trim_text && knurl_xml_field_is_array(field)) {
        if (trimmed_size == 0u) {
            return KNURL_OK;
        }
        text = trimmed_text;
        text_size = trimmed_size;
    }
    if (text_size == 0u) {
        return KNURL_OK;
    }
    index = (size_t)(field - map->fields);
    if (text_whitespace_only && !text_is_cdata && !space_preserve &&
        has_element_children && !states[index].text_pending &&
        (!knurl_xml_field_is_array(field) || !states[index].text_has_content)) {
        return KNURL_OK;
    }
    if (!(text_whitespace_only && !text_is_cdata && !space_preserve &&
          !states[index].text_pending) &&
        knurl_xml_text_exceeds_limit(0u, text_size, config->max_text_bytes)) {
        xml_set_error(error, KNURL_INVALID, "XML text exceeds max_text_bytes");
        return KNURL_INVALID;
    }
    if (text_whitespace_only && !text_is_cdata && !space_preserve &&
        !states[index].text_pending && knurl_xml_field_is_array(field)) {
        status = knurl_xml_append_deferred_text(&states[index], field, text,
                                                text_size,
                                                config->max_text_bytes, error);
        if (status != KNURL_OK) {
            return status;
        }
        states[index].text_deferred = 1;
        return KNURL_OK;
    }
    if (!knurl_xml_field_is_array(field)) {
        if (text_whitespace_only && !text_is_cdata && !space_preserve &&
            states[index].count > 0u && !states[index].text_pending) {
            return KNURL_OK;
        }
        if (text_whitespace_only && !text_is_cdata && !space_preserve &&
            !states[index].text_pending) {
            status = knurl_xml_append_deferred_text(
                &states[index], field, text, text_size, config->max_text_bytes,
                error);
            if (status != KNURL_OK) {
                return status;
            }
            states[index].text_deferred = 1;
            return KNURL_OK;
        }
        if (!states[index].text_pending) {
            if (states[index].text_deferred_overflow) {
                xml_set_error(error, KNURL_INVALID,
                              "XML text exceeds max_text_bytes");
                return KNURL_INVALID;
            }
            status =
                knurl_xml_begin_field(states, open_index, index, field, error);
            if (status != KNURL_OK) {
                return status;
            }
            if (states[index].text_deferred &&
                knurl_xml_field_is_direct_text_stream(field)) {
                status = knurl_xml_commit_deferred_text_stream(
                    &states[index], field, out, config, error);
                if (status != KNURL_OK) {
                    return status;
                }
            }
        }
        if (knurl_xml_text_exceeds_limit(states[index].text.size, text_size,
                                         config->max_text_bytes)) {
            xml_set_error(error, KNURL_INVALID,
                          "XML text exceeds max_text_bytes");
            return KNURL_INVALID;
        }
        if (knurl_xml_field_is_direct_text_stream(field)) {
            status =
                knurl_xml_set_spooled_field(field, out, text, text_size, error);
            if (status == KNURL_OK) {
                states[index].text.size += text_size;
            }
        } else {
            status = knurl_string_builder_append_n(&states[index].text, text,
                                                   text_size, error);
        }
        if (status != KNURL_OK) {
            return status;
        }
        states[index].text_pending = 1;
        states[index].text_deferred = 0;
        return KNURL_OK;
    }
    if (states[index].text_deferred) {
        if (states[index].text_deferred_overflow ||
            knurl_xml_text_exceeds_limit(states[index].text.size, 0u,
                                         config->max_text_bytes)) {
            xml_set_error(error, KNURL_INVALID,
                          "XML text exceeds max_text_bytes");
            return KNURL_INVALID;
        }
        status = knurl_xml_begin_field(states, open_index, index, field, error);
        if (status != KNURL_OK) {
            return status;
        }
        field_value = (unsigned char *)out + field->struct_offset;
        status = knurl_xml_set_array_scalar_field(
            runtime, map, out, field, field_value,
            states[index].text.data != NULL ? states[index].text.data : "",
            states[index].text.size, config, error);
        if (status != KNURL_OK) {
            return status;
        }
        states[index].text_has_content = 1;
        states[index].text.size = 0u;
        if (states[index].text.data != NULL) {
            states[index].text.data[0] = '\0';
        }
        states[index].text_deferred = 0;
        states[index].text_deferred_overflow = 0;
    }
    status = knurl_xml_begin_field(states, open_index, index, field, error);
    if (status != KNURL_OK) {
        return status;
    }
    field_value = (unsigned char *)out + field->struct_offset;
    status = knurl_xml_set_array_scalar_field(
        runtime, map, out, field, field_value, text, text_size, config, error);
    if (status == KNURL_OK) {
        states[index].text_has_content = 1;
    }
    return status;
}

/* The common shallow case needs no traversal allocation. Field states retain
 * their existing per-object ownership; only suspended reader positions grow. */
struct knurl_xml_object_frame {
    const lonejson_map *map;
    void *out;
    knurl_xml_field_state *states;
    size_t depth, open_index;
    int start_depth, space_preserve, has_element_children, entered;
};
static void knurl_xml_frame_cleanup(struct knurl_xml_object_frame *frame)
{
    size_t i;
    if (frame->states == NULL)
        return;
    for (i = 0u; i < frame->map->field_count; ++i) {
        knurl_string_builder_cleanup(&frame->states[i].text);
        if (frame->states[i].deferred_spool_initialized)
            lonejson_spooled_cleanup(&frame->states[i].deferred_spool);
    }
    free(frame->states);
    frame->states = NULL;
}
/* A child map requests descent. The reader stays on its opening element until
 * the driver enters that frame; source status remains synchronous. */
static knurl_status knurl_xml_stream_frame(xmlTextReaderPtr reader,
                                           const knurl_xml_config *config,
                                           lonejson *runtime,
                                           struct knurl_xml_object_frame *frame,
                                           const lonejson_map **child_map,
                                           void **child_out, knurl_error *error)
{
    const lonejson_map *map = frame->map;
    void *out = frame->out;
    size_t depth = frame->depth;
    knurl_xml_field_state *states;
    knurl_string_builder attr_key;
    const lonejson_field *field;
    const lonejson_field *text_field;
    const char *name;
    xmlChar *space;
    const xmlChar *value;
    size_t i;
    size_t index;
    size_t open_index;
    int empty;
    int has_element_children;
    int space_preserve;
    int start_depth;
    int type;
    int rc;
    knurl_status status;

    memset(&attr_key, 0, sizeof(attr_key));
    if (frame->entered) {
        states = frame->states;
        open_index = frame->open_index;
        start_depth = frame->start_depth;
        space_preserve = frame->space_preserve;
        has_element_children = frame->has_element_children;
        goto read_elements;
    }
    if (depth > config->max_depth) {
        xml_set_error(error, KNURL_INVALID, "XML nesting exceeds max_depth");
        return KNURL_INVALID;
    }
    if (map == NULL) {
        xml_set_error(error, KNURL_INVALID, "XML lonejson map is required");
        return KNURL_INVALID;
    }
    states =
        (knurl_xml_field_state *)calloc(map->field_count, sizeof(states[0]));
    if (states == NULL && map->field_count > 0u) {
        xml_set_error(error, KNURL_NOMEM, "failed to allocate XML field state");
        return KNURL_NOMEM;
    }
    frame->states = states;
    frame->entered = 1;
    for (i = 0u; i < map->field_count; ++i) {
        states[i].array = knurl_xml_field_is_array(&map->fields[i]);
        if (knurl_xml_field_is_direct_text_stream(&map->fields[i])) {
            lonejson_spooled_init(runtime, &states[i].deferred_spool);
            states[i].deferred_spool_initialized = 1;
        }
    }
    attr_key.data = NULL;
    attr_key.size = 0u;
    attr_key.capacity = 0u;
    open_index = SIZE_MAX;
    empty = xmlTextReaderIsEmptyElement(reader);
    space = xmlTextReaderGetAttribute(reader, (const xmlChar *)"xml:space");
    space_preserve = frame->space_preserve;
    if (space != NULL) {
        if (xmlStrEqual(space, (const xmlChar *)"preserve")) {
            space_preserve = 1;
        } else if (xmlStrEqual(space, (const xmlChar *)"default")) {
            space_preserve = 0;
        }
        xmlFree(space);
    }
    has_element_children = 0;
    start_depth = xmlTextReaderDepth(reader);
    status = KNURL_OK;
    if (xmlTextReaderMoveToFirstAttribute(reader) == 1) {
        do {
            if (xmlTextReaderIsNamespaceDecl(reader) == 1)
                continue;
            name = knurl_xml_reader_local_name(reader);
            if (name == NULL) {
                continue;
            }
            field = knurl_xml_find_attribute_field(map, config, name, &attr_key,
                                                   error);
            if (field == NULL) {
                if (error != NULL && error->status != KNURL_OK) {
                    status = error->status;
                    goto cleanup;
                }
                if (!config->skip_unknown_disabled) {
                    continue;
                }
                xml_set_errorf(error, KNURL_INVALID,
                               "unknown XML attribute '%s'", name);
                status = KNURL_INVALID;
                goto cleanup;
            }
            index = (size_t)(field - map->fields);
            status =
                knurl_xml_begin_field(states, &open_index, index, field, error);
            if (status != KNURL_OK) {
                goto cleanup;
            }
            value = xmlTextReaderConstValue(reader);
            if (knurl_xml_text_exceeds_limit(
                    0u, value != NULL ? strlen((const char *)value) : 0u,
                    config->max_text_bytes)) {
                xml_set_error(error, KNURL_INVALID,
                              "XML text exceeds max_text_bytes");
                status = KNURL_INVALID;
                goto cleanup;
            }
            if (knurl_xml_field_is_array(field)) {
                status = knurl_xml_set_array_scalar_field(
                    runtime, map, out, field,
                    (unsigned char *)out + field->struct_offset,
                    value != NULL ? (const char *)value : "",
                    value != NULL ? strlen((const char *)value) : 0u, config,
                    error);
            } else {
                status = knurl_xml_set_lonejson_scalar_field(
                    runtime, map, field, out,
                    value != NULL ? (const char *)value : "",
                    value != NULL ? strlen((const char *)value) : 0u, config,
                    error);
            }
            if (status != KNURL_OK) {
                goto cleanup;
            }
        } while (xmlTextReaderMoveToNextAttribute(reader) == 1);
        (void)xmlTextReaderMoveToElement(reader);
    }
    if (!empty) {
    read_elements:
        for (;;) {
            rc = xmlTextReaderRead(reader);
            if (rc != 1) {
                xml_set_error(error, KNURL_INVALID,
                              rc == 0
                                  ? "unexpected end of XML object"
                                  : "libxml2 failed while reading XML object");
                status = KNURL_INVALID;
                goto cleanup;
            }
            type = xmlTextReaderNodeType(reader);
            if (type == XML_READER_TYPE_END_ELEMENT &&
                xmlTextReaderDepth(reader) == start_depth) {
                break;
            }
            if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
                 type == XML_READER_TYPE_DOCUMENT_TYPE)) {
                status = knurl_xml_reject_entity_reference(error);
                goto cleanup;
            }
            if (type == XML_READER_TYPE_ELEMENT) {
                has_element_children = 1;
                if (!space_preserve && config->text_key != NULL &&
                    config->text_key[0] != '\0') {
                    text_field = knurl_xml_find_field(map, config->text_key);
                    if (text_field != NULL) {
                        index = (size_t)(text_field - map->fields);
                        if (knurl_xml_field_is_array(text_field) &&
                            states[index].text_has_content &&
                            states[index].text_deferred) {
                            if (states[index].text_deferred_overflow ||
                                knurl_xml_text_exceeds_limit(
                                    states[index].text.size, 0u,
                                    config->max_text_bytes)) {
                                xml_set_error(
                                    error, KNURL_INVALID,
                                    "XML text exceeds max_text_bytes");
                                status = KNURL_INVALID;
                                goto cleanup;
                            }
                            status = knurl_xml_begin_field(
                                states, &open_index, index, text_field, error);
                            if (status != KNURL_OK) {
                                goto cleanup;
                            }
                            status = knurl_xml_set_array_scalar_field(
                                runtime, map, out, text_field,
                                (unsigned char *)out +
                                    text_field->struct_offset,
                                states[index].text.data != NULL
                                    ? states[index].text.data
                                    : "",
                                states[index].text.size, config, error);
                            if (status != KNURL_OK) {
                                goto cleanup;
                            }
                            states[index].text.size = 0u;
                            if (states[index].text.data != NULL) {
                                states[index].text.data[0] = '\0';
                            }
                            states[index].text_deferred = 0;
                            states[index].text_deferred_overflow = 0;
                        } else {
                            knurl_xml_discard_deferred_text(states, index,
                                                            text_field, out);
                        }
                    }
                }
                name = knurl_xml_reader_local_name(reader);
                field = knurl_xml_find_field(map, name);
                if (field == NULL) {
                    if (config->skip_unknown_disabled) {
                        xml_set_errorf(error, KNURL_INVALID,
                                       "unknown XML element '%s'", name);
                        status = KNURL_INVALID;
                        goto cleanup;
                    }
                    status = knurl_xml_skip_element(reader, config->max_depth,
                                                    error);
                    if (status != KNURL_OK) {
                        goto cleanup;
                    }
                    continue;
                }
                index = (size_t)(field - map->fields);
                status = knurl_xml_begin_field(states, &open_index, index,
                                               field, error);
                if (status != KNURL_OK) {
                    goto cleanup;
                }
                status = knurl_xml_stream_field_value(
                    runtime, map, reader, field, config, out, depth + 1u,
                    child_map, child_out, error);
                if (status == KNURL_OK && *child_map != NULL) {
                    frame->open_index = open_index;
                    frame->start_depth = start_depth;
                    frame->space_preserve = space_preserve;
                    frame->has_element_children = has_element_children;
                    knurl_string_builder_cleanup(&attr_key);
                    return KNURL_OK;
                }
                if (status != KNURL_OK) {
                    goto cleanup;
                }
            } else if (type == XML_READER_TYPE_TEXT ||
                       type == XML_READER_TYPE_CDATA ||
                       type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
                       type == XML_READER_TYPE_WHITESPACE) {
                value = xmlTextReaderConstValue(reader);
                if (value != NULL) {
                    status = knurl_xml_stream_text_field(
                        runtime, map, states, &open_index, config, out,
                        has_element_children, space_preserve, type,
                        (const char *)value, strlen((const char *)value),
                        error);
                    if (status != KNURL_OK) {
                        goto cleanup;
                    }
                }
            }
        }
    }
    for (i = 0u; i < map->field_count; ++i) {
        if (states[i].text_deferred) {
            if (states[i].text_deferred_overflow ||
                (!knurl_xml_field_is_direct_text_stream(&map->fields[i]) &&
                 knurl_xml_text_exceeds_limit(states[i].text.size, 0u,
                                              config->max_text_bytes))) {
                xml_set_error(error, KNURL_INVALID,
                              "XML text exceeds max_text_bytes");
                status = KNURL_INVALID;
                goto cleanup;
            }
            status = knurl_xml_begin_field(states, &open_index, i,
                                           &map->fields[i], error);
            if (status != KNURL_OK) {
                goto cleanup;
            }
            if (knurl_xml_field_is_array(&map->fields[i])) {
                status = knurl_xml_set_array_scalar_field(
                    runtime, map, out, &map->fields[i],
                    (unsigned char *)out + map->fields[i].struct_offset,
                    states[i].text.data != NULL ? states[i].text.data : "",
                    states[i].text.size, config, error);
                if (status != KNURL_OK) {
                    goto cleanup;
                }
                states[i].text_has_content = 1;
                states[i].text.size = 0u;
                if (states[i].text.data != NULL) {
                    states[i].text.data[0] = '\0';
                }
            } else {
                if (knurl_xml_field_is_direct_text_stream(&map->fields[i])) {
                    status = knurl_xml_commit_deferred_text_stream(
                        &states[i], &map->fields[i], out, config, error);
                    if (status != KNURL_OK) {
                        goto cleanup;
                    }
                }
                states[i].text_pending = 1;
            }
            states[i].text_deferred = 0;
            states[i].text_deferred_overflow = 0;
        }
        if (!states[i].text_pending) {
            continue;
        }
        if (knurl_xml_field_is_direct_text_stream(&map->fields[i])) {
            if (knurl_xml_text_exceeds_limit(states[i].text.size, 0u,
                                             config->max_text_bytes)) {
                xml_set_error(error, KNURL_INVALID,
                              "XML text exceeds max_text_bytes");
                status = KNURL_INVALID;
                goto cleanup;
            }
            continue;
        }
        status = knurl_xml_set_lonejson_scalar_field(
            runtime, map, &map->fields[i], out,
            states[i].text.data != NULL ? states[i].text.data : "",
            states[i].text.size, config, error);
        if (status != KNURL_OK) {
            goto cleanup;
        }
    }
    status = knurl_xml_finish_array(states, &open_index);
    if (status != KNURL_OK) {
        goto cleanup;
    }
    for (i = 0u; i < map->field_count; ++i) {
        if ((map->fields[i].flags & LONEJSON_FIELD_REQUIRED) != 0u &&
            states[i].count == 0u) {
            xml_set_errorf(error, KNURL_INVALID,
                           "missing required XML field '%s'",
                           map->fields[i].json_key);
            status = KNURL_INVALID;
            goto cleanup;
        }
    }

cleanup:
    knurl_string_builder_cleanup(&attr_key);
    knurl_xml_frame_cleanup(frame);
    return status;
}

static knurl_status knurl_xml_stream_object(xmlTextReaderPtr reader,
                                            const lonejson_map *map,
                                            const knurl_xml_config *config,
                                            lonejson *runtime, void *out,
                                            knurl_error *error)
{
    struct knurl_xml_object_frame local[8], *frames = local, *next;
    const lonejson_map *child_map;
    void *child_out;
    size_t count = 1u, capacity = 8u;
    knurl_status status;
    memset(&frames[0], 0, sizeof(frames[0]));
    frames[0].map = map;
    frames[0].out = out;
    frames[0].depth = 1u;
    while (count != 0u) {
        child_map = NULL;
        child_out = NULL;
        status =
            knurl_xml_stream_frame(reader, config, runtime, &frames[count - 1u],
                                   &child_map, &child_out, error);
        if (status != KNURL_OK)
            goto cleanup;
        if (child_map == NULL) {
            --count;
            continue;
        }
        if (count == capacity) {
            if (capacity > SIZE_MAX / sizeof(frames[0]) / 2u) {
                status = xml_set_error(error, KNURL_NOMEM,
                                       "XML traversal size overflow");
                goto cleanup;
            }
            if (frames == local) {
                next = malloc(capacity * 2u * sizeof(frames[0]));
                if (next != NULL)
                    memcpy(next, frames, count * sizeof(frames[0]));
            } else {
                next = realloc(frames, capacity * 2u * sizeof(frames[0]));
            }
            if (next == NULL) {
                status = xml_set_error(error, KNURL_NOMEM,
                                       "allocating XML traversal frames");
                goto cleanup;
            }
            frames = next;
            capacity *= 2u;
        }
        memset(&frames[count], 0, sizeof(frames[count]));
        frames[count].map = child_map;
        frames[count].out = child_out;
        frames[count].depth = frames[count - 1u].depth + 1u;
        frames[count].space_preserve = frames[count - 1u].space_preserve;
        ++count;
    }
    status = KNURL_OK;
cleanup:
    while (count != 0u)
        knurl_xml_frame_cleanup(&frames[--count]);
    if (frames != local)
        free(frames);
    return status;
}

static knurl_status knurl_xml_stream_document(xmlTextReaderPtr reader,
                                              const lonejson_map *map,
                                              const knurl_xml_config *config,
                                              lonejson *runtime, void *out,
                                              knurl_error *error)
{
    const char *name;
    const xmlChar *value;
    const char *text;
    size_t text_size;
    int type;
    int rc;
    knurl_status status;

    for (;;) {
        rc = xmlTextReaderRead(reader);
        if (rc != 1) {
            xml_set_error(error, KNURL_INVALID,
                          "XML document has no root element");
            return KNURL_INVALID;
        }
        type = xmlTextReaderNodeType(reader);
        if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
             type == XML_READER_TYPE_DOCUMENT_TYPE)) {
            return knurl_xml_reject_entity_reference(error);
        }
        if (type == XML_READER_TYPE_ELEMENT) {
            break;
        }
    }
    name = knurl_xml_reader_local_name(reader);
    if (config->root_element != NULL &&
        !knurl_xml_name_matches(name, config->root_element)) {
        xml_set_errorf(error, KNURL_INVALID,
                       "XML root element '%s' does not match expected '%s'",
                       name != NULL ? name : "", config->root_element);
        return KNURL_INVALID;
    }
    status = knurl_xml_stream_object(reader, map, config, runtime, out, error);
    if (status != KNURL_OK) {
        return status;
    }
    for (;;) {
        rc = xmlTextReaderRead(reader);
        if (rc == 0) {
            return KNURL_OK;
        }
        if (rc < 0) {
            xml_set_error(error, KNURL_INVALID,
                          "libxml2 failed while reading after XML root");
            return KNURL_INVALID;
        }
        type = xmlTextReaderNodeType(reader);
        if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
             type == XML_READER_TYPE_DOCUMENT_TYPE)) {
            return knurl_xml_reject_entity_reference(error);
        }
        if (type == XML_READER_TYPE_ELEMENT) {
            xml_set_error(error, KNURL_INVALID,
                          "XML document contains multiple root elements");
            return KNURL_INVALID;
        }
        if (type != XML_READER_TYPE_TEXT && type != XML_READER_TYPE_CDATA &&
            type != XML_READER_TYPE_SIGNIFICANT_WHITESPACE &&
            type != XML_READER_TYPE_WHITESPACE) {
            continue;
        }
        value = xmlTextReaderConstValue(reader);
        text = value != NULL ? (const char *)value : "";
        text_size = value != NULL ? strlen(text) : 0u;
        knurl_xml_trim_span(&text, &text_size);
        if (text_size != 0u) {
            xml_set_error(error, KNURL_INVALID,
                          "XML document has non-whitespace content after root");
            return KNURL_INVALID;
        }
    }
}

struct xml_input {
    knurl_source *source;
    knurl_error failure;
};
static int xml_input_read(void *context, char *bytes, int capacity)
{
    struct xml_input *input = context;
    knurl_status status;
    size_t produced = 0;
    if (input->failure.status != KNURL_OK)
        return -1;
    if (capacity <= 0)
        return 0;
    status = knurl_source_read(input->source, bytes, (size_t)capacity,
                               &produced, &input->failure);
    if (status == KNURL_EOF)
        return 0;
    if (status == KNURL_AGAIN) {
        xml_set_error(&input->failure, KNURL_STATE,
                      "mapped XML requires a synchronous completed source");
        return -1;
    }
    if (status != KNURL_OK)
        return -1;
    return (int)produced;
}
static void xml_input_error(void *context, const xmlError *native)
{
    struct xml_input *input = context;
    if (native == NULL || native->level < XML_ERR_ERROR ||
        input->failure.status != KNURL_OK)
        return;
    xml_set_error(
        &input->failure,
        native->code == XML_ERR_NO_MEMORY ? KNURL_NOMEM : KNURL_INVALID,
        native->message == NULL ? "invalid XML input" : native->message);
    input->failure.native_code = native->code;
    snprintf(input->failure.detail, sizeof(input->failure.detail),
             "line %d, column %d", native->line, native->int2);
}
static xmlParserErrors xml_input_no_resource(void *context, const char *url,
                                             const char *public_id,
                                             xmlResourceType type,
                                             xmlParserInputFlags flags,
                                             xmlParserInput **out)
{
    struct xml_input *input = context;
    (void)url;
    (void)public_id;
    (void)type;
    (void)flags;
    *out = NULL;
    xml_set_error(&input->failure, KNURL_INVALID,
                  "external XML resources are disabled");
    return XML_IO_LOAD_ERROR;
}
knurl_status knurl_xml_parse_source(knurl_source *source,
                                    const lonejson_schema_view *schema,
                                    const knurl_xml_config *config,
                                    void *record, knurl_error *error)
{
    knurl_xml_config effective;
    struct xml_input input;
    xmlTextReaderPtr reader;
    lonejson *runtime;
    knurl_status status;
    int options, owned;
    knurl_error_clear(error);
    if (source == NULL || record == NULL)
        return xml_set_error(error, KNURL_INVALID,
                             "source and initialized record are required");
    status = knurl_json_schema_check(schema, error);
    if (status != KNURL_OK)
        return status;
    status = knurl_xml_effective_config(config, &effective, error);
    if (status != KNURL_OK)
        return status;
    runtime = schema->runtime;
    owned = runtime == NULL;
    if (owned)
        runtime = knurl_xml_lonejson_new(&effective, error);
    if (runtime == NULL)
        return error != NULL ? error->status : KNURL_NOMEM;
    lonejson_reset(runtime, schema->map, record);
    memset(&input, 0, sizeof(input));
    input.source = source;
    options = XML_PARSE_NONET | XML_PARSE_COMPACT;
    if (effective.max_text_bytes >= 10000000u)
        options |= XML_PARSE_HUGE;
    reader = xmlReaderForIO(xml_input_read, NULL, &input, NULL, NULL, options);
    if (reader == NULL)
        status = xml_set_error(error, KNURL_NOMEM, "creating XML reader");
    else {
        xmlTextReaderSetStructuredErrorHandler(reader, xml_input_error, &input);
        xmlTextReaderSetResourceLoader(reader, xml_input_no_resource, &input);
        status = knurl_xml_stream_document(reader, schema->map, &effective,
                                           runtime, record, error);
        xmlFreeTextReader(reader);
    }
    if (input.failure.status != KNURL_OK) {
        status = input.failure.status;
        if (error != NULL)
            *error = input.failure;
    }
    if (status == KNURL_LIMIT || status == KNURL_OVERFLOW) {
        status = KNURL_INVALID;
        if (error != NULL)
            error->status = status;
    }
    if (status != KNURL_OK)
        lonejson_reset(runtime, schema->map, record);
    if (owned)
        lonejson_free(runtime);
    if (status == KNURL_OK)
        knurl_error_clear(error);
    return status;
}
static knurl_status xml_body_read(void *bytes, size_t capacity,
                                  size_t *produced, void *data,
                                  knurl_error *error)
{
    return knurl_body_read(data, bytes, capacity, produced, error);
}
knurl_status knurl_request_xml_into(knurl_request *request,
                                    const lonejson_schema_view *schema,
                                    const knurl_xml_config *config,
                                    void *record, knurl_error *error)
{
    knurl_body *body;
    knurl_source *source;
    knurl_source_config input;
    knurl_status status, rewind_status;
    knurl_error rewind_error;
    body = knurl_request_body(request);
    if (body == NULL || knurl_body_kind(body) == KNURL_BODY_STORAGE_NONE)
        return xml_set_error(
            error, KNURL_STATE,
            "XML mapping needs a completed BUFFERED or MATERIALIZED body");
    status = knurl_body_rewind(body, error);
    if (status != KNURL_OK)
        return status;
    knurl_source_config_init(&input);
    input.read = xml_body_read;
    input.userdata = body;
    status = knurl_source_new(&input, &source, error);
    if (status == KNURL_OK) {
        status = knurl_xml_parse_source(source, schema, config, record, error);
        knurl_source_destroy(source, NULL);
    }
    rewind_status = knurl_body_rewind(body, &rewind_error);
    if (status == KNURL_OK && rewind_status != KNURL_OK) {
        status = rewind_status;
        if (error != NULL)
            *error = rewind_error;
    }
    return status;
}
