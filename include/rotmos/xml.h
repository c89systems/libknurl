#ifndef ROTMOS_XML_H
#define ROTMOS_XML_H
#include <rotmos/common.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ROTMOS_XML_DEFAULT_MAX_BYTES 16777216u
#define ROTMOS_XML_DEFAULT_MAX_XPATH_NODES 4096u
#define ROTMOS_XML_SAX_CHUNK_BYTES 65536u
#define ROTMOS_XML_SAX_MAX_ATTRIBUTES 256u
typedef struct rotmos_xml_document rotmos_xml_document;
typedef struct rotmos_xml_node rotmos_xml_node;
typedef struct rotmos_xml_sax rotmos_xml_sax;
typedef struct rotmos_xml_document_config {
    size_t max_bytes, max_xpath_nodes;
} rotmos_xml_document_config;
typedef struct rotmos_xml_name {
    const char *name, *namespace_uri, *prefix;
} rotmos_xml_name;
typedef struct rotmos_xml_namespace {
    const char *prefix, *uri;
} rotmos_xml_namespace;
typedef struct rotmos_xml_attribute {
    rotmos_xml_name name;
    const void *value;
    size_t length;
} rotmos_xml_attribute;
typedef struct rotmos_xml_child_config {
    const char *namespace_uri, *prefix;
    const void *text;
    size_t text_size;
    int text_present;
} rotmos_xml_child_config;
typedef enum rotmos_xml_xpath_kind {
    ROTMOS_XML_XPATH_NODES = 1,
    ROTMOS_XML_XPATH_STRING = 2,
    ROTMOS_XML_XPATH_NUMBER = 3,
    ROTMOS_XML_XPATH_BOOLEAN = 4
} rotmos_xml_xpath_kind;
typedef struct rotmos_xml_xpath_result {
    rotmos_xml_xpath_kind kind;
    rotmos_xml_node **nodes;
    size_t count;
    rotmos_owned_bytes string;
    double number;
    int boolean;
} rotmos_xml_xpath_result;
/* Owned materialized DOM, caller-serialized and process/thread-confined.
 * Views represent elements only and retain parent state. Every mutation
 * invalidates previous views, including its receiver. Name strings are borrowed
 * until mutation/close; returned byte copies are caller-owned. DTDs are outside
 * this slice; external resource loaders are disabled per parser context. */
ROTMOS_API void rotmos_xml_document_config_init(rotmos_xml_document_config *);
ROTMOS_API rotmos_status rotmos_xml_document_parse(
    const void *, size_t, const rotmos_xml_document_config *,
    rotmos_xml_document **, rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_document_root(rotmos_xml_document *,
                                                  rotmos_xml_node **,
                                                  rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_document_xpath(
    rotmos_xml_document *, const char *, const rotmos_xml_namespace *, size_t,
    rotmos_xml_xpath_result *, rotmos_error *);
ROTMOS_API void rotmos_xml_xpath_result_cleanup(rotmos_xml_xpath_result *);
ROTMOS_API rotmos_status rotmos_xml_document_serialize_materialized(
    rotmos_xml_document *, int, rotmos_owned_bytes *, rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_document_close(rotmos_xml_document *,
                                                   rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_node_name(rotmos_xml_node *,
                                              rotmos_xml_name *,
                                              rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_node_text(rotmos_xml_node *,
                                              rotmos_owned_bytes *,
                                              rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_node_attribute(rotmos_xml_node *,
                                                   const char *, const char *,
                                                   rotmos_owned_bytes *, int *,
                                                   rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_node_set_text(rotmos_xml_node *,
                                                  const void *, size_t,
                                                  rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_node_set_attribute(
    rotmos_xml_node *, const char *, const void *, size_t, const char *,
    const char *, rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_node_append_child(
    rotmos_xml_node *, const char *, const rotmos_xml_child_config *,
    rotmos_xml_node **, rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_node_remove(rotmos_xml_node *,
                                                rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_node_close(rotmos_xml_node *,
                                               rotmos_error *);

typedef struct rotmos_xml_sax_callbacks {
    rotmos_status (*start_element)(void *, const rotmos_xml_name *,
                                   const rotmos_xml_attribute *, size_t,
                                   const rotmos_xml_namespace *, size_t,
                                   rotmos_error *);
    rotmos_status (*end_element)(void *, const rotmos_xml_name *,
                                 rotmos_error *);
    rotmos_status (*text)(void *, const void *, size_t, rotmos_error *);
} rotmos_xml_sax_callbacks;
/* Synchronous bounded SAX flow, no DOM or cumulative stream-size cap. Callback
 * data is borrowed only during the call. Callbacks must not longjmp; failure
 * latches the parser. No callback runs from close. Context remains
 * caller-owned. metadata_limit=0 selects 16 MiB per start event;
 * attrs/declarations total<=256.
 */
ROTMOS_API rotmos_status rotmos_xml_sax_new(const rotmos_xml_sax_callbacks *,
                                            void *, size_t, rotmos_xml_sax **,
                                            rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_sax_feed(rotmos_xml_sax *, const void *,
                                             size_t, rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_sax_finish(rotmos_xml_sax *,
                                               rotmos_error *);
ROTMOS_API rotmos_status rotmos_xml_sax_close(rotmos_xml_sax *, rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif
