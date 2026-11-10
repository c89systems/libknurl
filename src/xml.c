#include <libxml/chvalid.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h> /* Installed public namespace/sort API. */
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <knurl/xml.h>
#include "xml_internal.h"
#include "internal.h"
typedef struct xml_document_state {
    xmlDoc *native;
    knurl_xml_document_config config;
    knurl_error error;
    size_t references;
    unsigned long generation;
    pid_t process;
    pthread_t thread;
} xml_document_state;
typedef struct xml_node_state {
    knurl_xml_node receiver;
    xml_document_state *owner;
    xmlNode *native;
    unsigned long generation;
} xml_node_state;
static knurl_status xml_failure(knurl_error *error, knurl_status status,
                                const char *message)
{
    (void)knurl_fail(error, status, "xml_document", message);
    if (error != NULL)
        error->source = KNURL_ERROR_XML;
    return status;
}
static void xml_native_error(void *context, const xmlError *native)
{
    xml_document_state *state = (xml_document_state *)context;
    if (native == NULL || native->level < XML_ERR_ERROR ||
        state->error.status != KNURL_OK)
        return;
    xml_failure(&state->error,
                native->code == XML_ERR_NO_MEMORY ? KNURL_NOMEM : KNURL_STATE,
                native->message == NULL ? "XML operation failed"
                                        : native->message);
    state->error.native_code = native->code;
    (void)snprintf(state->error.detail, sizeof(state->error.detail),
                   "line %d, column %d", native->line, native->int2);
}
static xmlParserErrors xml_no_resource(void *context, const char *url,
                                       const char *public_id,
                                       xmlResourceType type,
                                       xmlParserInputFlags flags,
                                       xmlParserInput **out)
{
    xml_document_state *state = (xml_document_state *)context;
    (void)url;
    (void)public_id;
    (void)type;
    (void)flags;
    *out = NULL;
    xml_failure(&state->error, KNURL_INVALID,
                "external XML resources are disabled");
    return XML_IO_LOAD_ERROR;
}
void knurl_xml_document_config_init(knurl_xml_document_config *config)
{
    if (config != NULL) {
        config->max_bytes = KNURL_XML_DEFAULT_MAX_BYTES;
        config->max_xpath_nodes = KNURL_XML_DEFAULT_MAX_XPATH_NODES;
    }
}
static void xml_unref(xml_document_state *state)
{
    if (--state->references == 0u) {
        if (state->native != NULL)
            xmlFreeDoc(state->native);
        free(state);
    }
}
static knurl_status xml_owner(xml_document_state *state, knurl_error *error)
{
    knurl_error_clear(error);
    if (state == NULL || state->native == NULL)
        return xml_failure(error, KNURL_STATE, "XML document is closed");
    if (state->process != getpid() ||
        !pthread_equal(state->thread, pthread_self()))
        return xml_failure(error, KNURL_STATE,
                           "XML document belongs to another process/thread");
    return KNURL_OK;
}
static knurl_status xml_node_check(knurl_xml_node *receiver,
                                   xml_node_state **out, knurl_error *error)
{
    xml_node_state *state =
        receiver == NULL ? NULL : (xml_node_state *)receiver->impl;
    knurl_status status;
    if (state == NULL)
        return xml_failure(error, KNURL_INVALID, "missing XML element view");
    status = xml_owner(state->owner, error);
    if (status != KNURL_OK)
        return status;
    if (state->generation != state->owner->generation)
        return xml_failure(error, KNURL_STATE,
                           "XML element view was invalidated by mutation");
    *out = state;
    return KNURL_OK;
}
static knurl_status xml_view(xml_document_state *owner, xmlNode *node,
                             knurl_xml_node **out, knurl_error *error)
{
    xml_node_state *state;
    *out = NULL;
    if (node == NULL || node->type != XML_ELEMENT_NODE)
        return xml_failure(error, KNURL_INVALID,
                           "XPath views support elements only; use scalar "
                           "functions for other node types");
    state = (xml_node_state *)calloc(1u, sizeof(*state));
    if (state == NULL)
        return xml_failure(error, KNURL_NOMEM, "allocating XML element view");
    state->owner = owner;
    ++owner->references;
    state->generation = owner->generation;
    state->native = node;
    state->receiver.impl = state;
    *out = &state->receiver;
    return KNURL_OK;
}
static knurl_status xml_copy(const void *data, size_t size, size_t limit,
                             knurl_owned_bytes *out, knurl_error *error)
{
    memset(out, 0, sizeof(*out));
    if (size > limit)
        return xml_failure(error, KNURL_INVALID,
                           "materialized XML value exceeds max_bytes");
    out->data = malloc(size == 0u ? 1u : size);
    if (out->data == NULL)
        return xml_failure(error, KNURL_NOMEM, "allocating XML byte copy");
    if (size != 0u)
        memcpy(out->data, data, size);
    out->length = size;
    return KNURL_OK;
}
knurl_status knurl_xml_document_parse(const void *data, size_t size,
                                      const knurl_xml_document_config *config,
                                      knurl_xml_document **out,
                                      knurl_error *error)
{
    knurl_xml_document_config defaults;
    xml_document_state *state;
    knurl_xml_document *receiver;
    xmlParserCtxt *parser;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    knurl_xml_document_config_init(&defaults);
    if (config == NULL)
        config = &defaults;
    if (out == NULL || data == NULL || size == 0u || config->max_bytes == 0u ||
        config->max_bytes > INT_MAX || size > config->max_bytes ||
        config->max_xpath_nodes == 0u || config->max_xpath_nodes > INT_MAX)
        return xml_failure(error, KNURL_INVALID,
                           "invalid XML input or document limits");
    state = (xml_document_state *)calloc(1u, sizeof(*state));
    receiver = (knurl_xml_document *)calloc(1u, sizeof(*receiver));
    if (state == NULL || receiver == NULL) {
        free(state);
        free(receiver);
        return xml_failure(error, KNURL_NOMEM, "allocating XML document owner");
    }
    state->config = *config;
    state->references = 1u;
    state->generation = 1u;
    state->process = getpid();
    state->thread = pthread_self();
    parser = xmlNewParserCtxt();
    if (parser == NULL) {
        xml_unref(state);
        free(receiver);
        return xml_failure(error, KNURL_NOMEM, "allocating XML parser");
    }
    xmlCtxtSetErrorHandler(parser, xml_native_error, state);
    xmlCtxtSetResourceLoader(parser, xml_no_resource, state);
    state->native =
        xmlCtxtReadMemory(parser, (const char *)data, (int)size, NULL, NULL,
                          XML_PARSE_NONET | XML_PARSE_NO_XXE);
    xmlFreeParserCtxt(parser);
    if (state->native == NULL || state->error.status != KNURL_OK ||
        xmlGetIntSubset(state->native) != NULL) {
        if (state->error.status != KNURL_OK) {
            status = state->error.status;
            if (error != NULL)
                *error = state->error;
        } else
            status = xml_failure(
                error, KNURL_INVALID,
                state->native == NULL
                    ? "XML document parse failed"
                    : "DTD/entity declarations are outside this XML surface");
        xml_unref(state);
        free(receiver);
        return status;
    }
    receiver->impl = state;
    *out = receiver;
    return KNURL_OK;
}
knurl_status knurl_xml_document_root(knurl_xml_document *receiver,
                                     knurl_xml_node **out, knurl_error *error)
{
    xml_document_state *state =
        receiver == NULL ? NULL : (xml_document_state *)receiver->impl;
    knurl_status status = xml_owner(state, error);
    if (out != NULL)
        *out = NULL;
    if (status != KNURL_OK)
        return status;
    if (out == NULL)
        return xml_failure(error, KNURL_INVALID, "missing XML root output");
    return xml_view(state, xmlDocGetRootElement(state->native), out, error);
}
knurl_status knurl_xml_document_close(knurl_xml_document *receiver,
                                      knurl_error *error)
{
    xml_document_state *state;
    if (receiver == NULL)
        return KNURL_OK;
    state = (xml_document_state *)receiver->impl;
    if (state->process != getpid() ||
        !pthread_equal(state->thread, pthread_self()))
        return xml_failure(error, KNURL_STATE,
                           "cannot close foreign-process/thread XML document");
    xmlFreeDoc(state->native);
    state->native = NULL;
    xml_unref(state);
    free(receiver);
    return KNURL_OK;
}
knurl_status knurl_xml_node_close(knurl_xml_node *receiver, knurl_error *error)
{
    xml_node_state *state;
    if (receiver == NULL)
        return KNURL_OK;
    state = (xml_node_state *)receiver->impl;
    if (state->owner->process != getpid() ||
        !pthread_equal(state->owner->thread, pthread_self()))
        return xml_failure(
            error, KNURL_STATE,
            "cannot close foreign-process/thread XML element view");
    xml_unref(state->owner);
    free(state);
    return KNURL_OK;
}
knurl_status knurl_xml_node_name(knurl_xml_node *receiver, knurl_xml_name *out,
                                 knurl_error *error)
{
    xml_node_state *state;
    knurl_status status = xml_node_check(receiver, &state, error);
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    if (status != KNURL_OK)
        return status;
    if (out == NULL)
        return xml_failure(error, KNURL_INVALID, "missing XML name output");
    out->name = (const char *)state->native->name;
    if (state->native->ns != NULL) {
        out->namespace_uri = (const char *)state->native->ns->href;
        out->prefix = (const char *)state->native->ns->prefix;
    }
    return KNURL_OK;
}
knurl_status knurl_xml_node_text(knurl_xml_node *receiver,
                                 knurl_owned_bytes *out, knurl_error *error)
{
    xml_node_state *state;
    xmlChar *value;
    knurl_status status = xml_node_check(receiver, &state, error);
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    if (status != KNURL_OK)
        return status;
    if (out == NULL)
        return xml_failure(error, KNURL_INVALID, "missing XML text output");
    value = xmlNodeGetContent(state->native);
    if (value == NULL)
        return xml_failure(error, KNURL_NOMEM, "reading XML text failed");
    status = xml_copy(value, (size_t)xmlStrlen(value),
                      state->owner->config.max_bytes, out, error);
    xmlFree(value);
    return status;
}
static int xml_name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
           xmlValidateNCName((const xmlChar *)name, 0) == 0;
}
knurl_status knurl_xml_node_attribute(knurl_xml_node *receiver,
                                      const char *name, const char *uri,
                                      knurl_owned_bytes *out, int *present,
                                      knurl_error *error)
{
    xml_node_state *state;
    xmlChar *value;
    knurl_status status = xml_node_check(receiver, &state, error);
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    if (present != NULL)
        *present = 0;
    if (status != KNURL_OK)
        return status;
    if (out == NULL || present == NULL || !xml_name_valid(name))
        return xml_failure(error, KNURL_INVALID,
                           "invalid XML attribute name/output");
    if (xmlHasNsProp(state->native, (const xmlChar *)name,
                     (const xmlChar *)uri) == NULL)
        return KNURL_OK;
    value = xmlGetNsProp(state->native, (const xmlChar *)name,
                         (const xmlChar *)uri);
    if (value == NULL)
        return xml_failure(error, KNURL_NOMEM, "reading XML attribute failed");
    status = xml_copy(value, (size_t)xmlStrlen(value),
                      state->owner->config.max_bytes, out, error);
    xmlFree(value);
    if (status == KNURL_OK)
        *present = 1;
    return status;
}
static knurl_status xml_text_valid(const void *data, size_t size, size_t limit,
                                   knurl_error *error)
{
    const unsigned char *text = (const unsigned char *)data;
    size_t offset = 0u;
    int length, character;
    if (size > limit || (data == NULL && size != 0u))
        return xml_failure(error, KNURL_INVALID,
                           "XML text exceeds max_bytes or is missing");
    while (offset < size) {
        length = size - offset > 4u ? 4 : (int)(size - offset);
        character = xmlGetUTF8Char(text + offset, &length);
        if (character < 0 || length <= 0 ||
            !xmlIsCharQ((unsigned int)character))
            return xml_failure(error, KNURL_INVALID,
                               "XML text requires valid XML UTF-8 characters");
        offset += (size_t)length;
    }
    return KNURL_OK;
}
static int xml_namespace_valid(const char *uri, const char *prefix)
{
    const char *xml_uri = "http://www.w3.org/XML/1998/namespace";
    if (uri == NULL)
        return prefix == NULL;
    if (uri[0] == '\0' || strcmp(uri, "http://www.w3.org/2000/xmlns/") == 0)
        return 0;
    if (prefix != NULL &&
        (!xml_name_valid(prefix) || strcmp(prefix, "xmlns") == 0))
        return 0;
    return (prefix != NULL && strcmp(prefix, "xml") == 0) ==
           (strcmp(uri, xml_uri) == 0);
}
static knurl_status xml_mutate(xml_document_state *state, knurl_error *error)
{
    if (state->generation == ULONG_MAX)
        return xml_failure(error, KNURL_STATE,
                           "XML mutation generation exhausted");
    ++state->generation;
    return KNURL_OK;
}
knurl_status knurl_xml_node_set_text(knurl_xml_node *receiver, const void *data,
                                     size_t size, knurl_error *error)
{
    xml_node_state *state;
    xmlNode *text = NULL;
    knurl_status status = xml_node_check(receiver, &state, error);
    if (status != KNURL_OK)
        return status;
    status = xml_text_valid(data, size, state->owner->config.max_bytes, error);
    if (status != KNURL_OK)
        return status;
    if (size != 0u) {
        text = xmlNewDocTextLen(state->owner->native, (const xmlChar *)data,
                                (int)size);
        if (text == NULL)
            return xml_failure(error, KNURL_NOMEM,
                               "allocating literal XML text");
    }
    status = xml_mutate(state->owner, error);
    if (status != KNURL_OK) {
        xmlFreeNode(text);
        return status;
    }
    if (xmlNodeSetContent(state->native, NULL) < 0) {
        xmlFreeNode(text);
        return xml_failure(error, KNURL_NOMEM, "clearing XML text failed");
    }
    if (text != NULL && xmlAddChild(state->native, text) == NULL) {
        xmlFreeNode(text);
        return xml_failure(error, KNURL_NOMEM,
                           "setting literal XML text failed");
    }
    return KNURL_OK;
}
knurl_status knurl_xml_node_set_attribute(knurl_xml_node *receiver,
                                          const char *name, const void *data,
                                          size_t size, const char *uri,
                                          const char *prefix,
                                          knurl_error *error)
{
    xml_node_state *state;
    xmlNs *ns = NULL;
    char *value;
    xmlAttr *attribute;
    knurl_status status = xml_node_check(receiver, &state, error);
    if (status != KNURL_OK)
        return status;
    if (!xml_name_valid(name) || (uri == NULL && strcmp(name, "xmlns") == 0) ||
        !xml_namespace_valid(uri, prefix) || (uri != NULL && prefix == NULL))
        return xml_failure(error, KNURL_INVALID,
                           "invalid XML attribute name/namespace/prefix");
    status = xml_text_valid(data, size, state->owner->config.max_bytes, error);
    if (status != KNURL_OK)
        return status;
    if (uri != NULL) {
        status = xml_text_valid(uri, strlen(uri),
                                state->owner->config.max_bytes, error);
        if (status != KNURL_OK)
            return status;
        ns = xmlSearchNs(state->owner->native, state->native,
                         (const xmlChar *)prefix);
        if (ns != NULL && !xmlStrEqual(ns->href, (const xmlChar *)uri))
            return xml_failure(
                error, KNURL_CONFLICT,
                "XML attribute prefix is bound to another namespace");
    }
    value = (char *)malloc(size + 1u);
    if (value == NULL)
        return xml_failure(error, KNURL_NOMEM, "copying XML attribute value");
    if (size != 0u)
        memcpy(value, data, size);
    value[size] = '\0';
    status = xml_mutate(state->owner, error);
    if (status != KNURL_OK) {
        free(value);
        return status;
    }
    if (uri != NULL && ns == NULL)
        ns = xmlNewNs(state->native, (const xmlChar *)uri,
                      (const xmlChar *)prefix);
    if (uri != NULL && ns == NULL) {
        free(value);
        return xml_failure(error, KNURL_NOMEM,
                           "creating XML attribute namespace failed");
    }
    attribute = xmlSetNsProp(state->native, ns, (const xmlChar *)name,
                             (const xmlChar *)value);
    free(value);
    if (attribute == NULL)
        return xml_failure(error, KNURL_NOMEM, "setting XML attribute failed");
    return KNURL_OK;
}
knurl_status knurl_xml_node_append_child(knurl_xml_node *receiver,
                                         const char *name,
                                         const knurl_xml_child_config *config,
                                         knurl_xml_node **out,
                                         knurl_error *error)
{
    xml_node_state *parent, *view;
    knurl_xml_child_config defaults;
    knurl_xml_node *result;
    xmlNode *child, *text;
    xmlNs *ns;
    knurl_status status = xml_node_check(receiver, &parent, error);
    if (out != NULL)
        *out = NULL;
    if (status != KNURL_OK)
        return status;
    memset(&defaults, 0, sizeof(defaults));
    if (config == NULL)
        config = &defaults;
    if (out == NULL || !xml_name_valid(name) ||
        (config->text_present != 0 && config->text_present != 1) ||
        !xml_namespace_valid(config->namespace_uri, config->prefix))
        return xml_failure(error, KNURL_INVALID,
                           "invalid XML child name/namespace/options");
    if (config->text_present) {
        status = xml_text_valid(config->text, config->text_size,
                                parent->owner->config.max_bytes, error);
        if (status != KNURL_OK)
            return status;
    }
    if (config->namespace_uri != NULL) {
        status =
            xml_text_valid(config->namespace_uri, strlen(config->namespace_uri),
                           parent->owner->config.max_bytes, error);
        if (status != KNURL_OK)
            return status;
    }
    child =
        xmlNewDocNode(parent->owner->native, NULL, (const xmlChar *)name, NULL);
    if (child == NULL)
        return xml_failure(error, KNURL_NOMEM, "allocating XML child");
    ns = config->namespace_uri == NULL
             ? parent->native->ns
             : xmlNewNs(child, (const xmlChar *)config->namespace_uri,
                        (const xmlChar *)config->prefix);
    if (config->namespace_uri != NULL && ns == NULL) {
        xmlFreeNode(child);
        return xml_failure(error, KNURL_NOMEM,
                           "creating XML child namespace failed");
    }
    xmlSetNs(child, ns);
    if (config->text_present && config->text_size != 0u) {
        text = xmlNewDocTextLen(parent->owner->native,
                                (const xmlChar *)config->text,
                                (int)config->text_size);
        if (text == NULL) {
            xmlFreeNode(child);
            return xml_failure(error, KNURL_NOMEM, "allocating XML child text");
        }
        if (xmlAddChild(child, text) == NULL) {
            xmlFreeNode(text);
            xmlFreeNode(child);
            return xml_failure(error, KNURL_NOMEM, "attaching XML child text");
        }
    }
    status = xml_view(parent->owner, child, &result, error);
    if (status != KNURL_OK) {
        xmlFreeNode(child);
        return status;
    }
    status = xml_mutate(parent->owner, error);
    if (status != KNURL_OK) {
        knurl_xml_node_close(result, NULL);
        xmlFreeNode(child);
        return status;
    }
    if (xmlAddChild(parent->native, child) == NULL) {
        knurl_xml_node_close(result, NULL);
        xmlFreeNode(child);
        return xml_failure(error, KNURL_NOMEM, "appending XML child failed");
    }
    view = (xml_node_state *)result->impl;
    view->generation = parent->owner->generation;
    *out = result;
    return KNURL_OK;
}
knurl_status knurl_xml_node_remove(knurl_xml_node *receiver, knurl_error *error)
{
    xml_node_state *state;
    knurl_status status = xml_node_check(receiver, &state, error);
    if (status != KNURL_OK)
        return status;
    if (xmlDocGetRootElement(state->owner->native) == state->native)
        return xml_failure(error, KNURL_INVALID,
                           "cannot remove the document root");
    status = xml_mutate(state->owner, error);
    if (status != KNURL_OK)
        return status;
    xmlUnlinkNode(state->native);
    xmlFreeNode(state->native);
    return KNURL_OK;
}
void knurl_xml_xpath_result_cleanup(knurl_xml_xpath_result *result)
{
    size_t i;
    if (result == NULL)
        return;
    for (i = 0u; i < result->count; ++i)
        knurl_xml_node_close(result->nodes[i], NULL);
    free(result->nodes);
    knurl_owned_bytes_cleanup(&result->string);
    memset(result, 0, sizeof(*result));
}
knurl_status knurl_xml_document_xpath(knurl_xml_document *receiver,
                                      const char *expression,
                                      const knurl_xml_namespace *namespaces,
                                      size_t namespace_count,
                                      knurl_xml_xpath_result *out,
                                      knurl_error *error)
{
    xml_document_state *state =
        receiver == NULL ? NULL : (xml_document_state *)receiver->impl;
    xmlXPathContext *context;
    xmlXPathObject *object = NULL;
    xmlNode **nodes;
    int count, i;
    size_t j;
    knurl_status status = xml_owner(state, error);
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    if (status != KNURL_OK)
        return status;
    if (out == NULL || expression == NULL || expression[0] == '\0' ||
        strlen(expression) > 1048576u || namespace_count > 256u ||
        (namespace_count != 0u && namespaces == NULL))
        return xml_failure(error, KNURL_INVALID,
                           "invalid XPath expression/namespaces/output");
    context = xmlXPathNewContext(state->native);
    if (context == NULL)
        return xml_failure(error, KNURL_NOMEM, "allocating XPath context");
    memset(&state->error, 0, sizeof(state->error));
    xmlXPathSetErrorHandler(context, xml_native_error, state);
    for (j = 0u; j < namespace_count; ++j) {
        if (!xml_name_valid(namespaces[j].prefix) ||
            namespaces[j].uri == NULL || namespaces[j].uri[0] == '\0') {
            status = xml_failure(
                error, KNURL_INVALID,
                "XPath requires nonempty namespace prefix/URI pairs");
            goto done;
        }
        if (xmlXPathRegisterNs(context, (const xmlChar *)namespaces[j].prefix,
                               (const xmlChar *)namespaces[j].uri) != 0) {
            status = xml_failure(error, KNURL_NOMEM,
                                 "registering XPath namespace failed");
            goto done;
        }
    }
    object = xmlXPathEvalExpression((const xmlChar *)expression, context);
    if (object == NULL || state->error.status != KNURL_OK) {
        if (state->error.status != KNURL_OK) {
            status = state->error.status;
            if (error != NULL)
                *error = state->error;
        } else
            status = xml_failure(error, KNURL_STATE, "XPath evaluation failed");
        goto done;
    }
    if (object->type == XPATH_BOOLEAN) {
        out->kind = KNURL_XML_XPATH_BOOLEAN;
        out->boolean = object->boolval != 0;
    } else if (object->type == XPATH_NUMBER) {
        out->kind = KNURL_XML_XPATH_NUMBER;
        out->number = object->floatval;
    } else if (object->type == XPATH_STRING) {
        out->kind = KNURL_XML_XPATH_STRING;
        status = xml_copy(object->stringval,
                          object->stringval == NULL
                              ? 0u
                              : (size_t)xmlStrlen(object->stringval),
                          state->config.max_bytes, &out->string, error);
    } else if (object->type == XPATH_NODESET) {
        out->kind = KNURL_XML_XPATH_NODES;
        count = xmlXPathNodeSetGetLength(object->nodesetval);
        if (count < 0 || (size_t)count > state->config.max_xpath_nodes ||
            (size_t)count > (size_t)-1 / sizeof(*out->nodes)) {
            status = xml_failure(error, KNURL_INVALID,
                                 "XPath result exceeds max_xpath_nodes");
            goto done;
        }
        nodes = object->nodesetval == NULL ? NULL : object->nodesetval->nodeTab;
        for (i = 0; i < count; ++i)
            if (nodes[i] == NULL || nodes[i]->type != XML_ELEMENT_NODE) {
                status =
                    xml_failure(error, KNURL_INVALID,
                                "XPath node views support elements only; use "
                                "scalar functions for other node types");
                goto done;
            }
        if (count > 1)
            xmlXPathNodeSetSort(object->nodesetval);
        nodes = object->nodesetval == NULL ? NULL : object->nodesetval->nodeTab;
        if (count != 0) {
            out->nodes =
                (knurl_xml_node **)calloc((size_t)count, sizeof(*out->nodes));
            if (out->nodes == NULL) {
                status = xml_failure(error, KNURL_NOMEM,
                                     "allocating XPath result views");
                goto done;
            }
            out->count = (size_t)count;
            for (i = 0; i < count; ++i) {
                status = xml_view(state, nodes[i], &out->nodes[i], error);
                if (status != KNURL_OK)
                    goto done;
            }
        }
    } else
        status =
            xml_failure(error, KNURL_INVALID, "unsupported XPath result type");
done:
    xmlXPathFreeObject(object);
    xmlXPathFreeContext(context);
    if (status != KNURL_OK)
        knurl_xml_xpath_result_cleanup(out);
    return status;
}
knurl_status
knurl_xml_document_serialize_materialized(knurl_xml_document *receiver,
                                          int pretty, knurl_owned_bytes *out,
                                          knurl_error *error)
{
    xml_document_state *state =
        receiver == NULL ? NULL : (xml_document_state *)receiver->impl;
    xmlChar *bytes = NULL;
    int size = 0;
    knurl_status status = xml_owner(state, error);
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    if (status != KNURL_OK)
        return status;
    if (out == NULL || (pretty != 0 && pretty != 1))
        return xml_failure(error, KNURL_INVALID,
                           "invalid XML serialization output/pretty flag");
    xmlDocDumpFormatMemoryEnc(state->native, &bytes, &size, "UTF-8", pretty);
    if (bytes == NULL || size < 0) {
        xmlFree(bytes);
        return xml_failure(error, KNURL_NOMEM,
                           "materializing XML serialization failed");
    }
    status = xml_copy(bytes, (size_t)size, state->config.max_bytes, out, error);
    xmlFree(bytes);
    return status;
}
