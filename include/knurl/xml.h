#ifndef KNURL_XML_H
#define KNURL_XML_H
#include <knurl/common.h>
#include <knurl/io.h>
#include <knurl/router.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Default materialized DOM input bound: 16 MiB. */
#define KNURL_XML_DEFAULT_MAX_BYTES 16777216u
/** Default maximum element views returned by one DOM XPath query: 4096. */
#define KNURL_XML_DEFAULT_MAX_XPATH_NODES 4096u
/** Maximum bytes in one SAX feed/text chunk: 65536; no cumulative stream-size
 * ceiling.
 */
#define KNURL_XML_SAX_CHUNK_BYTES 65536u
/** Maximum combined attributes/namespace declarations per SAX start event: 256.
 */
#define KNURL_XML_SAX_MAX_ATTRIBUTES 256u
/** Owned materialized XML DOM; element views retain its state. */
typedef struct knurl_xml_document knurl_xml_document;
/** Owned element view; every document mutation invalidates previous views. */
typedef struct knurl_xml_node knurl_xml_node;
/** Owned incremental SAX parser; feed/finish synchronously invoke borrowed
 * callbacks.
 */
typedef struct knurl_xml_sax knurl_xml_sax;
struct lonejson_schema_view;
struct lonejson_record_view;
/** Optional typed-record owner adapter. acquire and release must both be
 * supplied; release runs for every acquire attempt, including partial failure.
 * Callbacks run on the owner loop and use the route handler userdata. XML
 * storage remains pinned through native parser acknowledgement.
 */
struct knurl_record_provider;
/** Owned bounded XML pull generator retaining its mapped record owner. */
typedef struct knurl_xml_generator knurl_xml_generator;
/** Configuration for xml. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_xml_config {
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
    /** Mapped XML nesting limit; default 64, with zero selecting that default.
     * DOM and SAX limits are separate.
     */
    size_t max_depth;
    /** Mapped XML text byte bound; default 64 MiB (67108864 bytes), with zero
     * selecting that default. Independent of DOM and HTTP body limits.
     */
    size_t max_text_bytes;
} knurl_xml_config;
/** Initialize xml config with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_xml_config_init(knurl_xml_config *);
/** Synchronous mapped input. Borrows source; consumes to EOF, never closes it.
 * AGAIN fails; live blocking readers execute on native threads. Caller supplies
 * initialized storage; reset before parse and on failure, preserving fixed
 * array backing. Maps and optional runtime use the released public LoneJSON
 * schema interface. */
KNURL_API knurl_status
knurl_xml_parse_source(knurl_source *, const struct lonejson_schema_view *,
                       const knurl_xml_config *, void *, knurl_error *);
/** Parse a completed request body into caller-initialized mapped storage;
 * rewind before and after. See knurl_xml_parse_source.
 */
KNURL_API knurl_status
knurl_request_xml_into(knurl_request *, const struct lonejson_schema_view *,
                       const knurl_xml_config *, void *, knurl_error *);
/** Configuration for xml source. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_xml_source_config {
    /** Mapped record/view; retain backing storage according to the codec's
     * materialized/live contract.
     */
    const struct lonejson_record_view *record;
    /** Mapped XML configuration; DOM/SAX limits are separate. */
    knurl_xml_config xml;
    /** Exactly-once owner-loop record-owner release after successful
     * construction; failure takes nothing. Native work acknowledges before
     * release.
     */
    knurl_source_close_fn release;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_xml_source_config;
/** Initialize xml source config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_xml_source_config_init(knurl_xml_source_config *);
/** Resumable bounded output, no whole-output collection. XML names <=4096
 * bytes; text units <=2048 bytes, independent of mapped text limits. Success
 * copies view and config declarations and borrows their storage unchanged until
 * release. Spooled text uses its public sequential cursor: exclusive read
 * access until close. Failed construction takes nothing. Handles are
 * process/thread-confined.
 */
KNURL_API knurl_status knurl_xml_generator_new(const knurl_xml_source_config *,
                                               knurl_xml_generator **,
                                               knurl_error *);
/** Pull a bounded XML output chunk; EOF completes generation, without
 * collecting the whole output.
 */
KNURL_API knurl_status knurl_xml_generator_read(knurl_xml_generator *, void *,
                                                size_t, size_t *,
                                                knurl_error *);
