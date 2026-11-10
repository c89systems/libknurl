#include <libxml/entities.h>
#include <libxml/parser.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <knurl/xml.h>
#include "xml_internal.h"
#include "internal.h"
typedef struct xml_sax_state {
    knurl_xml_sax receiver;
    xmlSAXHandler handler;
    xmlParserCtxt *native;
    knurl_xml_sax_callbacks callbacks;
    void *context;
    knurl_xml_attribute attributes[KNURL_XML_SAX_MAX_ATTRIBUTES];
    knurl_xml_namespace namespaces[KNURL_XML_SAX_MAX_ATTRIBUTES];
    size_t metadata_limit;
    knurl_error failure;
    int busy, violated, finished;
    pid_t process;
    pthread_t thread;
} xml_sax_state;
static knurl_status sax_failure(knurl_error *error, knurl_status status,
                                const char *message)
{
    (void)knurl_fail(error, status, "xml_sax", message);
    if (error != NULL)
        error->source = KNURL_ERROR_XML;
    return status;
}
static void sax_stop(xml_sax_state *state, knurl_status status,
                     const char *message)
{
    if (state->failure.status == KNURL_OK)
        sax_failure(&state->failure, status, message);
    xmlStopParser(state->native);
}
static void sax_native_error(void *context, const xmlError *native)
{
    xml_sax_state *state = (xml_sax_state *)context;
    if (native == NULL || native->level < XML_ERR_ERROR ||
        state->failure.status != KNURL_OK)
        return;
    sax_failure(&state->failure,
                native->code == XML_ERR_NO_MEMORY ? KNURL_NOMEM : KNURL_STATE,
                native->message == NULL ? "XML SAX parse failed"
                                        : native->message);
    state->failure.native_code = native->code;
    (void)snprintf(state->failure.detail, sizeof(state->failure.detail),
                   "line %d, column %d", native->line, native->int2);
}
static xmlParserErrors sax_no_resource(void *context, const char *url,
                                       const char *public_id,
                                       xmlResourceType type,
                                       xmlParserInputFlags flags,
                                       xmlParserInput **out)
{
    xml_sax_state *state = (xml_sax_state *)context;
    (void)url;
    (void)public_id;
    (void)type;
    (void)flags;
    *out = NULL;
    sax_stop(state, KNURL_INVALID, "external XML resources are disabled");
    return XML_IO_LOAD_ERROR;
}
static void sax_doctype(void *context, const xmlChar *name,
                        const xmlChar *external_id, const xmlChar *system_id)
{
    (void)name;
    (void)external_id;
    (void)system_id;
    sax_stop((xml_sax_state *)context, KNURL_INVALID,
             "DTD/entity declarations are outside this SAX surface");
}
static xmlEntity *sax_entity(void *context, const xmlChar *name)
{
    (void)context;
    return xmlGetPredefinedEntity(name);
}
static int sax_metadata(xml_sax_state *state, size_t *total, const void *data,
                        size_t size)
{
    (void)data;
    if (size > state->metadata_limit - *total) {
        sax_stop(state, KNURL_INVALID, "SAX event exceeds max_metadata_bytes");
        return 0;
    }
    *total += size;
    return 1;
}
static int sax_metadata_string(xml_sax_state *state, size_t *total,
                               const xmlChar *value)
{
    return sax_metadata(state, total, value,
                        value == NULL ? 0u : (size_t)xmlStrlen(value));
}
static void sax_result(xml_sax_state *state, knurl_status status,
                       const knurl_error *error)
{
    if (status != KNURL_OK && state->failure.status == KNURL_OK) {
        if (status <= KNURL_EOF || status > KNURL_BACKEND)
            status = KNURL_INVALID;
        if (error->status == status)
            state->failure = *error;
        else
            sax_failure(&state->failure, status,
                        "SAX callback must accept with OK or fail terminally");
    }
    if (state->violated && state->failure.status == KNURL_OK)
        sax_failure(&state->failure, KNURL_STATE,
                    "SAX callback attempted receiver reentry/close");
    if (state->failure.status != KNURL_OK)
        xmlStopParser(state->native);
}
static void sax_start(void *context, const xmlChar *name, const xmlChar *prefix,
                      const xmlChar *uri, int namespace_count,
                      const xmlChar **namespaces, int attribute_count,
                      int defaulted, const xmlChar **attributes)
{
    xml_sax_state *state = (xml_sax_state *)context;
    knurl_xml_name element;
    knurl_error error;
    knurl_status status;
    size_t total = 0u, size;
    int i;
    (void)defaulted;
    if (state->failure.status != KNURL_OK)
        return;
    if (namespace_count < 0 || attribute_count < 0 ||
        namespace_count > (int)KNURL_XML_SAX_MAX_ATTRIBUTES ||
        attribute_count > (int)KNURL_XML_SAX_MAX_ATTRIBUTES - namespace_count) {
        sax_stop(state, KNURL_INVALID,
                 "SAX attribute/namespace count exceeds 256");
        return;
    }
    if (!sax_metadata_string(state, &total, name) ||
        !sax_metadata_string(state, &total, prefix) ||
        !sax_metadata_string(state, &total, uri))
        return;
    element.name = (const char *)name;
    element.namespace_uri = (const char *)uri;
    element.prefix = (const char *)prefix;
    for (i = 0; i < namespace_count; ++i) {
        if (!sax_metadata_string(state, &total, namespaces[i * 2]) ||
            !sax_metadata_string(state, &total, namespaces[i * 2 + 1]))
            return;
        state->namespaces[i].prefix = (const char *)namespaces[i * 2];
        state->namespaces[i].uri = namespaces[i * 2 + 1] == NULL
                                       ? ""
                                       : (const char *)namespaces[i * 2 + 1];
    }
    for (i = 0; i < attribute_count; ++i) {
        if (attributes[i * 5 + 3] == NULL ||
            attributes[i * 5 + 4] < attributes[i * 5 + 3]) {
            sax_stop(state, KNURL_STATE, "invalid native SAX attribute view");
            return;
        }
        size = (size_t)(attributes[i * 5 + 4] - attributes[i * 5 + 3]);
        if (!sax_metadata_string(state, &total, attributes[i * 5]) ||
            !sax_metadata_string(state, &total, attributes[i * 5 + 1]) ||
            !sax_metadata_string(state, &total, attributes[i * 5 + 2]) ||
            !sax_metadata(state, &total, attributes[i * 5 + 3], size))
            return;
        state->attributes[i].name.name = (const char *)attributes[i * 5];
        state->attributes[i].name.prefix = (const char *)attributes[i * 5 + 1];
        state->attributes[i].name.namespace_uri =
            (const char *)attributes[i * 5 + 2];
        state->attributes[i].value = attributes[i * 5 + 3];
        state->attributes[i].length = size;
    }
    if (state->callbacks.start_element == NULL)
        return;
    memset(&error, 0, sizeof(error));
    status = state->callbacks.start_element(
        state->context, &element, state->attributes, (size_t)attribute_count,
        state->namespaces, (size_t)namespace_count, &error);
    sax_result(state, status, &error);
}
static void sax_end(void *context, const xmlChar *name, const xmlChar *prefix,
                    const xmlChar *uri)
{
    xml_sax_state *state = (xml_sax_state *)context;
    knurl_xml_name element;
    knurl_error error;
    knurl_status status;
    if (state->failure.status != KNURL_OK ||
        state->callbacks.end_element == NULL)
        return;
    element.name = (const char *)name;
    element.namespace_uri = (const char *)uri;
    element.prefix = (const char *)prefix;
    memset(&error, 0, sizeof(error));
    status = state->callbacks.end_element(state->context, &element, &error);
    sax_result(state, status, &error);
}
static void sax_text(void *context, const xmlChar *data, int length)
{
    xml_sax_state *state = (xml_sax_state *)context;
    knurl_error error;
    knurl_status status;
    size_t remaining, size;
    if (state->failure.status != KNURL_OK || state->callbacks.text == NULL ||
        length <= 0)
        return;
    remaining = (size_t)length;
    while (remaining != 0u && state->failure.status == KNURL_OK) {
        size = remaining > KNURL_XML_SAX_CHUNK_BYTES ? KNURL_XML_SAX_CHUNK_BYTES
                                                     : remaining;
        if (size < remaining)
            while (size != 0u && (((unsigned char)data[size] & 0xc0u) == 0x80u))
                --size;
        memset(&error, 0, sizeof(error));
        status = state->callbacks.text(state->context, data, size, &error);
        sax_result(state, status, &error);
        data += size;
        remaining -= size;
    }
}
static knurl_status sax_check(xml_sax_state *state, knurl_error *error)
{
    if (state == NULL)
        return sax_failure(error, KNURL_INVALID, "missing SAX receiver");
    if (state->process != getpid() ||
        !pthread_equal(state->thread, pthread_self()))
        return sax_failure(error, KNURL_STATE,
                           "SAX receiver belongs to another process/thread");
    if (state->busy) {
        state->violated = 1;
        return sax_failure(error, KNURL_STATE,
                           "cannot reenter or close active SAX receiver");
    }
    if (state->failure.status != KNURL_OK) {
        if (error != NULL)
            *error = state->failure;
        return state->failure.status;
    }
    return KNURL_OK;
}
knurl_status knurl_xml_sax_new(const knurl_xml_sax_callbacks *callbacks,
                               void *context, size_t metadata_limit,
                               knurl_xml_sax **out, knurl_error *error)
{
    xml_sax_state *state;
    if (out != NULL)
        *out = NULL;
    if (callbacks == NULL || out == NULL)
        return sax_failure(error, KNURL_INVALID,
                           "missing SAX callbacks/output");
    state = (xml_sax_state *)calloc(1u, sizeof(*state));
    if (state == NULL)
        return sax_failure(error, KNURL_NOMEM, "allocating SAX receiver");
    state->callbacks = *callbacks;
    state->context = context;
    state->metadata_limit =
        metadata_limit == 0u ? KNURL_XML_DEFAULT_MAX_BYTES : metadata_limit;
    state->process = getpid();
    state->thread = pthread_self();
    state->handler.initialized = XML_SAX2_MAGIC;
    state->handler.startElementNs = sax_start;
    state->handler.endElementNs = sax_end;
    state->handler.characters = sax_text;
    state->handler.cdataBlock = sax_text;
    state->handler.ignorableWhitespace = sax_text;
    state->handler.internalSubset = sax_doctype;
    state->handler.getEntity = sax_entity;
    state->native =
        xmlCreatePushParserCtxt(&state->handler, state, NULL, 0, NULL);
    if (state->native == NULL) {
        free(state);
        return sax_failure(error, KNURL_NOMEM, "allocating native SAX parser");
    }
    xmlCtxtSetErrorHandler(state->native, sax_native_error, state);
    xmlCtxtSetResourceLoader(state->native, sax_no_resource, state);
    (void)xmlCtxtUseOptions(state->native, XML_PARSE_NONET | XML_PARSE_NO_XXE |
                                               XML_PARSE_NOENT);
    state->receiver.impl = state;
    *out = &state->receiver;
    return KNURL_OK;
}
static knurl_status sax_process(knurl_xml_sax *receiver, const void *data,
                                size_t size, int finish, knurl_error *error)
{
    xml_sax_state *state =
        receiver == NULL ? NULL : (xml_sax_state *)receiver->impl;
    knurl_status status = sax_check(state, error);
    int code;
    if (status != KNURL_OK)
        return status;
    if (size > KNURL_XML_SAX_CHUNK_BYTES || (data == NULL && size != 0u))
        return sax_failure(error, KNURL_INVALID,
                           "SAX feed requires <=65536 bytes");
    if (state->finished)
        return finish
                   ? KNURL_OK
                   : sax_failure(error, KNURL_STATE, "SAX parser is finished");
    state->busy = 1;
    code = xmlParseChunk(state->native, (const char *)data, (int)size, finish);
    state->busy = 0;
    if (code != 0 && state->failure.status == KNURL_OK) {
        sax_failure(&state->failure, KNURL_STATE, "XML SAX parse failed");
        state->failure.native_code = code;
    }
    if (state->failure.status != KNURL_OK) {
        if (error != NULL)
            *error = state->failure;
        return state->failure.status;
    }
    if (finish)
        state->finished = 1;
    return KNURL_OK;
}
knurl_status knurl_xml_sax_feed(knurl_xml_sax *receiver, const void *data,
                                size_t size, knurl_error *error)
{
    return sax_process(receiver, data, size, 0, error);
}
knurl_status knurl_xml_sax_finish(knurl_xml_sax *receiver, knurl_error *error)
{
    return sax_process(receiver, NULL, 0u, 1, error);
}
knurl_status knurl_xml_sax_close(knurl_xml_sax *receiver, knurl_error *error)
{
    xml_sax_state *state;
    if (receiver == NULL)
        return KNURL_OK;
    state = (xml_sax_state *)receiver->impl;
    if (state->process != getpid() ||
        !pthread_equal(state->thread, pthread_self()))
        return sax_failure(error, KNURL_STATE,
                           "cannot close foreign-process/thread SAX receiver");
    if (state->busy) {
        state->violated = 1;
        return sax_failure(error, KNURL_STATE,
                           "cannot close active SAX receiver");
    }
    xmlFreeParserCtxt(state->native);
    free(state);
    return KNURL_OK;
}
