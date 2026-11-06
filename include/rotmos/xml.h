#ifndef ROTMOS_XML_H
#define ROTMOS_XML_H
#include <rotmos/common.h>
#include <rotmos/io.h>
#include <rotmos/router.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Default materialized DOM input bound: 16 MiB. */
#define ROTMOS_XML_DEFAULT_MAX_BYTES 16777216u
/** Default maximum element views returned by one DOM XPath query: 4096. */
#define ROTMOS_XML_DEFAULT_MAX_XPATH_NODES 4096u
/** Maximum bytes in one SAX feed/text chunk: 65536; no cumulative stream-size
 * ceiling.
 */
#define ROTMOS_XML_SAX_CHUNK_BYTES 65536u
/** Maximum combined attributes/namespace declarations per SAX start event: 256.
 */
#define ROTMOS_XML_SAX_MAX_ATTRIBUTES 256u
/** Owned materialized XML DOM; element views retain its state. */
typedef struct rotmos_xml_document rotmos_xml_document;
/** Owned element view; every document mutation invalidates previous views. */
typedef struct rotmos_xml_node rotmos_xml_node;
/** Owned incremental SAX parser; feed/finish synchronously invoke borrowed
 * callbacks.
 */
typedef struct rotmos_xml_sax rotmos_xml_sax;
struct lonejson_schema_view;
struct lonejson_record_view;
/** Optional typed-record owner adapter. acquire and release must both be
 * supplied; release runs for every acquire attempt, including partial failure.
 * Callbacks run on the owner loop and use the route handler userdata. XML
 * storage remains pinned through native parser acknowledgement.
 */
struct rotmos_record_provider;
/** Owned bounded XML pull generator retaining its mapped record owner. */
typedef struct rotmos_xml_generator rotmos_xml_generator;
/** Configuration for xml. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_xml_config {
    /** Mapped XML root element name. */
    const char *root_element;
    /** Mapped XML field name used for element text. */
    const char *text_key;
    /** Mapped XML attribute field prefix. */
    const char *attribute_prefix;
    /** Nonzero trims mapped XML text values. */
    int trim_text;
    /** Nonzero rejects unknown mapped XML fields. */
    int skip_unknown_disabled;
    /** Maximum mapped XML nesting depth. */
    size_t max_depth;
    /** Mapped XML text accumulation limit. */
    size_t max_text_bytes;
} rotmos_xml_config;
/** Initialize xml config with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_xml_config_init(rotmos_xml_config *);
/** Synchronous mapped input. Borrows source; consumes to EOF, never closes it.
 * AGAIN fails; live blocking readers execute on native threads. Caller supplies
 * initialized storage; reset before parse and on failure, preserving fixed
 * array backing. Maps and optional runtime use the released public LoneJSON
 * schema interface. */
ROTMOS_API rotmos_status
rotmos_xml_parse_source(rotmos_source *, const struct lonejson_schema_view *,
                        const rotmos_xml_config *, void *, rotmos_error *);
/** Parse a completed request body into caller-initialized mapped storage;
 * rewind before and after. See rotmos_xml_parse_source.
 */
ROTMOS_API rotmos_status
rotmos_request_xml_into(rotmos_request *, const struct lonejson_schema_view *,
                        const rotmos_xml_config *, void *, rotmos_error *);
/** Configuration for xml source. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_xml_source_config {
    /** Mapped record/view; retain backing storage according to the codec's
     * materialized/live contract.
     */
    const struct lonejson_record_view *record;
    /** Mapped XML configuration; DOM/SAX limits are separate. */
    rotmos_xml_config xml;
    /** Exactly-once owner-loop record-owner release after successful
     * construction; failure takes nothing. Native work acknowledges before
     * release.
     */
    rotmos_source_close_fn release;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_xml_source_config;
/** Initialize xml source config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_xml_source_config_init(rotmos_xml_source_config *);
/** Resumable bounded output, no whole-output collection. XML names <=4096
 * bytes; text units <=2048 bytes, independent of mapped text limits. Success
 * copies view and config declarations and borrows their storage unchanged until
 * release. Spooled text uses its public sequential cursor: exclusive read
 * access until close. Failed construction takes nothing. Handles are
 * process/thread-confined.
 */
ROTMOS_API rotmos_status rotmos_xml_generator_new(
    const rotmos_xml_source_config *, rotmos_xml_generator **, rotmos_error *);
/** Pull a bounded XML output chunk; EOF completes generation, without
 * collecting the whole output.
 */
ROTMOS_API rotmos_status rotmos_xml_generator_read(rotmos_xml_generator *,
                                                   void *, size_t, size_t *,
                                                   rotmos_error *);