/** Close the generator and release its retained record owner once. */
KNURL_API knurl_status knurl_xml_generator_destroy(knurl_xml_generator *,
                                                   knurl_error *);
/** Wrap a bounded XML generator as an owned source; record storage remains
 * borrowed until release.
 */
KNURL_API knurl_status knurl_source_xml(const knurl_xml_source_config *,
                                        knurl_source **, knurl_error *);
/** Stage an explicit live XML source; record storage remains borrowed until
 * terminal source release.
 */
KNURL_API knurl_status knurl_reply_xml(knurl_reply *, int,
                                       const knurl_xml_source_config *,
                                       knurl_error *);
/** Synchronously accept the complete offered XML chunk with OK or fail
 * terminally. Chunk bytes are borrowed only during the call.
 */
typedef knurl_status (*knurl_xml_sink_fn)(const void *, size_t, void *,
                                          knurl_error *);
/** Sink must accept the complete offered chunk with OK or fail terminally. */
KNURL_API knurl_status knurl_xml_write_sink(const struct lonejson_record_view *,
                                            const knurl_xml_config *,
                                            knurl_xml_sink_fn, void *,
                                            knurl_error *);
/** Explicit output collection; max_bytes=0 adds no ceiling. Empty out required.
 */
KNURL_API knurl_status knurl_xml_serialize_materialized(
    const struct lonejson_record_view *, const knurl_xml_config *, size_t,
    knurl_owned_bytes *, knurl_error *);
/** Serialize XML synchronously into a copied staged reply; no record/schema
 * lifetime extends beyond return.
 */
KNURL_API knurl_status knurl_reply_xml_materialized(
    knurl_reply *, int, const struct lonejson_record_view *,
    const knurl_xml_config *, size_t, knurl_error *);
/** Owner-loop handler after bounded native XML parsing. Borrow the mapped
 * record and select a staged reply; no transport body was collected.
 */
typedef knurl_status (*knurl_xml_route_fn)(knurl_request *,
                                           const struct lonejson_record_view *,
                                           knurl_reply *, void *,
                                           knurl_error *);
/** Configuration for xml route. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_xml_route_config {
    /** Single HTTP method; methods supplies the explicit multi-method mask when
     * nonzero.
     */
    knurl_http_method method;
    /** HTTP method mask; zero uses this operation's single-method/default
     * policy.
     */
    knurl_http_methods methods;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    knurl_route_path_kind path_kind;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    knurl_body_policy body;
    /** Bounded live-upload queue/chunk capacity; zero selects the operation's
     * documented default.
     */
    size_t buffer_bytes;
    /** Borrowed input schema; an absent schema supplies no mapped input view.
     */
    const struct lonejson_schema_view *input;
    /** Mapped XML configuration; DOM/SAX limits are separate. */
    knurl_xml_config xml;
    /** Application handler invoked under this route's documented execution and
     * lifetime rules.
     */
    knurl_xml_route_fn handler;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    knurl_route_hooks hooks;
    /** Optional copied record-owner adapter; its callbacks/storage obey
     * knurl_record_provider.
     */
    const struct knurl_record_provider *records;
} knurl_xml_route_config;
/** Initialize xml route config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_xml_route_config_init(knurl_xml_route_config *);
/** Live input uses the bounded upload-reader queue (buffer_bytes=0: 256 KiB).
 * Native parsing retains no live request/Lua handle; the completed record and
 * handler return to the owner loop. Records survive response cleanup. Ordinary
 * hooks and empty-reply 204 behavior apply. No transport body is collected.
 * Borrowed runtime allocators and mapped field callbacks used by parsing must
 * support native threads, as with the underlying upload-reader contract. */
KNURL_API knurl_status knurl_router_xml_route(knurl_router *,
                                              const knurl_xml_route_config *,
                                              knurl_route **, knurl_error *);
/** Register a mapped XML live-input route under the group prefix. The bounded
 * native parser returns the mapped record to owner-loop handlers; retain
 * schemas/userdata through router destruction.
 */
KNURL_API knurl_status
knurl_route_group_xml_route(knurl_route_group *, const knurl_xml_route_config *,
                            knurl_route **, knurl_error *);
