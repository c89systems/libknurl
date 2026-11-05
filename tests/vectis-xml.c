/* Baseline receiver/callback translation only; no libxml2 or Core internals. */
#include "vectis-xml.h"
#include <rotmos/xml.h>
#include <stdlib.h>
#include <string.h>

static reference_status failed(reference_error *error, reference_status code)
{
    reference_error_clear(error);
    if (error != NULL) {
        error->code = code;
        error->source = 1;
        strcpy(error->message, code == REFERENCE_NOMEM
                                   ? "allocating XML receiver adapter"
                                   : "missing XML output/callbacks");
    }
    return code;
}
static reference_status result(rotmos_status status, rotmos_error *native,
                               reference_error *error)
{
    return reference_error_from_rotmos(status, native, error);
}
static void bytes_move(struct reference_mutable_bytes *out,
                       rotmos_owned_bytes *bytes)
{
    out->data = bytes->data;
    out->size = bytes->length;
}
static reference_status node_wrap(rotmos_xml_node *native,
                                  reference_xml_node **out,
                                  reference_error *error)
{
    reference_xml_node *node = calloc(1, sizeof(*node));
    *out = NULL;
    if (node == NULL) {
        rotmos_xml_node_close(native, NULL);
        return failed(error, REFERENCE_NOMEM);
    }
    node->name = reference_xml_node_name;
    node->text = reference_xml_node_text;
    node->attribute = reference_xml_node_attribute;
    node->set_text = reference_xml_node_set_text;
    node->set_attribute = reference_xml_node_set_attribute;
    node->append_child = reference_xml_node_append_child;
    node->remove = reference_xml_node_remove;
    node->close = reference_xml_node_close;
    node->impl = native;
    *out = node;
    reference_error_clear(error);
    return REFERENCE_OK;
}
void reference_xml_document_config_init(reference_xml_document_config *out)
{
    rotmos_xml_document_config config;
    if (out == NULL)
        return;
    rotmos_xml_document_config_init(&config);
    out->max_bytes = config.max_bytes;
    out->max_xpath_nodes = config.max_xpath_nodes;
}
reference_status reference_xml_document_parse(
    const void *data, size_t length, const reference_xml_document_config *input,
    reference_xml_document **out, reference_error *error)
{
    rotmos_xml_document_config config, *effective = NULL;
    rotmos_xml_document *native = NULL;
    reference_xml_document *doc;
    rotmos_error cause;
    rotmos_status status;
    if (out == NULL)
        return failed(error, REFERENCE_INVALID);
    *out = NULL;
    if (input != NULL) {
        config.max_bytes = input->max_bytes;
        config.max_xpath_nodes = input->max_xpath_nodes;
        effective = &config;
    }
    status =
        rotmos_xml_document_parse(data, length, effective, &native, &cause);
    if (status != ROTMOS_OK)
        return result(status, &cause, error);
    doc = calloc(1, sizeof(*doc));
    if (doc == NULL) {
        rotmos_xml_document_close(native, NULL);
        return failed(error, REFERENCE_NOMEM);
    }
    doc->root = reference_xml_document_root;
    doc->xpath = reference_xml_document_xpath;
    doc->serialize_materialized = reference_xml_document_serialize_materialized;
    doc->close = reference_xml_document_close;
    doc->impl = native;
    *out = doc;
    reference_error_clear(error);
    return REFERENCE_OK;
}
reference_status reference_xml_document_root(reference_xml_document *doc,
                                             reference_xml_node **out,
                                             reference_error *error)
{
    rotmos_xml_node *node;
    rotmos_error cause;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    status = rotmos_xml_document_root(doc != NULL ? doc->impl : NULL,
                                      out != NULL ? &node : NULL, &cause);
    return status == ROTMOS_OK ? node_wrap(node, out, error)
                               : result(status, &cause, error);
}
reference_status reference_xml_document_close(reference_xml_document *doc,
                                              reference_error *error)
{
    rotmos_error cause;
    rotmos_status status =
        rotmos_xml_document_close(doc != NULL ? doc->impl : NULL, &cause);
    if (status == ROTMOS_OK)
        free(doc);
    return result(status, &cause, error);
}
reference_status reference_xml_node_close(reference_xml_node *node,
                                          reference_error *error)
{
    rotmos_error cause;
    rotmos_status status =
        rotmos_xml_node_close(node != NULL ? node->impl : NULL, &cause);
    if (status == ROTMOS_OK)
        free(node);
    return result(status, &cause, error);
}
reference_status reference_xml_node_name(reference_xml_node *node,
                                         reference_xml_name *out,
                                         reference_error *error)
{
    rotmos_xml_name name;
    rotmos_error cause;
    rotmos_status status = rotmos_xml_node_name(
        node != NULL ? node->impl : NULL, out != NULL ? &name : NULL, &cause);
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        if (status == ROTMOS_OK) {
            out->name = name.name;
            out->namespace_uri = name.namespace_uri;
            out->prefix = name.prefix;
        }
    }
    return result(status, &cause, error);
}
reference_status reference_xml_node_text(reference_xml_node *node,
                                         struct reference_mutable_bytes *out,
                                         reference_error *error)
{
    rotmos_owned_bytes bytes = {NULL, 0};
    rotmos_error cause;
    rotmos_status status = rotmos_xml_node_text(
        node != NULL ? node->impl : NULL, out != NULL ? &bytes : NULL, &cause);
    if (out != NULL)
        bytes_move(out, &bytes);
    return result(status, &cause, error);
}
reference_status reference_xml_node_attribute(
    reference_xml_node *node, const char *name, const char *uri,
    struct reference_mutable_bytes *out, int *found, reference_error *error)
{
    rotmos_owned_bytes bytes = {NULL, 0};
    rotmos_error cause;
    rotmos_status status =
        rotmos_xml_node_attribute(node != NULL ? node->impl : NULL, name, uri,
                                  out != NULL ? &bytes : NULL, found, &cause);
    if (out != NULL)
        bytes_move(out, &bytes);
    return result(status, &cause, error);
}
reference_status reference_xml_node_set_text(reference_xml_node *node,
                                             const void *bytes, size_t length,
                                             reference_error *error)
{
    rotmos_error cause;
    rotmos_status status = rotmos_xml_node_set_text(
        node != NULL ? node->impl : NULL, bytes, length, &cause);
    return result(status, &cause, error);
}
reference_status reference_xml_node_set_attribute(
    reference_xml_node *node, const char *name, const void *bytes,
    size_t length, const char *uri, const char *prefix, reference_error *error)
{
    rotmos_error cause;
    rotmos_status status =
        rotmos_xml_node_set_attribute(node != NULL ? node->impl : NULL, name,
                                      bytes, length, uri, prefix, &cause);
    return result(status, &cause, error);
}
reference_status reference_xml_node_remove(reference_xml_node *node,
                                           reference_error *error)
{
    rotmos_error cause;
    rotmos_status status =
        rotmos_xml_node_remove(node != NULL ? node->impl : NULL, &cause);
    return result(status, &cause, error);
}
reference_status
reference_xml_node_append_child(reference_xml_node *node, const char *name,
                                const reference_xml_child_config *input,
                                reference_xml_node **out,
                                reference_error *error)
{
    rotmos_xml_child_config config, *effective = NULL;
    rotmos_xml_node *child;
    rotmos_error cause;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (input != NULL) {
        config.namespace_uri = input->namespace_uri;
        config.prefix = input->prefix;
        config.text = input->text;
        config.text_size = input->text_size;
        config.text_present = input->text_present;
        effective = &config;
    }
    status = rotmos_xml_node_append_child(node != NULL ? node->impl : NULL,
                                          name, effective,
                                          out != NULL ? &child : NULL, &cause);
    return status == ROTMOS_OK ? node_wrap(child, out, error)
                               : result(status, &cause, error);
}
reference_status reference_xml_document_serialize_materialized(
    reference_xml_document *doc, int pretty,
    struct reference_mutable_bytes *out, reference_error *error)
{
    rotmos_owned_bytes bytes = {NULL, 0};
    rotmos_error cause;
    rotmos_status status = rotmos_xml_document_serialize_materialized(
        doc != NULL ? doc->impl : NULL, pretty, out != NULL ? &bytes : NULL,
        &cause);
    if (out != NULL)
        bytes_move(out, &bytes);
    return result(status, &cause, error);
}
void reference_xml_xpath_result_cleanup(reference_xml_xpath_result *out)
{
    size_t i;
    if (out == NULL)
        return;
    for (i = 0; i < out->count; ++i)
        reference_xml_node_close(out->nodes[i], NULL);
    free(out->nodes);
    reference_mutable_bytes_cleanup(&out->string);
    memset(out, 0, sizeof(*out));
}
reference_status reference_xml_document_xpath(
    reference_xml_document *doc, const char *expression,
    const reference_xml_namespace *namespaces, size_t count,
    reference_xml_xpath_result *out, reference_error *error)
{
    rotmos_xml_namespace *bindings = NULL;
    rotmos_xml_xpath_result native;
    rotmos_error cause;
    rotmos_status status;
    reference_status translated = REFERENCE_OK;
    size_t i;
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    if (namespaces != NULL && count != 0) {
        if (count > (size_t)-1 / sizeof(*bindings) ||
            (bindings = calloc(count, sizeof(*bindings))) == NULL)
            return failed(error, REFERENCE_NOMEM);
        for (i = 0; i < count; ++i) {
            bindings[i].prefix = namespaces[i].prefix;
            bindings[i].uri = namespaces[i].uri;
        }
    }
    status = rotmos_xml_document_xpath(doc != NULL ? doc->impl : NULL,
                                       expression, bindings, count,
                                       out != NULL ? &native : NULL, &cause);
    free(bindings);
    if (status != ROTMOS_OK)
        return result(status, &cause, error);
    out->kind = (reference_xml_xpath_kind)native.kind;
    out->number = native.number;
    out->boolean = native.boolean;
    if (native.count != 0) {
        out->nodes = calloc(native.count, sizeof(*out->nodes));
        if (out->nodes == NULL)
            translated = failed(error, REFERENCE_NOMEM);
        for (i = 0; translated == REFERENCE_OK && i < native.count; ++i) {
            translated = node_wrap(native.nodes[i], &out->nodes[i], error);
            native.nodes[i] = NULL;
            if (translated == REFERENCE_OK)
                ++out->count;
        }
    }
    if (translated == REFERENCE_OK) {
        bytes_move(&out->string, &native.string);
        memset(&native.string, 0, sizeof(native.string));
        reference_error_clear(error);
    } else
        reference_xml_xpath_result_cleanup(out);
    rotmos_xml_xpath_result_cleanup(&native);
    return translated;
}
struct sax_adapter {
    reference_xml_sax receiver;
    rotmos_xml_sax *native;
    reference_xml_sax_callbacks callbacks;
    void *context;
    reference_xml_attribute attributes[ROTMOS_XML_SAX_MAX_ATTRIBUTES];
    reference_xml_namespace namespaces[ROTMOS_XML_SAX_MAX_ATTRIBUTES];
};
static reference_xml_name name_from(const rotmos_xml_name *native)
{
    reference_xml_name name;
    name.name = native->name;
    name.prefix = native->prefix;
    name.namespace_uri = native->namespace_uri;
    return name;
}
static rotmos_status start(void *data, const rotmos_xml_name *element,
                           const rotmos_xml_attribute *attrs, size_t count,
                           const rotmos_xml_namespace *namespaces,
                           size_t declarations, rotmos_error *error)
{
    struct sax_adapter *adapter = data;
    reference_xml_name name = name_from(element);
    reference_error cause;
    reference_status status;
    size_t i;
    for (i = 0; i < count; ++i) {
        adapter->attributes[i].name = name_from(&attrs[i].name);
        adapter->attributes[i].value = attrs[i].value;
        adapter->attributes[i].size = attrs[i].length;
    }
    for (i = 0; i < declarations; ++i) {
        adapter->namespaces[i].prefix = namespaces[i].prefix;
        adapter->namespaces[i].uri = namespaces[i].uri;
    }
    reference_error_clear(&cause);
    status = adapter->callbacks.start_element(
        adapter->context, &name, adapter->attributes, count,
        adapter->namespaces, declarations, &cause);
    return reference_error_to_rotmos(status, &cause, error);
}
static rotmos_status end(void *data, const rotmos_xml_name *element,
                         rotmos_error *error)
{
    struct sax_adapter *adapter = data;
    reference_xml_name name = name_from(element);
    reference_error cause;
    reference_status status;
    reference_error_clear(&cause);
    status = adapter->callbacks.end_element(adapter->context, &name, &cause);
    return reference_error_to_rotmos(status, &cause, error);
}
static rotmos_status text(void *data, const void *bytes, size_t length,
                          rotmos_error *error)
{
    struct sax_adapter *adapter = data;
    reference_error cause;
    reference_status status;
    reference_error_clear(&cause);
    status = adapter->callbacks.text(adapter->context, bytes, length, &cause);
    return reference_error_to_rotmos(status, &cause, error);
}
reference_status reference_xml_sax_new(const reference_xml_sax_callbacks *input,
                                       void *context, size_t metadata_limit,
                                       reference_xml_sax **out,
                                       reference_error *error)
{
    struct sax_adapter *adapter;
    rotmos_xml_sax_callbacks callbacks;
    rotmos_error cause;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (input == NULL || out == NULL)
        return failed(error, REFERENCE_INVALID);
    adapter = calloc(1, sizeof(*adapter));
    if (adapter == NULL)
        return failed(error, REFERENCE_NOMEM);
    adapter->callbacks = *input;
    adapter->context = context;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.start_element = input->start_element != NULL ? start : NULL;
    callbacks.end_element = input->end_element != NULL ? end : NULL;
    callbacks.text = input->text != NULL ? text : NULL;
    status = rotmos_xml_sax_new(&callbacks, adapter, metadata_limit,
                                &adapter->native, &cause);
    if (status != ROTMOS_OK) {
        free(adapter);
        return result(status, &cause, error);
    }
    adapter->receiver.feed = reference_xml_sax_feed;
    adapter->receiver.finish = reference_xml_sax_finish;
    adapter->receiver.close = reference_xml_sax_close;
    adapter->receiver.impl = adapter;
    *out = &adapter->receiver;
    reference_error_clear(error);
    return REFERENCE_OK;
}
reference_status reference_xml_sax_feed(reference_xml_sax *receiver,
                                        const void *bytes, size_t length,
                                        reference_error *error)
{
    struct sax_adapter *adapter = receiver != NULL ? receiver->impl : NULL;
    rotmos_error cause;
    rotmos_status status = rotmos_xml_sax_feed(
        adapter != NULL ? adapter->native : NULL, bytes, length, &cause);
    return result(status, &cause, error);
}
reference_status reference_xml_sax_finish(reference_xml_sax *receiver,
                                          reference_error *error)
{
    struct sax_adapter *adapter = receiver != NULL ? receiver->impl : NULL;
    rotmos_error cause;
    rotmos_status status =
        rotmos_xml_sax_finish(adapter != NULL ? adapter->native : NULL, &cause);
    return result(status, &cause, error);
}
reference_status reference_xml_sax_close(reference_xml_sax *receiver,
                                         reference_error *error)
{
    struct sax_adapter *adapter = receiver != NULL ? receiver->impl : NULL;
    rotmos_error cause;
    rotmos_status status =
        rotmos_xml_sax_close(adapter != NULL ? adapter->native : NULL, &cause);
    if (status == ROTMOS_OK)
        free(adapter);
    return result(status, &cause, error);
}
