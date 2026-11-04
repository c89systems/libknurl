# XML DOM, XPath and SAX

Include [`<rotmos/xml.h>`](../include/rotmos/xml.h), or the ordinary umbrella.
Opaque document, node and SAX handles keep libxml2 layouts private. The selected
cpkt libxml2 supplies the implementation. This surface lifts Vectis's generic
DOM/SAX code under the repository's MIT license.

`rotmos_xml_document_parse` creates an explicitly materialized owned DOM from a
bounded input span. The input is copied by parsing and can be released afterward.
Document defaults are 16 MiB and 4096 returned XPath nodes. Root and XPath node
views retain the document state. Closing the document releases its native DOM;
remaining views become unusable and still require their own close. Every mutation
invalidates all previous views, including the mutation receiver. Obtain a fresh
root or XPath result after a change.

Element names expose borrowed local-name, namespace-URI and prefix strings.
Text/attribute reads and document serialization return independent owned byte
values. A found flag distinguishes an absent attribute from an empty one. Use
`rotmos_owned_bytes_cleanup` to release byte values and
`rotmos_xml_xpath_result_cleanup` to release a result's node views/string.
Values are byte spans; callers use their lengths rather than assuming NUL
termination.

XPath expressions and namespace bindings are application declarations. Results
can contain element views, strings, numbers or booleans. Non-element node sets
are rejected; scalar XPath functions cover attribute/text queries. The returned-
node bound does not bound XPath's intermediate allocation or make evaluation
preemptible. DOM serialization is materialized and separately bounded by the
document byte limit.

Text/attribute mutation validates XML UTF-8, names and namespace bindings. Child
append and removal preserve namespace ownership; removal cannot delete the root.
DOM and SAX objects belong to their creating process and thread. Foreign use or
close returns STATE rather than dereferencing inherited native objects.

`rotmos_xml_sax_new/feed/finish/close` provide incremental input without a DOM or
cumulative stream-size cap. Feed offers at most 65536 bytes. Callback spans are
borrowed until return; text callbacks receive bounded chunks. Start events permit
at most 256 total attributes/namespace declarations, with a separately configured
metadata bound defaulting to 16 MiB. The callback table is copied and userdata is
borrowed until close. Callbacks are synchronous and cannot longjmp through
libxml2. Callbacks accept with OK or fail terminally; AGAIN/EOF are invalid.
Failure or reentry/close during a callback latches an error. Finish
validates EOF, and close never invokes application callbacks.

Both parsers disable external resource loading per parser context and reject DTD
declarations. They do not change libxml2's global error/resource handlers. Parser
errors retain the XML native code and location in the public error.

The [ported public fixture](../tests/xml.c) checks namespace-aware queries,
empty attributes, scalar results, mutation invalidation, owner close, malformed
input, DTD rejection, document/XPath/metadata bounds, SAX callback failure/reentry,
input exceeding 16 MiB without a DOM, and process/thread confinement. DOM/SAX work
also runs on a newly created default-stack thread, including on musl. The same
fixture runs against relocated static/shared installed SDKs.

Mapped LoneJSON parsing, mapped routes, synchronous sink output and the resumable
`rotmos_xml_generator_new/read/destroy` remain the next XML surface under the
[complete specification](server-api-spec.md#fast-typed-json-and-xmllibxml2-routes).
Their separate mapped limits remain nesting 64 and text 64 MiB. The DOM's 16 MiB
limit does not replace them. A synchronous writer or complete DOM serialization
cannot substitute for the required resumable output generator.