/** Owned-DOM input/result limits, initialized by
 * knurl_xml_document_config_init; separate from mapped and SAX bounds.
 */
typedef struct knurl_xml_document_config {
    /** Maximum bytes for this operation; zero-inheritance/unlimited behavior is
     * operation-specific.
     */
    size_t max_bytes;
    /** Maximum DOM XPath node results. */
    size_t max_xpath_nodes;
} knurl_xml_document_config;
/** Borrowed XML local name, namespace URI and prefix; lifetime follows the
 * producing node/event.
 */
typedef struct knurl_xml_name {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Namespace URI; NULL/empty means no namespace where supported. */
    const char *namespace_uri;
    /** Namespace prefix, not a filesystem/group path. NULL/empty means no
     * prefix where supported.
     */
    const char *prefix;
} knurl_xml_name;
/** One namespace prefix/URI binding for XPath or a borrowed SAX event. */
typedef struct knurl_xml_namespace {
    /** Namespace prefix, not a filesystem/group path. NULL/empty means no
     * prefix where supported.
     */
    const char *prefix;
    /** Namespace URI for an XPath or SAX namespace binding. */
    const char *uri;
} knurl_xml_namespace;
/** Borrowed SAX attribute name/value/namespace metadata, valid for the current
 * callback.
 */
typedef struct knurl_xml_attribute {
    /** Name of this declaration, field or listed resource. */
    knurl_xml_name name;
    /** Field value; byte views may contain embedded NUL where the operation
     * permits it.
     */
    const void *value;
    /** Exact number of bytes; excludes any incidental trailing NUL. */
    size_t length;
} knurl_xml_attribute;
/** Configuration for xml child. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_xml_child_config {
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
} knurl_xml_child_config;
/** Values for xml xpath kind. */
typedef enum knurl_xml_xpath_kind {
    /** XPath result representation: nodes. */
    KNURL_XML_XPATH_NODES = 1,
    /** XPath result representation: string. */
    KNURL_XML_XPATH_STRING = 2,
    /** XPath result representation: number. */
    KNURL_XML_XPATH_NUMBER = 3,
    /** XPath result representation: boolean. */
    KNURL_XML_XPATH_BOOLEAN = 4
} knurl_xml_xpath_kind;
/** Owned tagged XPath result. Initialize empty; cleanup releases returned node
 * views and string bytes.
 */
typedef struct knurl_xml_xpath_result {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    knurl_xml_xpath_kind kind;
    /** Owned node-view array; release with knurl_xml_xpath_result_cleanup. */
    knurl_xml_node **nodes;
    /** Number of elements or requested worker processes, according to the
     * containing type.
     */
    size_t count;
    /** Owned XPath string result; release with the enclosing cleanup helper. */
    knurl_owned_bytes string;
    /** XPath numeric result when kind is NUMBER. */
    double number;
    /** XPath boolean result when kind is BOOLEAN. */
    int boolean;
} knurl_xml_xpath_result;
/** Owned materialized DOM, caller-serialized and process/thread-confined.
 * Views represent elements only and retain parent state. Every mutation
 * invalidates previous views, including its receiver. Name strings are borrowed
 * until mutation/close; returned byte copies are caller-owned. DTDs are not
 * supported; external resource loaders are disabled per parser context. */
KNURL_API void knurl_xml_document_config_init(knurl_xml_document_config *);
/** Parse bytes into an owned bounded DOM; external-resource loading is
 * disabled.
 */
KNURL_API knurl_status knurl_xml_document_parse(
    const void *, size_t, const knurl_xml_document_config *,
    knurl_xml_document **, knurl_error *);
/** Return an owned element view retaining its document; mutation invalidates
 * previous views, including the receiver.
 */
KNURL_API knurl_status knurl_xml_document_root(knurl_xml_document *,
                                               knurl_xml_node **,
                                               knurl_error *);
/** Evaluate namespace-aware XPath into caller-owned results; clean with
 * knurl_xml_xpath_result_cleanup.
 */
KNURL_API knurl_status knurl_xml_document_xpath(
    knurl_xml_document *, const char *, const knurl_xml_namespace *, size_t,
    knurl_xml_xpath_result *, knurl_error *);
/** Release result node views and owned string storage; clear the initialized
 * result.
 */