/** Close the generator and release its retained record owner once. */
ROTMOS_API rotmos_status rotmos_xml_generator_destroy(rotmos_xml_generator *,
                                                      rotmos_error *);
/** Wrap a bounded XML generator as an owned source; record storage remains
 * borrowed until release.
 */
ROTMOS_API rotmos_status rotmos_source_xml(const rotmos_xml_source_config *,
                                           rotmos_source **, rotmos_error *);
/** Stage an explicit live XML source; record storage remains borrowed until
 * terminal source release.
 */
ROTMOS_API rotmos_status rotmos_reply_xml(rotmos_reply *, int,
                                          const rotmos_xml_source_config *,
                                          rotmos_error *);
/** Synchronously accept the complete offered XML chunk with OK or fail
 * terminally. Chunk bytes are borrowed only during the call.
 */
typedef rotmos_status (*rotmos_xml_sink_fn)(const void *, size_t, void *,
                                            rotmos_error *);
/** Sink must accept the complete offered chunk with OK or fail terminally. */
ROTMOS_API rotmos_status rotmos_xml_write_sink(
    const struct lonejson_record_view *, const rotmos_xml_config *,
    rotmos_xml_sink_fn, void *, rotmos_error *);
/** Explicit output collection; max_bytes=0 adds no ceiling. Empty out required.
 */
ROTMOS_API rotmos_status rotmos_xml_serialize_materialized(
    const struct lonejson_record_view *, const rotmos_xml_config *, size_t,
    rotmos_owned_bytes *, rotmos_error *);
/** Serialize XML synchronously into a copied staged reply; no record/schema
 * lifetime extends beyond return.
 */
ROTMOS_API rotmos_status rotmos_reply_xml_materialized(
    rotmos_reply *, int, const struct lonejson_record_view *,
    const rotmos_xml_config *, size_t, rotmos_error *);
/** Owner-loop handler after bounded native XML parsing. Borrow the mapped
 * record and select a staged reply; no transport body was collected.
 */
typedef rotmos_status (*rotmos_xml_route_fn)(
    rotmos_request *, const struct lonejson_record_view *, rotmos_reply *,
    void *, rotmos_error *);
/** Configuration for xml route. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_xml_route_config {
    /** Single HTTP method; methods supplies the explicit multi-method mask when
     * nonzero.
     */
    rotmos_http_method method;
    /** HTTP method mask; zero uses this operation's single-method/default
     * policy.
     */
    rotmos_http_methods methods;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    rotmos_route_path_kind path_kind;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    rotmos_body_policy body;
    /** Bounded live-upload queue/chunk capacity; zero selects the operation's
     * documented default.
     */
    size_t buffer_bytes;
    /** Borrowed input schema; an absent schema supplies no mapped input view.
     */
    const struct lonejson_schema_view *input;
    /** Mapped XML configuration; DOM/SAX limits are separate. */
    rotmos_xml_config xml;
    /** Application handler invoked under this route's documented execution and
     * lifetime rules.
     */
    rotmos_xml_route_fn handler;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    rotmos_route_hooks hooks;
    /** Optional copied record-owner adapter; its callbacks/storage obey
     * rotmos_record_provider.
     */
    const struct rotmos_record_provider *records;
} rotmos_xml_route_config;
/** Initialize xml route config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_xml_route_config_init(rotmos_xml_route_config *);
/** Live input uses the bounded upload-reader queue (buffer_bytes=0: 256 KiB).
 * Native parsing retains no live request/Lua handle; the completed record and
 * handler return to the owner loop. Records survive response cleanup. Ordinary
 * hooks and empty-reply 204 behavior apply. No transport body is collected.
 * Borrowed runtime allocators and mapped field callbacks used by parsing must
 * support native threads, as with the underlying upload-reader contract. */
ROTMOS_API rotmos_status
rotmos_router_xml_route(rotmos_router *, const rotmos_xml_route_config *,
                        rotmos_route **, rotmos_error *);
/** Register a mapped XML live-input route under the group prefix. The bounded
 * native parser returns the mapped record to owner-loop handlers; retain
 * schemas/userdata through router destruction.
 */
ROTMOS_API rotmos_status rotmos_route_group_xml_route(
    rotmos_route_group *, const rotmos_xml_route_config *, rotmos_route **,
    rotmos_error *);
/** Owned-DOM input/result limits, initialized by
 * rotmos_xml_document_config_init; separate from mapped and SAX bounds.
 */
