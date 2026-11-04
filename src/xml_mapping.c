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
typedef struct rotmos_string_builder {
    char *data;
    size_t size, capacity;
} rotmos_string_builder;
static rotmos_status xml_set_error(rotmos_error *error, rotmos_status code,
                                   const char *message)
{
    rotmos_fail(error, code, "xml_mapping", message);
    if (error != NULL)
        error->source = ROTMOS_ERROR_XML;
    return code;
}
static void xml_set_errorf(rotmos_error *error, rotmos_status code,
                           const char *format, ...)
{
    char message[192];
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);
    xml_set_error(error, code, message);
}
static void rotmos_string_builder_cleanup(rotmos_string_builder *s)
{
    free(s->data);
    memset(s, 0, sizeof(*s));
}
static rotmos_status rotmos_string_builder_append_n(rotmos_string_builder *s,
                                                    const char *p, size_t n,
                                                    rotmos_error *error)
{
    size_t cap;
    char *next;
    if (n >= SIZE_MAX - s->size)
        return xml_set_error(error, ROTMOS_NOMEM, "XML text size overflow");
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
            return xml_set_error(error, ROTMOS_NOMEM, "allocating XML text");
        s->data = next;
        s->capacity = cap;
    }
    if (n)
        memcpy(s->data + s->size, p, n);
    s->size += n;
    s->data[s->size] = 0;
    return ROTMOS_OK;
}
static rotmos_status rotmos_string_builder_append(rotmos_string_builder *s,
                                                  const char *p,
                                                  rotmos_error *e)
{
    return rotmos_string_builder_append_n(s, p, strlen(p), e);
}
typedef struct rotmos_xml_field_state {
    size_t count;
    rotmos_string_builder text;
    lonejson_spooled deferred_spool;
    int array, array_open, array_closed, text_pending, text_deferred,
        text_deferred_overflow, text_has_content, deferred_spool_initialized;
} rotmos_xml_field_state;
void rotmos_xml_config_init(rotmos_xml_config *config)
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

rotmos_status rotmos_xml_effective_config(const rotmos_xml_config *config,
                                          rotmos_xml_config *out,
                                          rotmos_error *error)
{
    rotmos_xml_config defaults;

    rotmos_xml_config_init(&defaults);
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
        xml_set_error(error, ROTMOS_INVALID,
                      "XML root_element must not be empty");
        return ROTMOS_INVALID;
    }
    return ROTMOS_OK;
}

lonejson *rotmos_xml_lonejson_new(const rotmos_xml_config *config,
                                  rotmos_error *error)
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
        xml_set_errorf(error, ROTMOS_NOMEM,
                       "failed to initialize XML lonejson runtime: %s",
                       json_error.message);
        if (error != NULL) {
            error->source = ROTMOS_ERROR_LONEJSON;
            error->native_code = (long)LONEJSON_STATUS_ALLOCATION_FAILED;
        }
    }
    return runtime;
}