KNURL_API void knurl_xml_xpath_result_cleanup(knurl_xml_xpath_result *);
/** Copy the DOM into caller-owned bytes; the integer selects pretty formatting.
 */
KNURL_API knurl_status knurl_xml_document_serialize_materialized(
    knurl_xml_document *, int, knurl_owned_bytes *, knurl_error *);
/** Explicitly close the document owner and invalidate its views. Views
 * retain parent allocation state for safe later view cleanup.
 */
KNURL_API knurl_status knurl_xml_document_close(knurl_xml_document *,
                                                knurl_error *);
/** Borrow local name/namespace/prefix until document mutation or close. */
KNURL_API knurl_status knurl_xml_node_name(knurl_xml_node *, knurl_xml_name *,
                                           knurl_error *);
/** Return a caller-owned copy of the element's text content. */
KNURL_API knurl_status knurl_xml_node_text(knurl_xml_node *,
                                           knurl_owned_bytes *, knurl_error *);
/** Copy a named optional attribute; present distinguishes an absent attribute
 * from empty text.
 */
KNURL_API knurl_status knurl_xml_node_attribute(knurl_xml_node *, const char *,
                                                const char *,
                                                knurl_owned_bytes *, int *,
                                                knurl_error *);
/** Replace element text; invalidates all existing views, including this
 * receiver.
 */
KNURL_API knurl_status knurl_xml_node_set_text(knurl_xml_node *, const void *,
                                               size_t, knurl_error *);
/** Set a namespaced attribute from caller bytes; invalidates existing views,
 * including this receiver.
 */
KNURL_API knurl_status knurl_xml_node_set_attribute(knurl_xml_node *,
                                                    const char *, const void *,
                                                    size_t, const char *,
                                                    const char *,
                                                    knurl_error *);
/** Append an element with optional namespace/text; returns its owned view and
 * invalidates older views.
 */
KNURL_API knurl_status knurl_xml_node_append_child(
    knurl_xml_node *, const char *, const knurl_xml_child_config *,
    knurl_xml_node **, knurl_error *);
/** Remove the element and invalidate all existing document views. */
KNURL_API knurl_status knurl_xml_node_remove(knurl_xml_node *, knurl_error *);
/** Release an owned node view; safe cleanup does not mutate the document. */
KNURL_API knurl_status knurl_xml_node_close(knurl_xml_node *, knurl_error *);

/** @file xml.h Public xml API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

/** Xml sax callbacks. */
typedef struct knurl_xml_sax_callbacks {
    /** SAX callback with names/attributes/namespaces borrowed for the call. */
    knurl_status (*start_element)(void *, const knurl_xml_name *,
                                  const knurl_xml_attribute *, size_t,
                                  const knurl_xml_namespace *, size_t,
                                  knurl_error *);
    /** SAX callback with the ending name borrowed for the call. */
    knurl_status (*end_element)(void *, const knurl_xml_name *, knurl_error *);
    /** Element text bytes or SAX text callback according to the containing
     * type.
     */
    knurl_status (*text)(void *, const void *, size_t, knurl_error *);
} knurl_xml_sax_callbacks;
/** Synchronous bounded SAX flow, no DOM or cumulative stream-size cap. Callback
 * data is borrowed only during the call. Callbacks must not longjmp; failure
 * latches the parser. No callback runs from close. Context remains
 * caller-owned. metadata_limit=0 selects 16 MiB per start event;
 * attrs/declarations total<=256.
 */
KNURL_API knurl_status knurl_xml_sax_new(const knurl_xml_sax_callbacks *,
                                         void *, size_t, knurl_xml_sax **,
                                         knurl_error *);
/** Synchronously feed a bounded XML chunk; callback data expires on return and
 * failures latch the parser.
 */
KNURL_API knurl_status knurl_xml_sax_feed(knurl_xml_sax *, const void *, size_t,
                                          knurl_error *);
/** Finish incremental XML parsing; report incomplete or failed input without
 * constructing a DOM.
 */
KNURL_API knurl_status knurl_xml_sax_finish(knurl_xml_sax *, knurl_error *);
/** Release a SAX parser without invoking callbacks. */
KNURL_API knurl_status knurl_xml_sax_close(knurl_xml_sax *, knurl_error *);
#ifdef __cplusplus
}
#endif
#endif
