#ifndef ROTMOS_REFERENCE_XML_H
#define ROTMOS_REFERENCE_XML_H
/* Test-only transcription of Vectis 6b1e263's public XML receivers. */
#include "vectis-route.h"
typedef void (*reference_receiver_reserved_fn)(void);
#ifdef __cplusplus
extern "C" {
#endif
#define REFERENCE_XML_DEFAULT_MAX_BYTES 16777216u
#define REFERENCE_XML_DEFAULT_MAX_XPATH_NODES 4096u
#define REFERENCE_XML_SAX_CHUNK_BYTES 65536u
#define REFERENCE_XML_SAX_MAX_ATTRIBUTES 256u
typedef struct reference_xml_document reference_xml_document;
typedef struct reference_xml_node reference_xml_node;
typedef struct reference_xml_sax reference_xml_sax;
typedef struct reference_xml_document_config {
    size_t max_bytes, max_xpath_nodes;
} reference_xml_document_config;
typedef struct reference_xml_name {
    const char *name, *namespace_uri, *prefix;
} reference_xml_name;
typedef struct reference_xml_namespace {
    const char *prefix, *uri;
} reference_xml_namespace;
typedef struct reference_xml_attribute {
    reference_xml_name name;
    const void *value;
    size_t size;
} reference_xml_attribute;
typedef struct reference_xml_child_config {
    const char *namespace_uri, *prefix;
    const void *text;
    size_t text_size;
    int text_present;
} reference_xml_child_config;
typedef enum reference_xml_xpath_kind {
    REFERENCE_XML_XPATH_NODES = 1,
    REFERENCE_XML_XPATH_STRING = 2,
    REFERENCE_XML_XPATH_NUMBER = 3,
    REFERENCE_XML_XPATH_BOOLEAN = 4
} reference_xml_xpath_kind;
typedef struct reference_xml_xpath_result {
    reference_xml_xpath_kind kind;
    reference_xml_node **nodes;
    size_t count;
    struct reference_mutable_bytes string;
    double number;
    int boolean;
} reference_xml_xpath_result;
/* Owned materialized DOM, caller-serialized and process/thread-confined.
 * Views represent elements only and retain parent state. Every mutation
 * invalidates previous views, including its receiver. Name strings are borrowed
 * until mutation/close; returned byte copies are caller-owned. DTDs are outside
 * this slice; external resource loaders are disabled per parser context. */
struct reference_xml_document {
    reference_status (*root)(reference_xml_document *, reference_xml_node **,
                             reference_error *);
    reference_status (*xpath)(reference_xml_document *, const char *,
                              const reference_xml_namespace *, size_t,
                              reference_xml_xpath_result *, reference_error *);
    reference_status (*serialize_materialized)(reference_xml_document *, int,
                                               struct reference_mutable_bytes *,
                                               reference_error *);
    reference_status (*close)(reference_xml_document *, reference_error *);
    void *impl;
    reference_receiver_reserved_fn reserved[8];
};
struct reference_xml_node {
    reference_status (*name)(reference_xml_node *, reference_xml_name *,
                             reference_error *);
    reference_status (*text)(reference_xml_node *,
                             struct reference_mutable_bytes *,
                             reference_error *);
    reference_status (*attribute)(reference_xml_node *, const char *,
                                  const char *,
                                  struct reference_mutable_bytes *, int *,
                                  reference_error *);
    reference_status (*set_text)(reference_xml_node *, const void *, size_t,
                                 reference_error *);
    reference_status (*set_attribute)(reference_xml_node *, const char *,
                                      const void *, size_t, const char *,
                                      const char *, reference_error *);
    reference_status (*append_child)(reference_xml_node *, const char *,
                                     const reference_xml_child_config *,
                                     reference_xml_node **, reference_error *);
    reference_status (*remove)(reference_xml_node *, reference_error *);
    reference_status (*close)(reference_xml_node *, reference_error *);
    void *impl;
    reference_receiver_reserved_fn reserved[8];
};
void reference_xml_document_config_init(reference_xml_document_config *);
reference_status
reference_xml_document_parse(const void *, size_t,
                             const reference_xml_document_config *,
                             reference_xml_document **, reference_error *);
reference_status reference_xml_document_root(reference_xml_document *,
                                             reference_xml_node **,
                                             reference_error *);
reference_status
reference_xml_document_xpath(reference_xml_document *, const char *,
                             const reference_xml_namespace *, size_t,
                             reference_xml_xpath_result *, reference_error *);
void reference_xml_xpath_result_cleanup(reference_xml_xpath_result *);
reference_status
reference_xml_document_serialize_materialized(reference_xml_document *, int,
                                              struct reference_mutable_bytes *,
                                              reference_error *);
reference_status reference_xml_document_close(reference_xml_document *,
                                              reference_error *);
reference_status reference_xml_node_name(reference_xml_node *,
                                         reference_xml_name *,
                                         reference_error *);
reference_status reference_xml_node_text(reference_xml_node *,
                                         struct reference_mutable_bytes *,
                                         reference_error *);
reference_status reference_xml_node_attribute(reference_xml_node *,
                                              const char *, const char *,
                                              struct reference_mutable_bytes *,
                                              int *, reference_error *);
reference_status reference_xml_node_set_text(reference_xml_node *, const void *,
                                             size_t, reference_error *);
reference_status reference_xml_node_set_attribute(reference_xml_node *,
                                                  const char *, const void *,
                                                  size_t, const char *,
                                                  const char *,
                                                  reference_error *);
reference_status
reference_xml_node_append_child(reference_xml_node *, const char *,
                                const reference_xml_child_config *,
                                reference_xml_node **, reference_error *);
reference_status reference_xml_node_remove(reference_xml_node *,
                                           reference_error *);
reference_status reference_xml_node_close(reference_xml_node *,
                                          reference_error *);

typedef struct reference_xml_sax_callbacks {
    reference_status (*start_element)(void *, const reference_xml_name *,
                                      const reference_xml_attribute *, size_t,
                                      const reference_xml_namespace *, size_t,
                                      reference_error *);
    reference_status (*end_element)(void *, const reference_xml_name *,
                                    reference_error *);
    reference_status (*text)(void *, const void *, size_t, reference_error *);
} reference_xml_sax_callbacks;
/* Synchronous bounded SAX flow, no DOM or cumulative stream-size cap. Callback
 * data is borrowed only during the call. Callbacks must not longjmp; failure
 * latches the parser. No callback runs from close. Context remains
 * caller-owned. metadata_limit=0 selects 16 MiB per start event;
 * attrs/declarations total<=256.
 */
struct reference_xml_sax {
    reference_status (*feed)(reference_xml_sax *, const void *, size_t,
                             reference_error *);
    reference_status (*finish)(reference_xml_sax *, reference_error *);
    reference_status (*close)(reference_xml_sax *, reference_error *);
    void *impl;
    reference_receiver_reserved_fn reserved[8];
};
reference_status reference_xml_sax_new(const reference_xml_sax_callbacks *,
                                       void *, size_t, reference_xml_sax **,
                                       reference_error *);
reference_status reference_xml_sax_feed(reference_xml_sax *, const void *,
                                        size_t, reference_error *);
reference_status reference_xml_sax_finish(reference_xml_sax *,
                                          reference_error *);
reference_status reference_xml_sax_close(reference_xml_sax *,
                                         reference_error *);
#ifdef __cplusplus
}
#endif
#endif