static int rotmos_parse_lonejson_u64_text(const char *text,
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

static int rotmos_parse_lonejson_i64_text(const char *text,
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
static const char *rotmos_xml_reader_local_name(xmlTextReaderPtr reader)
{
    const xmlChar *name;

    name = xmlTextReaderConstLocalName(reader);
    if (name == NULL) {
        name = xmlTextReaderConstName(reader);
    }
    return (const char *)name;
}

static int rotmos_xml_name_matches(const char *actual, const char *expected)
{
    return actual != NULL && expected != NULL && strcmp(actual, expected) == 0;
}

static const lonejson_field *rotmos_xml_find_field(const lonejson_map *map,
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

static const lonejson_field *rotmos_xml_find_attribute_field(
    const lonejson_map *map, const rotmos_xml_config *config, const char *name,
    rotmos_string_builder *key, rotmos_error *error)
{
    const lonejson_field *field;

    if (config->attribute_prefix[0] == '\0') {
        return rotmos_xml_find_field(map, name);
    }
    key->size = 0u;
    if (key->data != NULL) {
        key->data[0] = '\0';
    }
    if (rotmos_string_builder_append(key, config->attribute_prefix, error) !=
            ROTMOS_OK ||
        rotmos_string_builder_append(key, name, error) != ROTMOS_OK) {
        return NULL;
    }
    field = rotmos_xml_find_field(map, key->data);
    return field != NULL ? field : rotmos_xml_find_field(map, name);
}

static int rotmos_xml_field_is_array(const lonejson_field *field)
{
    return field != NULL && (field->kind == LONEJSON_FIELD_KIND_STRING_ARRAY ||
                             field->kind == LONEJSON_FIELD_KIND_I64_ARRAY ||
                             field->kind == LONEJSON_FIELD_KIND_U64_ARRAY ||
                             field->kind == LONEJSON_FIELD_KIND_F64_ARRAY ||
                             field->kind == LONEJSON_FIELD_KIND_BOOL_ARRAY ||
                             field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY);
}

static void rotmos_xml_trim_span(const char **data, size_t *size)
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

static int rotmos_xml_text_exceeds_limit(size_t accumulated, size_t size,
                                         size_t limit)
{
    return limit > 0u && (accumulated > limit || size > limit - accumulated);
}

static rotmos_status rotmos_xml_reject_entity_reference(rotmos_error *error)
{
    xml_set_error(error, ROTMOS_INVALID,
                  "XML entity references are not supported");
    return ROTMOS_INVALID;
}

static int rotmos_xml_field_is_direct_text_stream(const lonejson_field *field)
{
    return field != NULL && field->kind == LONEJSON_FIELD_KIND_STRING_STREAM;
}

static rotmos_status rotmos_xml_set_lonejson_string_field(
    lonejson *runtime, const lonejson_map *map, const lonejson_field *field,
    void *value, const char *text, size_t size, rotmos_error *error)
{
    lonejson_error native;
    lonejson_status code;
    code = lonejson_record_assign_string(runtime, map, value, field, text, size,
                                         &native);
    return rotmos_json_error(code, &native, error);
}

static rotmos_status rotmos_xml_set_lonejson_scalar_field(
    lonejson *runtime, const lonejson_map *map, const lonejson_field *field,
    void *value, const char *data, size_t size, const rotmos_xml_config *config,
    rotmos_error *error)
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
        rotmos_xml_trim_span(&text, &text_size);
    }
    if (field->flags & LONEJSON_FIELD_HAS_PRESENCE) {
        *(int *)((unsigned char *)value + field->presence_offset) = 1;
    }
    if (field->kind == LONEJSON_FIELD_KIND_STRING) {
        return rotmos_xml_set_lonejson_string_field(runtime, map, field, value,
                                                    text, text_size, error);
    }
    if (text_size == 0u) {
        xml_set_error(error, ROTMOS_INVALID,
                      "XML scalar field must not be empty");
        return ROTMOS_INVALID;
    }
    if (text_size >= sizeof(buffer)) {
        xml_set_error(error, ROTMOS_INVALID, "XML scalar field is too large");
        return ROTMOS_INVALID;
    }
    memcpy(buffer, text, text_size);
    buffer[text_size] = '\0';
    errno = 0;
    switch (field->kind) {
    case LONEJSON_FIELD_KIND_I64:
        if (!rotmos_parse_lonejson_i64_text(buffer, &i64_value)) {
            xml_set_error(error, ROTMOS_INVALID, "XML scalar field is invalid");
            return ROTMOS_INVALID;
        }
        *(lonejson_int64 *)((unsigned char *)value + field->struct_offset) =
            i64_value;
        return ROTMOS_OK;
    case LONEJSON_FIELD_KIND_U64:
        if (!rotmos_parse_lonejson_u64_text(buffer, &u64_value)) {
            xml_set_error(error, ROTMOS_INVALID, "XML scalar field is invalid");
            return ROTMOS_INVALID;
        }
        *(lonejson_uint64 *)((unsigned char *)value + field->struct_offset) =
            u64_value;
        return ROTMOS_OK;
    case LONEJSON_FIELD_KIND_F64:
        *(double *)((unsigned char *)value + field->struct_offset) =
            strtod(buffer, &end);
        break;
    case LONEJSON_FIELD_KIND_BOOL:
        if ((text_size == 4u && strncasecmp(text, "true", 4u) == 0) ||
            (text_size == 1u && text[0] == '1')) {
            *(int *)((unsigned char *)value + field->struct_offset) = 1;
            return ROTMOS_OK;
        } else if ((text_size == 5u && strncasecmp(text, "false", 5u) == 0) ||
                   (text_size == 1u && text[0] == '0')) {
            *(int *)((unsigned char *)value + field->struct_offset) = 0;
            return ROTMOS_OK;
        }
        xml_set_error(error, ROTMOS_INVALID,
                      "XML boolean value must be true, false, 1, or 0");
        return ROTMOS_INVALID;
    default:
        xml_set_error(error, ROTMOS_INVALID,
                      "XML lonejson field kind is not supported by direct XML "
                      "mapping");
        return ROTMOS_INVALID;
    }
    if (errno != 0 || end == buffer || *end != '\0' ||
        *(double *)((unsigned char *)value + field->struct_offset) !=
            *(double *)((unsigned char *)value + field->struct_offset) ||
        *(double *)((unsigned char *)value + field->struct_offset) > DBL_MAX ||
        *(double *)((unsigned char *)value + field->struct_offset) < -DBL_MAX) {
        xml_set_error(error, ROTMOS_INVALID, "XML scalar field is invalid");
        return ROTMOS_INVALID;
    }
    return ROTMOS_OK;
}

static rotmos_status rotmos_xml_lonejson_error(rotmos_error *error,
                                               lonejson_status status,
                                               const lonejson_error *json_error,
                                               const char *message)
{
    if (json_error != NULL && json_error->message[0] != '\0') {
        return rotmos_json_error(status, json_error, error);
    }
    xml_set_error(error, ROTMOS_INVALID, message);
    if (error != NULL) {
        error->source = ROTMOS_ERROR_LONEJSON;
        error->native_code = (long)status;
    }
    return ROTMOS_INVALID;
}

static rotmos_status rotmos_xml_set_spooled_field(const lonejson_field *field,
                                                  void *value, const char *data,
                                                  size_t size,
                                                  rotmos_error *error)
{
    lonejson_error json_error;
    lonejson_status json_status;
    lonejson_spooled *spooled;

    spooled =
        (lonejson_spooled *)((unsigned char *)value + field->struct_offset);
    json_status = lonejson_spooled_append(spooled, data, size, &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
        return rotmos_xml_lonejson_error(error, json_status, &json_error,
                                         "failed to append XML spooled field");
    }
    return ROTMOS_OK;
}

static rotmos_status rotmos_xml_set_array_scalar_field(
    lonejson *runtime, const lonejson_map *map, void *record,
    const lonejson_field *field, void *array_value, const char *data,
    size_t size, const rotmos_xml_config *config, rotmos_error *error)
{
    lonejson_field item;
    lonejson_error native;
    lonejson_status code;
    rotmos_status status;
    union {
        lonejson_int64 i;
        lonejson_uint64 u;
        double d;
        int b;
    } value;
    const char *text = data;
    if (config->trim_text)
        rotmos_xml_trim_span(&text, &size);
    if (field->kind == LONEJSON_FIELD_KIND_STRING_ARRAY) {
        code = lonejson_record_array_append_string(
            runtime, map, record, field, (lonejson_string_array *)array_value,
            text, size, &native);
        return rotmos_json_error(code, &native, error);
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
        return rotmos_fail(error, ROTMOS_INVALID, "xml_mapping",
                           "unsupported XML array");
    }
    status = rotmos_xml_set_lonejson_scalar_field(runtime, map, &item, &value,
                                                  text, size, config, error);
    if (status != ROTMOS_OK)
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
    return rotmos_json_error(code, &native, error);
}
static rotmos_status
rotmos_xml_append_object_array_item(lonejson *runtime, const lonejson_map *map,
                                    void *record, const lonejson_field *field,
                                    void *array_value, void **out,
                                    rotmos_error *error)
{
    lonejson_error native;
    size_t stride;
    if (field->submap == NULL)
        return xml_set_error(error, ROTMOS_INVALID,
                             "XML object array needs a submap");
    stride =
        field->elem_size != 0 ? field->elem_size : field->submap->struct_size;
    if (stride < field->submap->struct_size)
        return xml_set_error(error, ROTMOS_INVALID,
                             "XML object array stride is too small");
    *out = lonejson_record_object_array_append_slot(
        runtime, map, record, field, (lonejson_object_array *)array_value,
        &native);
    if (*out == NULL)
        return rotmos_json_error(native.code, &native, error);
    memset(*out, 0, stride);
    lonejson_init(runtime, field->submap, *out);
    return ROTMOS_OK;
}

static rotmos_status rotmos_xml_skip_element(xmlTextReaderPtr reader,
                                             size_t max_depth,
                                             rotmos_error *error)
{
    int start_depth;
    int rc;
    int type;

    start_depth = xmlTextReaderDepth(reader);
    if (start_depth < 0 || (size_t)start_depth >= max_depth) {
        xml_set_error(error, ROTMOS_INVALID, "XML nesting exceeds max_depth");
        return ROTMOS_INVALID;
    }
    if (xmlTextReaderIsEmptyElement(reader)) {
        return ROTMOS_OK;
    }
    for (;;) {
        rc = xmlTextReaderRead(reader);
        if (rc != 1) {
            if (rc == 0) {
                xml_set_error(error, ROTMOS_INVALID,
                              "unexpected end of XML while skipping element");
            } else {
                xml_set_error(error, ROTMOS_INVALID,
                              "libxml2 failed while skipping XML element");
            }
            return ROTMOS_INVALID;
        }
        type = xmlTextReaderNodeType(reader);
        if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
             type == XML_READER_TYPE_DOCUMENT_TYPE)) {
            return rotmos_xml_reject_entity_reference(error);
        }
        if (type == XML_READER_TYPE_ELEMENT &&
            (xmlTextReaderDepth(reader) < 0 ||
             (size_t)xmlTextReaderDepth(reader) >= max_depth)) {
            xml_set_error(error, ROTMOS_INVALID,
                          "XML nesting exceeds max_depth");
            return ROTMOS_INVALID;
        }
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth(reader) == start_depth) {
            return ROTMOS_OK;
        }
    }
}

static rotmos_status
rotmos_xml_stream_object(xmlTextReaderPtr reader, const lonejson_map *map,
                         const rotmos_xml_config *config, lonejson *runtime,
                         void *out, size_t depth, int parent_space_preserve,
                         rotmos_error *error);

static rotmos_status rotmos_xml_finish_array(rotmos_xml_field_state *states,
                                             size_t *open_index)
{
    if (*open_index == SIZE_MAX) {
        return ROTMOS_OK;
    }
    states[*open_index].array_open = 0;
    states[*open_index].array_closed = 1;
    *open_index = SIZE_MAX;
    return ROTMOS_OK;
}

static rotmos_status rotmos_xml_begin_field(rotmos_xml_field_state *states,
                                            size_t *open_index, size_t index,
                                            const lonejson_field *field,
                                            rotmos_error *error)
{
    rotmos_status status;

    if (!states[index].array && states[index].count > 0u) {
        xml_set_errorf(error, ROTMOS_INVALID,
                       "duplicate XML value for field '%s'", field->json_key);
        return ROTMOS_INVALID;
    }
    if (states[index].array && states[index].array_closed) {
        xml_set_errorf(error, ROTMOS_INVALID,
                       "non-contiguous XML array field '%s'", field->json_key);
        return ROTMOS_INVALID;
    }
    if (states[index].array && *open_index != SIZE_MAX &&
        *open_index == index) {
        states[index].count++;
        return ROTMOS_OK;
    }
    status = rotmos_xml_finish_array(states, open_index);
    if (status != ROTMOS_OK) {
        return status;
    }
    if (states[index].array) {
        states[index].array_open = 1;
        *open_index = index;
    }
    states[index].count++;
    return ROTMOS_OK;
}

static void rotmos_xml_discard_deferred_text(rotmos_xml_field_state *states,
                                             size_t index,
                                             const lonejson_field *field,
                                             void *out)
{
    if (states == NULL || field == NULL || out == NULL ||
        !states[index].text_deferred) {
        return;
    }
    if (rotmos_xml_field_is_direct_text_stream(field)) {
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

static rotmos_status
rotmos_xml_append_deferred_text(rotmos_xml_field_state *state,
                                const lonejson_field *field, const char *text,
                                size_t text_size, size_t max_text_bytes,
                                rotmos_error *error)
{
    lonejson_error json_error;
    lonejson_status json_status;
    size_t current_size;
    size_t accepted;

    if (state == NULL || field == NULL || text == NULL) {
        xml_set_error(error, ROTMOS_INVALID,
                      "XML deferred text state is required");
        return ROTMOS_INVALID;
    }
    if (state->text_deferred_overflow) {
        return ROTMOS_OK;
    }
    current_size = rotmos_xml_field_is_direct_text_stream(field)
                       ? lonejson_spooled_size(&state->deferred_spool)
                       : state->text.size;
    if (current_size >= max_text_bytes) {
        state->text_deferred_overflow = text_size > 0u;
        return ROTMOS_OK;
    }
    accepted = max_text_bytes - current_size;
    if (accepted > text_size) {
        accepted = text_size;
    }
    if (accepted > 0u) {
        if (rotmos_xml_field_is_direct_text_stream(field)) {
            lonejson_error_init(&json_error);
            json_status = lonejson_spooled_append(&state->deferred_spool, text,
                                                  accepted, &json_error);
            if (json_status != LONEJSON_STATUS_OK) {
                return rotmos_xml_lonejson_error(
                    error, json_status, &json_error,
                    "failed to spool deferred XML text");
            }
        } else if (rotmos_string_builder_append_n(&state->text, text, accepted,
                                                  error) != ROTMOS_OK) {
            return error != NULL ? error->status : ROTMOS_NOMEM;
        }
    }
    if (accepted < text_size) {
        state->text_deferred_overflow = 1;
    }
    return ROTMOS_OK;
}

static rotmos_status rotmos_xml_commit_deferred_text_stream(
    rotmos_xml_field_state *state, const lonejson_field *field, void *out,
    const rotmos_xml_config *config, rotmos_error *error)
{
    unsigned char buffer[8192];
    lonejson_error json_error;
    lonejson_read_result read_result;
    lonejson_status json_status;
    size_t deferred_size;
    rotmos_status status;

    if (state == NULL || field == NULL || out == NULL || config == NULL) {
        xml_set_error(error, ROTMOS_INVALID,
                      "XML deferred stream state is required");
        return ROTMOS_INVALID;
    }
    deferred_size = lonejson_spooled_size(&state->deferred_spool);
    if (state->text_deferred_overflow ||
        rotmos_xml_text_exceeds_limit(state->text.size, deferred_size,
                                      config->max_text_bytes)) {
        xml_set_error(error, ROTMOS_INVALID, "XML text exceeds max_text_bytes");
        return ROTMOS_INVALID;
    }
    lonejson_error_init(&json_error);
    json_status = lonejson_spooled_rewind(&state->deferred_spool, &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
        return rotmos_xml_lonejson_error(error, json_status, &json_error,
                                         "failed to rewind deferred XML text");
    }
    for (;;) {
        read_result = lonejson_spooled_read(&state->deferred_spool, buffer,
                                            sizeof(buffer));
        if (read_result.error_code != 0) {
            xml_set_error(error, ROTMOS_STATE,
                          "failed to read deferred XML text");
            return ROTMOS_STATE;
        }
        if (read_result.bytes_read > 0u) {
            status =
                rotmos_xml_set_spooled_field(field, out, (const char *)buffer,
                                             read_result.bytes_read, error);
            if (status != ROTMOS_OK) {
                return status;
            }
            state->text.size += read_result.bytes_read;
        }
        if (read_result.eof) {
            break;
        }
        if (read_result.would_block || read_result.bytes_read == 0u) {
            xml_set_error(error, ROTMOS_STATE,
                          "deferred XML text source did not complete");
            return ROTMOS_STATE;
        }
    }
    lonejson_spooled_reset(&state->deferred_spool);
    state->text_deferred = 0;
    state->text_deferred_overflow = 0;
    return ROTMOS_OK;
}

static rotmos_status rotmos_xml_stream_scalar_content(
    lonejson *runtime, const lonejson_map *map, xmlTextReaderPtr reader,
    const lonejson_field *field, const rotmos_xml_config *config, void *dst,
    rotmos_error *error)
{
    rotmos_string_builder text;
    const xmlChar *xml_value;
    const char *chunk;
    size_t chunk_size;
    int start_depth;
    int type;
    int rc;
    rotmos_status status;

    text.data = NULL;
    text.size = 0u;
    text.capacity = 0u;
    status = ROTMOS_OK;
    if (rotmos_xml_field_is_direct_text_stream(field) && config->trim_text) {
        xml_set_error(error, ROTMOS_INVALID,
                      "XML spooled lonejson fields require trim_text=0 for "
                      "true streaming");
        return ROTMOS_INVALID;
    }
    if (xmlTextReaderIsEmptyElement(reader)) {
        status = rotmos_xml_field_is_direct_text_stream(field)
                     ? rotmos_xml_set_spooled_field(field, dst, "", 0u, error)
                     : rotmos_xml_set_lonejson_scalar_field(
                           runtime, map, field, dst, "", 0u, config, error);
        rotmos_string_builder_cleanup(&text);
        return status;
    }
    start_depth = xmlTextReaderDepth(reader);
    for (;;) {
        rc = xmlTextReaderRead(reader);
        if (rc != 1) {
            xml_set_error(error, ROTMOS_INVALID,
                          rc == 0 ? "unexpected end of XML while reading scalar"
                                  : "libxml2 failed while reading XML scalar");
            status = ROTMOS_INVALID;
            break;
        }
        type = xmlTextReaderNodeType(reader);
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth(reader) == start_depth) {
            break;
        }
        if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
             type == XML_READER_TYPE_DOCUMENT_TYPE)) {
            status = rotmos_xml_reject_entity_reference(error);
            break;
        }
        if (type == XML_READER_TYPE_ELEMENT) {
            xml_set_error(error, ROTMOS_INVALID,
                          "XML scalar field contains nested elements");
            status = ROTMOS_INVALID;
            break;
        }
        if (type == XML_READER_TYPE_TEXT || type == XML_READER_TYPE_CDATA ||
            type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
            type == XML_READER_TYPE_WHITESPACE) {
            xml_value = xmlTextReaderConstValue(reader);
            if (xml_value != NULL) {
                chunk = (const char *)xml_value;
                chunk_size = strlen(chunk);
                if (rotmos_xml_text_exceeds_limit(text.size, chunk_size,
                                                  config->max_text_bytes)) {
                    xml_set_error(error, ROTMOS_INVALID,
                                  "XML text exceeds max_text_bytes");
                    status = ROTMOS_INVALID;
                    break;
                }
                if (rotmos_xml_field_is_direct_text_stream(field)) {
                    text.size += chunk_size;
                    status = rotmos_xml_set_spooled_field(field, dst, chunk,
                                                          chunk_size, error);
                    if (status != ROTMOS_OK) {
                        break;
                    }
                } else {
                    if (rotmos_string_builder_append_n(&text, chunk, chunk_size,
                                                       error) != ROTMOS_OK) {
                        status = error != NULL ? error->status : ROTMOS_NOMEM;
                        break;
                    }
                }
            }
        }
    }
    if (!rotmos_xml_field_is_direct_text_stream(field) && status == ROTMOS_OK) {
        status = rotmos_xml_set_lonejson_scalar_field(
            runtime, map, field, dst, text.data != NULL ? text.data : "",
            text.size, config, error);
    }
    rotmos_string_builder_cleanup(&text);
    return status;
}

static rotmos_status rotmos_xml_stream_array_scalar_content(
    lonejson *runtime, const lonejson_map *map, xmlTextReaderPtr reader,
    const lonejson_field *field, const rotmos_xml_config *config, void *out,
    rotmos_error *error)
{
    rotmos_string_builder text;
    const xmlChar *xml_value;
    const char *chunk;
    size_t chunk_size;
    int start_depth;
    int type;
    int rc;
    rotmos_status status;

    text.data = NULL;
    text.size = 0u;
    text.capacity = 0u;
    status = ROTMOS_OK;
    if (xmlTextReaderIsEmptyElement(reader)) {
        status = rotmos_xml_set_array_scalar_field(
            runtime, map, out, field,
            (unsigned char *)out + field->struct_offset, "", 0u, config, error);
        rotmos_string_builder_cleanup(&text);
        return status;
    }
    start_depth = xmlTextReaderDepth(reader);
    for (;;) {
        rc = xmlTextReaderRead(reader);
        if (rc != 1) {
            xml_set_error(error, ROTMOS_INVALID,
                          rc == 0 ? "unexpected end of XML while reading scalar"
                                  : "libxml2 failed while reading XML scalar");
            status = ROTMOS_INVALID;
            break;
        }
        type = xmlTextReaderNodeType(reader);
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth(reader) == start_depth) {
            break;
        }
        if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
             type == XML_READER_TYPE_DOCUMENT_TYPE)) {
            status = rotmos_xml_reject_entity_reference(error);
            break;
        }
        if (type == XML_READER_TYPE_ELEMENT) {
            xml_set_error(error, ROTMOS_INVALID,
                          "XML scalar field contains nested elements");
            status = ROTMOS_INVALID;
            break;
        }
        if (type == XML_READER_TYPE_TEXT || type == XML_READER_TYPE_CDATA ||
            type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
            type == XML_READER_TYPE_WHITESPACE) {
            xml_value = xmlTextReaderConstValue(reader);
            if (xml_value != NULL) {
                chunk = (const char *)xml_value;
                chunk_size = strlen(chunk);
                if (rotmos_xml_text_exceeds_limit(text.size, chunk_size,
                                                  config->max_text_bytes)) {
                    xml_set_error(error, ROTMOS_INVALID,
                                  "XML text exceeds max_text_bytes");
                    status = ROTMOS_INVALID;
                    break;
                }
                if (rotmos_string_builder_append_n(&text, chunk, chunk_size,
                                                   error) != ROTMOS_OK) {
                    status = error != NULL ? error->status : ROTMOS_NOMEM;
                    break;
                }
            }
        }
    }
    if (status == ROTMOS_OK) {
        status = rotmos_xml_set_array_scalar_field(
            runtime, map, out, field,
            (unsigned char *)out + field->struct_offset,
            text.data != NULL ? text.data : "", text.size, config, error);
    }
    rotmos_string_builder_cleanup(&text);
    return status;
}

static rotmos_status rotmos_xml_stream_field_value(
    lonejson *runtime, const lonejson_map *map, xmlTextReaderPtr reader,
    const lonejson_field *field, const rotmos_xml_config *config, void *out,
    size_t depth, int parent_space_preserve, rotmos_error *error)
{
    void *field_value;
    void *item;

    if (depth > config->max_depth) {
        xml_set_error(error, ROTMOS_INVALID, "XML nesting exceeds max_depth");
        return ROTMOS_INVALID;
    }
    field_value = (unsigned char *)out + field->struct_offset;
    if (rotmos_xml_field_is_direct_text_stream(field) &&
        (field->flags & LONEJSON_FIELD_HAS_PRESENCE) != 0u) {
        xml_set_error(error, ROTMOS_INVALID,
                      "XML spooled lonejson fields do not support presence "
                      "flags");
        return ROTMOS_INVALID;
    }
    if (field->kind == LONEJSON_FIELD_KIND_OBJECT) {
        if (field->submap == NULL) {
            xml_set_error(error, ROTMOS_INVALID,
                          "XML object field requires a submap");
            return ROTMOS_INVALID;
        }
        return rotmos_xml_stream_object(reader, field->submap, config, runtime,
                                        field_value, depth,
                                        parent_space_preserve, error);
    }
    if (field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY) {
        if (rotmos_xml_append_object_array_item(runtime, map, out, field,
                                                field_value, &item,
                                                error) != ROTMOS_OK) {
            return error != NULL ? error->status : ROTMOS_INVALID;
        }
        return rotmos_xml_stream_object(reader, field->submap, config, runtime,
                                        item, depth, parent_space_preserve,
                                        error);
    }
    if (rotmos_xml_field_is_array(field)) {
        return rotmos_xml_stream_array_scalar_content(
            runtime, map, reader, field, config, out, error);
    }
    return rotmos_xml_stream_scalar_content(runtime, map, reader, field, config,
                                            out, error);
}

static rotmos_status rotmos_xml_stream_text_field(
    lonejson *runtime, const lonejson_map *map, rotmos_xml_field_state *states,
    size_t *open_index, const rotmos_xml_config *config, void *out,
    int has_element_children, int space_preserve, int node_type,
    const char *data, size_t size, rotmos_error *error)
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
    rotmos_status status;

    if (config->text_key == NULL || config->text_key[0] == '\0') {
        return ROTMOS_OK;
    }
    text = data;
    text_size = size;
    text_is_cdata = node_type == XML_READER_TYPE_CDATA;
    field = rotmos_xml_find_field(map, config->text_key);
    if (field == NULL) {
        if (!config->skip_unknown_disabled) {
            return ROTMOS_OK;
        }
        trimmed_text = text;
        trimmed_size = text_size;
        rotmos_xml_trim_span(&trimmed_text, &trimmed_size);
        if (trimmed_size == 0u && !text_is_cdata && !space_preserve) {
            return ROTMOS_OK;
        }
        xml_set_errorf(error, ROTMOS_INVALID, "unknown XML text field '%s'",
                       config->text_key);
        return ROTMOS_INVALID;
    }
    trimmed_text = text;
    trimmed_size = text_size;
    rotmos_xml_trim_span(&trimmed_text, &trimmed_size);
    text_whitespace_only = trimmed_size == 0u;
    if (rotmos_xml_field_is_direct_text_stream(field) &&
        (field->flags & LONEJSON_FIELD_HAS_PRESENCE) != 0u) {
        xml_set_error(error, ROTMOS_INVALID,
                      "XML spooled lonejson fields do not support presence "
                      "flags");
        return ROTMOS_INVALID;
    }
    if (rotmos_xml_field_is_direct_text_stream(field) && config->trim_text) {
        xml_set_error(error, ROTMOS_INVALID,
                      "XML spooled lonejson fields require trim_text=0 for "
                      "true streaming");
        return ROTMOS_INVALID;
    }
    if (config->trim_text && rotmos_xml_field_is_array(field)) {
        if (trimmed_size == 0u) {
            return ROTMOS_OK;
        }
        text = trimmed_text;
        text_size = trimmed_size;
    }
    if (text_size == 0u) {
        return ROTMOS_OK;
    }
    index = (size_t)(field - map->fields);
    if (text_whitespace_only && !text_is_cdata && !space_preserve &&
        has_element_children && !states[index].text_pending &&
        (!rotmos_xml_field_is_array(field) ||
         !states[index].text_has_content)) {
        return ROTMOS_OK;
    }
    if (!(text_whitespace_only && !text_is_cdata && !space_preserve &&
          !states[index].text_pending) &&
        rotmos_xml_text_exceeds_limit(0u, text_size, config->max_text_bytes)) {
        xml_set_error(error, ROTMOS_INVALID, "XML text exceeds max_text_bytes");
        return ROTMOS_INVALID;
    }
    if (text_whitespace_only && !text_is_cdata && !space_preserve &&
        !states[index].text_pending && rotmos_xml_field_is_array(field)) {
        status = rotmos_xml_append_deferred_text(&states[index], field, text,
                                                 text_size,
                                                 config->max_text_bytes, error);
        if (status != ROTMOS_OK) {
            return status;
        }
        states[index].text_deferred = 1;
        return ROTMOS_OK;
    }
    if (!rotmos_xml_field_is_array(field)) {
        if (text_whitespace_only && !text_is_cdata && !space_preserve &&
            states[index].count > 0u && !states[index].text_pending) {
            return ROTMOS_OK;
        }
        if (text_whitespace_only && !text_is_cdata && !space_preserve &&
            !states[index].text_pending) {
            status = rotmos_xml_append_deferred_text(
                &states[index], field, text, text_size, config->max_text_bytes,
                error);
            if (status != ROTMOS_OK) {
                return status;
            }
            states[index].text_deferred = 1;
            return ROTMOS_OK;
        }
        if (!states[index].text_pending) {
            if (states[index].text_deferred_overflow) {
                xml_set_error(error, ROTMOS_INVALID,
                              "XML text exceeds max_text_bytes");
                return ROTMOS_INVALID;
            }
            status =
                rotmos_xml_begin_field(states, open_index, index, field, error);
            if (status != ROTMOS_OK) {
                return status;
            }
            if (states[index].text_deferred &&
                rotmos_xml_field_is_direct_text_stream(field)) {
                status = rotmos_xml_commit_deferred_text_stream(
                    &states[index], field, out, config, error);
                if (status != ROTMOS_OK) {
                    return status;
                }
            }
        }
        if (rotmos_xml_text_exceeds_limit(states[index].text.size, text_size,
                                          config->max_text_bytes)) {
            xml_set_error(error, ROTMOS_INVALID,
                          "XML text exceeds max_text_bytes");
            return ROTMOS_INVALID;
        }
        if (rotmos_xml_field_is_direct_text_stream(field)) {
            status = rotmos_xml_set_spooled_field(field, out, text, text_size,
                                                  error);
            if (status == ROTMOS_OK) {
                states[index].text.size += text_size;
            }
        } else {
            status = rotmos_string_builder_append_n(&states[index].text, text,
                                                    text_size, error);
        }
        if (status != ROTMOS_OK) {
            return status;
        }
        states[index].text_pending = 1;
        states[index].text_deferred = 0;
        return ROTMOS_OK;
    }
    if (states[index].text_deferred) {
        if (states[index].text_deferred_overflow ||
            rotmos_xml_text_exceeds_limit(states[index].text.size, 0u,
                                          config->max_text_bytes)) {
            xml_set_error(error, ROTMOS_INVALID,
                          "XML text exceeds max_text_bytes");
            return ROTMOS_INVALID;
        }
        status =
            rotmos_xml_begin_field(states, open_index, index, field, error);
        if (status != ROTMOS_OK) {
            return status;
        }
        field_value = (unsigned char *)out + field->struct_offset;
        status = rotmos_xml_set_array_scalar_field(
            runtime, map, out, field, field_value,
            states[index].text.data != NULL ? states[index].text.data : "",
            states[index].text.size, config, error);
        if (status != ROTMOS_OK) {
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
    status = rotmos_xml_begin_field(states, open_index, index, field, error);
    if (status != ROTMOS_OK) {
        return status;
    }
    field_value = (unsigned char *)out + field->struct_offset;
    status = rotmos_xml_set_array_scalar_field(
        runtime, map, out, field, field_value, text, text_size, config, error);
    if (status == ROTMOS_OK) {
        states[index].text_has_content = 1;
    }
    return status;
}

static rotmos_status
rotmos_xml_stream_object(xmlTextReaderPtr reader, const lonejson_map *map,
                         const rotmos_xml_config *config, lonejson *runtime,
                         void *out, size_t depth, int parent_space_preserve,
                         rotmos_error *error)
{
    rotmos_xml_field_state *states;
    rotmos_string_builder attr_key;
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
    rotmos_status status;

    if (depth > config->max_depth) {
        xml_set_error(error, ROTMOS_INVALID, "XML nesting exceeds max_depth");
        return ROTMOS_INVALID;
    }
    if (map == NULL) {
        xml_set_error(error, ROTMOS_INVALID, "XML lonejson map is required");
        return ROTMOS_INVALID;
    }
    states =
        (rotmos_xml_field_state *)calloc(map->field_count, sizeof(states[0]));
    if (states == NULL && map->field_count > 0u) {
        xml_set_error(error, ROTMOS_NOMEM,
                      "failed to allocate XML field state");
        return ROTMOS_NOMEM;
    }
    for (i = 0u; i < map->field_count; ++i) {
        states[i].array = rotmos_xml_field_is_array(&map->fields[i]);
        if (rotmos_xml_field_is_direct_text_stream(&map->fields[i])) {
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
    space_preserve = parent_space_preserve;
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
    status = ROTMOS_OK;
    if (xmlTextReaderMoveToFirstAttribute(reader) == 1) {
        do {
            if (xmlTextReaderIsNamespaceDecl(reader) == 1)
                continue;
            name = rotmos_xml_reader_local_name(reader);
            if (name == NULL) {
                continue;
            }
            field = rotmos_xml_find_attribute_field(map, config, name,
                                                    &attr_key, error);
            if (field == NULL) {
                if (error != NULL && error->status != ROTMOS_OK) {
                    status = error->status;
                    goto cleanup;
                }
                if (!config->skip_unknown_disabled) {
                    continue;
                }
                xml_set_errorf(error, ROTMOS_INVALID,
                               "unknown XML attribute '%s'", name);
                status = ROTMOS_INVALID;
                goto cleanup;
            }
            index = (size_t)(field - map->fields);
            status = rotmos_xml_begin_field(states, &open_index, index, field,
                                            error);
            if (status != ROTMOS_OK) {
                goto cleanup;
            }
            value = xmlTextReaderConstValue(reader);
            if (rotmos_xml_text_exceeds_limit(
                    0u, value != NULL ? strlen((const char *)value) : 0u,
                    config->max_text_bytes)) {
                xml_set_error(error, ROTMOS_INVALID,
                              "XML text exceeds max_text_bytes");
                status = ROTMOS_INVALID;
                goto cleanup;
            }
            if (rotmos_xml_field_is_array(field)) {
                status = rotmos_xml_set_array_scalar_field(
                    runtime, map, out, field,
                    (unsigned char *)out + field->struct_offset,
                    value != NULL ? (const char *)value : "",
                    value != NULL ? strlen((const char *)value) : 0u, config,
                    error);
            } else {
                status = rotmos_xml_set_lonejson_scalar_field(
                    runtime, map, field, out,
                    value != NULL ? (const char *)value : "",
                    value != NULL ? strlen((const char *)value) : 0u, config,
                    error);
            }
            if (status != ROTMOS_OK) {
                goto cleanup;
            }
        } while (xmlTextReaderMoveToNextAttribute(reader) == 1);
        (void)xmlTextReaderMoveToElement(reader);
    }
    if (!empty) {
        for (;;) {
            rc = xmlTextReaderRead(reader);
            if (rc != 1) {
                xml_set_error(error, ROTMOS_INVALID,
                              rc == 0
                                  ? "unexpected end of XML object"
                                  : "libxml2 failed while reading XML object");
                status = ROTMOS_INVALID;
                goto cleanup;
            }
            type = xmlTextReaderNodeType(reader);
            if (type == XML_READER_TYPE_END_ELEMENT &&
                xmlTextReaderDepth(reader) == start_depth) {
                break;
            }
            if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
                 type == XML_READER_TYPE_DOCUMENT_TYPE)) {
                status = rotmos_xml_reject_entity_reference(error);
                goto cleanup;
            }
            if (type == XML_READER_TYPE_ELEMENT) {
                has_element_children = 1;
                if (!space_preserve && config->text_key != NULL &&
                    config->text_key[0] != '\0') {
                    text_field = rotmos_xml_find_field(map, config->text_key);
                    if (text_field != NULL) {
                        index = (size_t)(text_field - map->fields);
                        if (rotmos_xml_field_is_array(text_field) &&
                            states[index].text_has_content &&
                            states[index].text_deferred) {
                            if (states[index].text_deferred_overflow ||
                                rotmos_xml_text_exceeds_limit(
                                    states[index].text.size, 0u,
                                    config->max_text_bytes)) {
                                xml_set_error(
                                    error, ROTMOS_INVALID,
                                    "XML text exceeds max_text_bytes");
                                status = ROTMOS_INVALID;
                                goto cleanup;
                            }
                            status = rotmos_xml_begin_field(
                                states, &open_index, index, text_field, error);
                            if (status != ROTMOS_OK) {
                                goto cleanup;
                            }
                            status = rotmos_xml_set_array_scalar_field(
                                runtime, map, out, text_field,
                                (unsigned char *)out +
                                    text_field->struct_offset,
                                states[index].text.data != NULL
                                    ? states[index].text.data
                                    : "",
                                states[index].text.size, config, error);
                            if (status != ROTMOS_OK) {
                                goto cleanup;
                            }
                            states[index].text.size = 0u;
                            if (states[index].text.data != NULL) {
                                states[index].text.data[0] = '\0';
                            }
                            states[index].text_deferred = 0;
                            states[index].text_deferred_overflow = 0;
                        } else {
                            rotmos_xml_discard_deferred_text(states, index,
                                                             text_field, out);
                        }
                    }
                }
                name = rotmos_xml_reader_local_name(reader);
                field = rotmos_xml_find_field(map, name);
                if (field == NULL) {
                    if (config->skip_unknown_disabled) {
                        xml_set_errorf(error, ROTMOS_INVALID,
                                       "unknown XML element '%s'", name);
                        status = ROTMOS_INVALID;
                        goto cleanup;
                    }
                    status = rotmos_xml_skip_element(reader, config->max_depth,
                                                     error);
                    if (status != ROTMOS_OK) {
                        goto cleanup;
                    }
                    continue;
                }
                index = (size_t)(field - map->fields);
                status = rotmos_xml_begin_field(states, &open_index, index,
                                                field, error);
                if (status != ROTMOS_OK) {
                    goto cleanup;
                }
                status = rotmos_xml_stream_field_value(
                    runtime, map, reader, field, config, out, depth + 1u,
                    space_preserve, error);
                if (status != ROTMOS_OK) {
                    goto cleanup;
                }
            } else if (type == XML_READER_TYPE_TEXT ||
                       type == XML_READER_TYPE_CDATA ||
                       type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
                       type == XML_READER_TYPE_WHITESPACE) {
                value = xmlTextReaderConstValue(reader);
                if (value != NULL) {
                    status = rotmos_xml_stream_text_field(
                        runtime, map, states, &open_index, config, out,
                        has_element_children, space_preserve, type,
                        (const char *)value, strlen((const char *)value),
                        error);
                    if (status != ROTMOS_OK) {
                        goto cleanup;
                    }
                }
            }
        }
    }
    for (i = 0u; i < map->field_count; ++i) {
        if (states[i].text_deferred) {
            if (states[i].text_deferred_overflow ||
                (!rotmos_xml_field_is_direct_text_stream(&map->fields[i]) &&
                 rotmos_xml_text_exceeds_limit(states[i].text.size, 0u,
                                               config->max_text_bytes))) {
                xml_set_error(error, ROTMOS_INVALID,
                              "XML text exceeds max_text_bytes");
                status = ROTMOS_INVALID;
                goto cleanup;
            }
            status = rotmos_xml_begin_field(states, &open_index, i,
                                            &map->fields[i], error);
            if (status != ROTMOS_OK) {
                goto cleanup;
            }
            if (rotmos_xml_field_is_array(&map->fields[i])) {
                status = rotmos_xml_set_array_scalar_field(
                    runtime, map, out, &map->fields[i],
                    (unsigned char *)out + map->fields[i].struct_offset,
                    states[i].text.data != NULL ? states[i].text.data : "",
                    states[i].text.size, config, error);
                if (status != ROTMOS_OK) {
                    goto cleanup;
                }
                states[i].text_has_content = 1;
                states[i].text.size = 0u;
                if (states[i].text.data != NULL) {
                    states[i].text.data[0] = '\0';
                }
            } else {
                if (rotmos_xml_field_is_direct_text_stream(&map->fields[i])) {
                    status = rotmos_xml_commit_deferred_text_stream(
                        &states[i], &map->fields[i], out, config, error);
                    if (status != ROTMOS_OK) {
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
        if (rotmos_xml_field_is_direct_text_stream(&map->fields[i])) {
            if (rotmos_xml_text_exceeds_limit(states[i].text.size, 0u,
                                              config->max_text_bytes)) {
                xml_set_error(error, ROTMOS_INVALID,
                              "XML text exceeds max_text_bytes");
                status = ROTMOS_INVALID;
                goto cleanup;
            }
            continue;
        }
        status = rotmos_xml_set_lonejson_scalar_field(
            runtime, map, &map->fields[i], out,
            states[i].text.data != NULL ? states[i].text.data : "",
            states[i].text.size, config, error);
        if (status != ROTMOS_OK) {
            goto cleanup;
        }
    }
    status = rotmos_xml_finish_array(states, &open_index);
    if (status != ROTMOS_OK) {
        goto cleanup;
    }
    for (i = 0u; i < map->field_count; ++i) {
        if ((map->fields[i].flags & LONEJSON_FIELD_REQUIRED) != 0u &&
            states[i].count == 0u) {
            xml_set_errorf(error, ROTMOS_INVALID,
                           "missing required XML field '%s'",
                           map->fields[i].json_key);
            status = ROTMOS_INVALID;
            goto cleanup;
        }
    }

cleanup:
    rotmos_string_builder_cleanup(&attr_key);
    for (i = 0u; i < map->field_count; ++i) {
        rotmos_string_builder_cleanup(&states[i].text);
        if (states[i].deferred_spool_initialized) {
            lonejson_spooled_cleanup(&states[i].deferred_spool);
        }
    }
    free(states);
    return status;
}

static rotmos_status rotmos_xml_stream_document(xmlTextReaderPtr reader,
                                                const lonejson_map *map,
                                                const rotmos_xml_config *config,
                                                lonejson *runtime, void *out,
                                                rotmos_error *error)
{
    const char *name;
    const xmlChar *value;
    const char *text;
    size_t text_size;
    int type;
    int rc;
    rotmos_status status;

    for (;;) {
        rc = xmlTextReaderRead(reader);
        if (rc != 1) {
            xml_set_error(error, ROTMOS_INVALID,
                          "XML document has no root element");
            return ROTMOS_INVALID;
        }
        type = xmlTextReaderNodeType(reader);
        if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
             type == XML_READER_TYPE_DOCUMENT_TYPE)) {
            return rotmos_xml_reject_entity_reference(error);
        }
        if (type == XML_READER_TYPE_ELEMENT) {
            break;
        }
    }
    name = rotmos_xml_reader_local_name(reader);
    if (config->root_element != NULL &&
        !rotmos_xml_name_matches(name, config->root_element)) {
        xml_set_errorf(error, ROTMOS_INVALID,
                       "XML root element '%s' does not match expected '%s'",
                       name != NULL ? name : "", config->root_element);
        return ROTMOS_INVALID;
    }
    status = rotmos_xml_stream_object(reader, map, config, runtime, out, 1u, 0,
                                      error);
    if (status != ROTMOS_OK) {
        return status;
    }
    for (;;) {
        rc = xmlTextReaderRead(reader);
        if (rc == 0) {
            return ROTMOS_OK;
        }
        if (rc < 0) {
            xml_set_error(error, ROTMOS_INVALID,
                          "libxml2 failed while reading after XML root");
            return ROTMOS_INVALID;
        }
        type = xmlTextReaderNodeType(reader);
        if ((type == XML_READER_TYPE_ENTITY_REFERENCE ||
             type == XML_READER_TYPE_DOCUMENT_TYPE)) {
            return rotmos_xml_reject_entity_reference(error);
        }
        if (type == XML_READER_TYPE_ELEMENT) {
            xml_set_error(error, ROTMOS_INVALID,
                          "XML document contains multiple root elements");
            return ROTMOS_INVALID;
        }
        if (type != XML_READER_TYPE_TEXT && type != XML_READER_TYPE_CDATA &&
            type != XML_READER_TYPE_SIGNIFICANT_WHITESPACE &&
            type != XML_READER_TYPE_WHITESPACE) {
            continue;
        }
        value = xmlTextReaderConstValue(reader);
        text = value != NULL ? (const char *)value : "";
        text_size = value != NULL ? strlen(text) : 0u;
        rotmos_xml_trim_span(&text, &text_size);
        if (text_size != 0u) {
            xml_set_error(error, ROTMOS_INVALID,
                          "XML document has non-whitespace content after root");
            return ROTMOS_INVALID;
        }
    }
}

struct xml_input {
    rotmos_source *source;
    rotmos_error failure;
};
static int xml_input_read(void *context, char *bytes, int capacity)
{
    struct xml_input *input = context;
    rotmos_status status;
    size_t produced = 0;
    if (input->failure.status != ROTMOS_OK)
        return -1;
    if (capacity <= 0)
        return 0;
    status = rotmos_source_read(input->source, bytes, (size_t)capacity,
                                &produced, &input->failure);
    if (status == ROTMOS_EOF)
        return 0;
    if (status == ROTMOS_AGAIN) {
        xml_set_error(&input->failure, ROTMOS_STATE,
                      "mapped XML requires a synchronous completed source");
        return -1;
    }
    if (status != ROTMOS_OK)
        return -1;
    return (int)produced;
}
static void xml_input_error(void *context, const xmlError *native)
{
    struct xml_input *input = context;
    if (native == NULL || native->level < XML_ERR_ERROR ||
        input->failure.status != ROTMOS_OK)
        return;
    xml_set_error(
        &input->failure,
        native->code == XML_ERR_NO_MEMORY ? ROTMOS_NOMEM : ROTMOS_INVALID,
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
    xml_set_error(&input->failure, ROTMOS_INVALID,
                  "external XML resources are disabled");
    return XML_IO_LOAD_ERROR;
}
rotmos_status rotmos_xml_parse_source(rotmos_source *source,
                                      const lonejson_schema_view *schema,
                                      const rotmos_xml_config *config,
                                      void *record, rotmos_error *error)
{
    rotmos_xml_config effective;
    struct xml_input input;
    xmlTextReaderPtr reader;
    lonejson *runtime;
    rotmos_status status;
    int options, owned;
    rotmos_error_clear(error);
    if (source == NULL || record == NULL)
        return xml_set_error(error, ROTMOS_INVALID,
                             "source and initialized record are required");
    status = rotmos_json_schema_check(schema, error);
    if (status != ROTMOS_OK)
        return status;
    status = rotmos_xml_effective_config(config, &effective, error);
    if (status != ROTMOS_OK)
        return status;
    runtime = schema->runtime;
    owned = runtime == NULL;
    if (owned)
        runtime = rotmos_xml_lonejson_new(&effective, error);
    if (runtime == NULL)
        return error != NULL ? error->status : ROTMOS_NOMEM;
    lonejson_reset(runtime, schema->map, record);
    memset(&input, 0, sizeof(input));
    input.source = source;
    options = XML_PARSE_NONET | XML_PARSE_COMPACT;
    if (effective.max_text_bytes >= 10000000u)
        options |= XML_PARSE_HUGE;
    reader = xmlReaderForIO(xml_input_read, NULL, &input, NULL, NULL, options);
    if (reader == NULL)
        status = xml_set_error(error, ROTMOS_NOMEM, "creating XML reader");
    else {
        xmlTextReaderSetStructuredErrorHandler(reader, xml_input_error, &input);
        xmlTextReaderSetResourceLoader(reader, xml_input_no_resource, &input);
        status = rotmos_xml_stream_document(reader, schema->map, &effective,
                                            runtime, record, error);
        xmlFreeTextReader(reader);
    }
    if (input.failure.status != ROTMOS_OK) {
        status = input.failure.status;
        if (error != NULL)
            *error = input.failure;
    }
    if (status == ROTMOS_LIMIT || status == ROTMOS_OVERFLOW) {
        status = ROTMOS_INVALID;
        if (error != NULL)
            error->status = status;
    }
    if (status != ROTMOS_OK)
        lonejson_reset(runtime, schema->map, record);
    if (owned)
        lonejson_free(runtime);
    if (status == ROTMOS_OK)
        rotmos_error_clear(error);
    return status;
}
static rotmos_status xml_body_read(void *bytes, size_t capacity,
                                   size_t *produced, void *data,
                                   rotmos_error *error)
{
    return rotmos_body_read(data, bytes, capacity, produced, error);
}
rotmos_status rotmos_request_xml_into(rotmos_request *request,
                                      const lonejson_schema_view *schema,
                                      const rotmos_xml_config *config,
                                      void *record, rotmos_error *error)
{
    rotmos_body *body;
    rotmos_source *source;
    rotmos_source_config input;
    rotmos_status status, rewind_status;
    rotmos_error rewind_error;
    body = rotmos_request_body(request);
    if (body == NULL || rotmos_body_kind(body) == ROTMOS_BODY_STORAGE_NONE)
        return xml_set_error(
            error, ROTMOS_STATE,
            "XML mapping needs a completed BUFFERED or MATERIALIZED body");
    status = rotmos_body_rewind(body, error);
    if (status != ROTMOS_OK)
        return status;
    rotmos_source_config_init(&input);
    input.read = xml_body_read;
    input.userdata = body;
    status = rotmos_source_new(&input, &source, error);
    if (status == ROTMOS_OK) {
        status = rotmos_xml_parse_source(source, schema, config, record, error);
        rotmos_source_destroy(source, NULL);
    }
    rewind_status = rotmos_body_rewind(body, &rewind_error);
    if (status == ROTMOS_OK && rewind_status != ROTMOS_OK) {
        status = rewind_status;
        if (error != NULL)
            *error = rewind_error;
    }
    return status;
}