typedef struct rotmos_xml_document_config {
    /** Maximum bytes for this operation; zero-inheritance/unlimited behavior is
     * operation-specific.
     */
    size_t max_bytes;
    /** Maximum DOM XPath node results. */
    size_t max_xpath_nodes;
} rotmos_xml_document_config;
/** Borrowed XML local name, namespace URI and prefix; lifetime follows the
 * producing node/event.
 */
typedef struct rotmos_xml_name {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Namespace URI; NULL/empty means no namespace where supported. */
    const char *namespace_uri;
    /** Namespace prefix, not a filesystem/group path. NULL/empty means no
     * prefix where supported.
     */
    const char *prefix;
} rotmos_xml_name;
/** One namespace prefix/URI binding for XPath or a borrowed SAX event. */
typedef struct rotmos_xml_namespace {
    /** Namespace prefix, not a filesystem/group path. NULL/empty means no
     * prefix where supported.
     */
    const char *prefix;
    /** Namespace URI for an XPath or SAX namespace binding. */
    const char *uri;
} rotmos_xml_namespace;
/** Borrowed SAX attribute name/value/namespace metadata, valid for the current
 * callback.
 */
typedef struct rotmos_xml_attribute {
    /** Name of this declaration, field or listed resource. */
    rotmos_xml_name name;
    /** Field value; byte views may contain embedded NUL where the operation
     * permits it.
     */
    const void *value;
    /** Exact number of bytes; excludes any incidental trailing NUL. */
    size_t length;
} rotmos_xml_attribute;
/** Configuration for xml child. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_xml_child_config {
    /** Namespace URI; NULL/empty means no namespace where supported. */
    const char *namespace_uri;
    /** Namespace prefix, not a filesystem/group path. NULL/empty means no
     * prefix where supported.
     */
    const char *prefix;
    /** Element text bytes or SAX text callback according to the containing
     * type.
     */
    const void *text;
    /** Number of bytes in text, excluding any trailing NUL. */
    size_t text_size;
    /** Nonzero requests element text even when the byte span is empty. */
    int text_present;
} rotmos_xml_child_config;
/** Values for xml xpath kind. */
typedef enum rotmos_xml_xpath_kind {
    /** XPath result representation: nodes. */
    ROTMOS_XML_XPATH_NODES = 1,
    /** XPath result representation: string. */
    ROTMOS_XML_XPATH_STRING = 2,
    /** XPath result representation: number. */
    ROTMOS_XML_XPATH_NUMBER = 3,
    /** XPath result representation: boolean. */
    ROTMOS_XML_XPATH_BOOLEAN = 4
} rotmos_xml_xpath_kind;
/** Owned tagged XPath result. Initialize empty; cleanup releases returned node
 * views and string bytes.
 */
typedef struct rotmos_xml_xpath_result {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    rotmos_xml_xpath_kind kind;
    /** Owned node-view array; release with rotmos_xml_xpath_result_cleanup. */
    rotmos_xml_node **nodes;
    /** Number of elements or requested worker processes, according to the
     * containing type.
     */
    size_t count;
    /** Owned XPath string result; release with the enclosing cleanup helper. */
    rotmos_owned_bytes string;
    /** XPath numeric result when kind is NUMBER. */
    double number;
    /** XPath boolean result when kind is BOOLEAN. */
    int boolean;
} rotmos_xml_xpath_result;
/** Owned materialized DOM, caller-serialized and process/thread-confined.
 * Views represent elements only and retain parent state. Every mutation
 * invalidates previous views, including its receiver. Name strings are borrowed
 * until mutation/close; returned byte copies are caller-owned. DTDs are not
 * supported; external resource loaders are disabled per parser context. */
ROTMOS_API void rotmos_xml_document_config_init(rotmos_xml_document_config *);
/** Parse bytes into an owned bounded DOM; external-resource loading is
 * disabled.
 */
ROTMOS_API rotmos_status rotmos_xml_document_parse(
    const void *, size_t, const rotmos_xml_document_config *,
    rotmos_xml_document **, rotmos_error *);
/** Return an owned element view retaining its document; mutation invalidates
 * previous views, including the receiver.
 */
ROTMOS_API rotmos_status rotmos_xml_document_root(rotmos_xml_document *,
                                                  rotmos_xml_node **,
                                                  rotmos_error *);
/** Evaluate namespace-aware XPath into caller-owned results; clean with
 * rotmos_xml_xpath_result_cleanup.
 */
ROTMOS_API rotmos_status rotmos_xml_document_xpath(
    rotmos_xml_document *, const char *, const rotmos_xml_namespace *, size_t,
    rotmos_xml_xpath_result *, rotmos_error *);
/** Release result node views and owned string storage; clear the initialized
 * result.
 */
