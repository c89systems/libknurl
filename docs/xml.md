# XML mapping, routes, DOM and SAX

> Implementation reference under correction. The [extraction contract](server-api-spec.md) governs required behavior and scope; this document describes current code, not compatibility or an additional product requirement. See the [audit](server-api-audit.md) for known failures.


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

Mapped codecs use released public LoneJSON schemas and record views from
`<rotmos/lonejson.h>`. `rotmos_xml_parse_source` consumes a synchronous source to
EOF without closing it. The caller supplies initialized mapped storage; parsing
resets it before use and on failure, preserving fixed array backing. Dynamic
strings and arrays are assigned through public LoneJSON APIs. No allocator
headers or language-binding layouts are copied. A supplied schema runtime owns
its allocation/spool policy; a NULL runtime creates an operation-owned runtime
with mapped text bounds. Returned records remain caller-owned and require
`lonejson_cleanup`.

`rotmos_xml_config_init` selects nesting 64 and text 67108864 bytes. Zero limits
select these defaults. The mapped text bound is separate from the DOM's 16 MiB
bound and from HTTP body policy. Root matching uses local names, including
namespaced elements. `attribute_prefix` selects attribute fields, with plain-name
fallback on input; `text_key` selects mixed text. Contiguous repeated elements
populate arrays. Preserve trimming, `xml:space`, CDATA, deferred formatting
whitespace, required fields and duplicate/noncontiguous-array rejection.
`skip_unknown_disabled` rejects unknown fields when true. Spooled text requires
`trim_text=0` and no presence flag. DTD/entity declarations and external resources
are rejected per reader. Errors preserve dependency codes/locations and failed
parses retire partial allocations.

`rotmos_router_xml_route` and its route-group counterpart copy declarations and
mapping option strings, borrowing maps/runtime/userdata through route destruction.
The handler receives a request-owned public input record view and a general
reply builder, matching Vectis's XML route boundary. It can borrow input into an
XML source reply, synchronously materialize a stack output record, or choose any
other reply kind/status. Records survive asynchronous response output and staged
replacement. Before/auth/after hooks follow the ordinary staged route order; an
unselected reply defaults to 204. Routes use live STREAMING_UPLOAD/STREAM input.
`buffer_bytes` bounds the existing upload-reader queue (zero selects 256 KiB);
no full transport body or spool directory is needed. Parsing materializes the
declared record, not an extra copy of the HTTP body. `rotmos_request_xml_into`
remains a separate operation on explicitly completed bodies, with rewind.

The parser runs natively; completion and the request/reply handler run on the
owner loop after input EOF and native acknowledgement, before after-hooks.
Vectis's baseline invokes its XML handler on the reader thread while its owner
joins that thread at EOF (`vectis_upload_reader_thread_main` and
`vectis_upload_reader_finish` in [vectis.c](../../vectis/src/vectis.c)). This
extraction moves the rich request callback to its owning loop while retaining
that dispatch phase, avoiding foreign-thread request/Lua operations and an
owner-loop join. Explicit native-reader handlers keep their native contract.
Mapped field callbacks/allocators used by the parser must support native work;
LoneJSON permits shared runtimes with thread-safe allocator callbacks. Close
revokes the request immediately; parser state remains retained until native ACK.

The [XML reference adapter](../tests/vectis-forward.c) forwards the baseline
app/request/raw-record/reply/userdata shape through public registrations. Its
[wire fixture](../tests/xml-runtime.c) withholds the document end until a public
LoneJSON allocator observes native field parsing. It also checks no transport
body store, owner-loop callbacks, early malformed input, disconnect while
parsing, a tiny queue, source replacement, auth/limits, empty 204 and handler 501.
The Lua typed-route binding uses this same C registration, supplying a pinned
Lua-owned record through the optional public record provider. Its native parser
receives only public record/schema pointers; the Lua handler runs on the owner
loop and retained Lua records survive completion. Providers receive a release
for each acquire attempt, including partial failure and disconnect during parsing.

`rotmos_xml_generator_new/read/destroy` provide resumable output. Each read
advances an iterative traversal using public libxml2 writer calls, then flushes
a bounded unit before proceeding. It retains nesting frames, libxml2's writer
state and a 32 KiB output unit; it never collects a document or uses an output
staging file. Text units contain at most 2048 input bytes, retaining split UTF-8
characters between reads. XML names are application metadata bounded to 4096
bytes each. Text escaping and element/attribute state belong to libxml2. UTF-8,
XML character, nesting and text bounds are checked as traversal advances; an
invalid later field can therefore fail after earlier bytes have been delivered.
Errors latch. Positive reads return OK; completed output returns EOF with zero
bytes, including on repeated reads.

The generator copies public record/config descriptors and borrows their referenced
storage through destroy. Callers keep logical record values/schema/options
unchanged. Spooled text uses its public sequential cursor and requires exclusive
read access until close; its cursor is not restored. Handles, and source/reply
wrappers around them, must be created, consumed and closed on their owning process
and thread. A release callback runs exactly once after successful construction,
with OK after EOF, CANCELLED before completion, or the terminal failure. Failed
construction takes no ownership. Release callbacks cannot longjmp or recursively
operate on the closing generator.

`rotmos_source_xml` and `rotmos_reply_xml` connect the same generator to the
ordinary source/HTTP response path. Failed source/reply registration takes no
release notification. `rotmos_xml_write_sink` synchronously drains that generator;
the sink must accept a complete chunk or fail. `rotmos_xml_serialize_materialized`
and `rotmos_reply_xml_materialized` explicitly collect output with an optional
bound; zero adds no ceiling. Stack records are safe with these synchronous calls. Returned byte
spans require their lengths and `rotmos_owned_bytes_cleanup`.

The [mapped fixture](../tests/xml-mapped.c) covers Vectis mapping behavior,
namespace/local-name matching, attributes, mixed text, repeated object/scalar
arrays, integer edges, fixed-array overflow and cleanup, input errors at EOF,
DTD rejection, mapped text beyond 10 MiB, split UTF-8, one-byte output reads,
spilled text, cancellation, failed ownership transfer and deferred output errors.
It runs on a default-stack pthread. The [wire fixture](../tests/xml-runtime.c)
covers grouped registration, copied options, memory/file bodies, rewind, output,
malformed input, auth short circuit, body limits and staged replacement. Both
fixtures run against relocated static/shared installed SDKs.

The [Lua facade](lua.md) binds mapped `parse`/`parse_record`, materialized/live
output, request-owned parsing and `server/group:xml_route` through the public C
codecs and LoneJSON interop. Data/path inputs, root/text/attribute options,
trimming, unknown-field precedence and separate mapped limits follow Vectis.
The [installed typed fixture](../tests/lua/typed.lua) covers these paths and copied
route options. Owned Lua document/XPath/element and incremental SAX bindings lift
Vectis's object facade through these public operations. Their
[ported fixture](../tests/lua/xml.lua) covers mutation, retained owners, result
cleanup, callback identity/reentry and Lua allocation failure. Node finalizers
retire independent views during other queries without closing the active DOM.