ROTMOS_API void rotmos_xml_xpath_result_cleanup(rotmos_xml_xpath_result *);
/** Copy the DOM into caller-owned bytes; the integer selects pretty formatting.
 */
ROTMOS_API rotmos_status rotmos_xml_document_serialize_materialized(
    rotmos_xml_document *, int, rotmos_owned_bytes *, rotmos_error *);
/** Explicitly close the document owner and invalidate its views. Views
 * retain parent allocation state for safe later view cleanup.
 */
ROTMOS_API rotmos_status rotmos_xml_document_close(rotmos_xml_document *,
                                                   rotmos_error *);
/** Borrow local name/namespace/prefix until document mutation or close. */
ROTMOS_API rotmos_status rotmos_xml_node_name(rotmos_xml_node *,
                                              rotmos_xml_name *,
                                              rotmos_error *);
/** Return a caller-owned copy of the element's text content. */
ROTMOS_API rotmos_status rotmos_xml_node_text(rotmos_xml_node *,
                                              rotmos_owned_bytes *,
                                              rotmos_error *);
/** Copy a named optional attribute; present distinguishes an absent attribute
 * from empty text.
 */
ROTMOS_API rotmos_status rotmos_xml_node_attribute(rotmos_xml_node *,
                                                   const char *, const char *,
                                                   rotmos_owned_bytes *, int *,
                                                   rotmos_error *);
/** Replace element text; invalidates all existing views, including this
 * receiver.
 */
ROTMOS_API rotmos_status rotmos_xml_node_set_text(rotmos_xml_node *,
                                                  const void *, size_t,
                                                  rotmos_error *);
/** Set a namespaced attribute from caller bytes; invalidates existing views,
 * including this receiver.
 */
ROTMOS_API rotmos_status rotmos_xml_node_set_attribute(
    rotmos_xml_node *, const char *, const void *, size_t, const char *,
    const char *, rotmos_error *);
/** Append an element with optional namespace/text; returns its owned view and
 * invalidates older views.
 */
ROTMOS_API rotmos_status rotmos_xml_node_append_child(
    rotmos_xml_node *, const char *, const rotmos_xml_child_config *,
    rotmos_xml_node **, rotmos_error *);
/** Remove the element and invalidate all existing document views. */
ROTMOS_API rotmos_status rotmos_xml_node_remove(rotmos_xml_node *,
                                                rotmos_error *);
/** Release an owned node view; safe cleanup does not mutate the document. */
ROTMOS_API rotmos_status rotmos_xml_node_close(rotmos_xml_node *,
                                               rotmos_error *);

/** @file xml.h Public xml API. Expected failures return rotmos_status and
 * optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

/** Xml sax callbacks. */
typedef struct rotmos_xml_sax_callbacks {
    /** SAX callback with names/attributes/namespaces borrowed for the call. */
    rotmos_status (*start_element)(void *, const rotmos_xml_name *,
                                   const rotmos_xml_attribute *, size_t,
                                   const rotmos_xml_namespace *, size_t,
                                   rotmos_error *);
    /** SAX callback with the ending name borrowed for the call. */
    rotmos_status (*end_element)(void *, const rotmos_xml_name *,
                                 rotmos_error *);
    /** Element text bytes or SAX text callback according to the containing
     * type.
     */
    rotmos_status (*text)(void *, const void *, size_t, rotmos_error *);
} rotmos_xml_sax_callbacks;
/** Synchronous bounded SAX flow, no DOM or cumulative stream-size cap. Callback
 * data is borrowed only during the call. Callbacks must not longjmp; failure
 * latches the parser. No callback runs from close. Context remains
 * caller-owned. metadata_limit=0 selects 16 MiB per start event;
 * attrs/declarations total<=256.
 */
ROTMOS_API rotmos_status rotmos_xml_sax_new(const rotmos_xml_sax_callbacks *,
                                            void *, size_t, rotmos_xml_sax **,
                                            rotmos_error *);
/** Synchronously feed a bounded XML chunk; callback data expires on return and
 * failures latch the parser.
 */
ROTMOS_API rotmos_status rotmos_xml_sax_feed(rotmos_xml_sax *, const void *,
                                             size_t, rotmos_error *);
/** Finish incremental XML parsing; report incomplete or failed input without
 * constructing a DOM.
 */
ROTMOS_API rotmos_status rotmos_xml_sax_finish(rotmos_xml_sax *,
                                               rotmos_error *);
/** Release a SAX parser without invoking callbacks. */
ROTMOS_API rotmos_status rotmos_xml_sax_close(rotmos_xml_sax *, rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif
