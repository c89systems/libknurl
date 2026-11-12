# SOAP 1.2 server implementation specification

Status: planned extension, 2026-11-13. This document specifies future work;
the SOAP APIs described below do not exist yet. It does not change the current
[extraction contract](server-api-spec.md) or authorize implementation or release.
Implementation must incorporate the delivered SOAP scope into that contract.

## Goal and initial scope

Provide SOAP 1.2 services through libknurl's existing HTTP server, router,
request/reply lifecycle and XML implementation. Implement protocol behavior once
in strict C89 and bind it through the host's existing Lua VM. Public C receivers
and Lua methods must expose equivalent capabilities and error behavior.

The first delivery is an ultimate-receiver endpoint supporting document/literal
request-response over HTTP POST. One HTTP route owns an explicit operation
table; one qualified document element in the SOAP Body selects an operation.
Every successfully handled request produces a SOAP response envelope. This is
an application profile of SOAP 1.2, not a claim to implement every SOAP feature.

Assume a new service whose clients can use SOAP 1.2. Existing clients requiring
SOAP 1.1 or a broader WSDL contract require a separate scope decision.

Excluded from this delivery: SOAP 1.1 processing/version negotiation, SOAP RPC
encoding, SOAP intermediaries and forwarding, custom roles, one-way/GET message
exchange patterns, client APIs, WSDL generation/import, XSD validation, WS-Addressing,
WS-Security, signatures, encryption, MTOM/XOP and multipart attachments.
Applications may serve separately maintained WSDL/XSD files as static assets;
that does not imply runtime schema validation or generated bindings.

## References and existing boundaries

Use the current W3C Recommendations and their published errata:

- [SOAP 1.2 Part 1: Messaging Framework](https://www.w3.org/TR/soap12-part1/)
  for message structure, namespace identity, roles, mandatory headers and faults.
- [SOAP 1.2 Part 2: Adjuncts](https://www.w3.org/TR/soap12-part2/)
  for the HTTP binding and fault/status mapping.
- [RFC 3902](https://www.rfc-editor.org/rfc/rfc3902.html)
  for `application/soap+xml` and its action parameter.
- [Existing XML behavior](xml.md), [request/body lifecycle](requests.md),
  [reply/source ownership](responses.md) and [Lua binding](lua.md).

SOAP 1.2's published envelope namespace is
`http://www.w3.org/2003/05/soap-envelope`. This identifier is independent of
libknurl's release version and must not change with library releases.

Keep Kore private. libxml2 owns XML tokenization, namespace resolution and
escaping. LoneJSON owns public map/record storage and assignments. SOAP owns
envelope semantics, qualified operation/binding lookup and fault composition.
Use the existing upload reader, backpressure, cancellation and source machinery.
Do not introduce a second HTTP router, parser, Lua runtime or dependency stack.

The current mapped XML reader matches local names. Wrapping an existing
`xml_route` and checking a string named `Envelope` is insufficient. SOAP requires
a namespace-aware input cursor and output writer, including payload fields.
Factor reusable XML internals where appropriate, preserving existing XML routes'
observable behavior. Never strip prefixes, rewrite XML text or serialize a
subtree and parse it again to bridge the two implementations.

## HTTP contract

The SOAP endpoint accepts POST with one `application/soap+xml` Content-Type.
Parse media types through the existing validated header machinery; media type
and parameter names are case-insensitive. Malformed or duplicate singleton
headers/parameters fail before operation dispatch. Parameters must not silently
shadow each other. Invalid action syntax is an HTTP media-type error (400);
an otherwise valid action that disagrees with the selected operation is a SOAP
Sender fault.

Support XML 1.0 input in UTF-8 and UTF-16 using libxml2's encoding facilities.
Honor a supplied charset and reject conflicting charset/BOM/XML declarations.
An unsupported charset is HTTP 415. Output is UTF-8 and uses
`application/soap+xml; charset=utf-8`, including SOAP fault responses.

The optional `action` parameter must be a nonempty absolute URI. The operation
table may declare one request action and one response action per operation.
Request dispatch always uses the Body document's expanded name. When action is
present it must match that operation's declared request action; an undeclared or
mismatching action is a Sender fault. An absent action remains valid. An action
must never select a different operation than the Body name. Emit a response
action parameter only when declared. Ignore the legacy `SOAPAction` header for
dispatch; it cannot substitute for the media-type action parameter.

Transport admission, authentication and before hooks retain existing ordering.
An HTTP-layer rejection may use an ordinary HTTP reply without a SOAP envelope.
The initial endpoint does not implement GET or empty-success acknowledgements.
Method rejection must produce 405 with `Allow: POST`; shared-path routing must
not accidentally convert that case into 404 or override a separately registered
ordinary route for another method. Existing malformed-HTTP, body-limit, timeout,
disconnect and unread-input drain/close behavior continues to apply.

`text/xml`, `application/xml`, multipart messages and unsupported content
encodings receive 415 before SOAP parsing. There is no SOAP 1.1 binding or
Appendix A version-transition mode. An envelope version mismatch inside an
accepted SOAP 1.2 representation follows the fault contract below; it must not
silently activate SOAP 1.1 processing.

## Envelope and qualified-name rules

Compare expanded names as the exact namespace URI and local name. Prefixes are
presentation only; accept equivalent prefixes, default namespaces and legal
namespace rebinding. Resolve QName-valued attributes/text in their own scope.
An unbound prefix or wrong namespace cannot match a declaration by local name.

The envelope contains optional Header followed by exactly one Body. This profile
requires exactly one operation document inside Body, or one Fault in a fault
response. Reject duplicate, reordered or additional envelope children, empty or
multiple operation documents, unexpected structural text and trailing documents.
An inbound Fault is not a service invocation. Accept legal XML whitespace and
comments inside the envelope; reject processing instructions and comments
outside the document element. An XML declaration is not a processing instruction.

Reject DTDs and external-resource resolution using parser-local settings. Apply
the same rule to skipped optional header/payload subtrees. Do not relax these
settings, XML character checks or resource limits for SOAP.

Document/literal payloads do not use SOAP encoding. Accept an absent
`encodingStyle` or the standard explicit no-encoding URI where that attribute
is legal. Other styles on processed data yield DataEncodingUnknown. Illegal
placement yields Sender. Ignored header subtrees do not become processed data
merely because they carry an encodingStyle declaration.

## Headers and processing order

The endpoint assumes the standard ultimateReceiver and next roles. Missing role
means ultimateReceiver. Blocks targeting none or another role are not processed;
their mandatory flag alone does not cause a fault. No message is forwarded.
Header blocks must be namespace-qualified. Validate the SOAP-defined role,
mustUnderstand and relay attributes and their lexical forms; accept both valid
boolean spellings (`true`/`1`, `false`/`0`). Relay does not enable forwarding.

Applications may register header handlers by expanded name, with an input schema
and namespace binding. Registration means the application implements that
header's semantics; recognizing its element name alone does not establish that
the header was understood. Reject repeated targeted blocks of the same name in
this initial profile. Unknown optional blocks may be skipped within all limits.

Before executing any application header or operation callback, determine whether
all targeted mandatory blocks have a registered handler. If any do not, generate
one MustUnderstand fault, with bounded NotUnderstood metadata identifying the
offending expanded names, and execute no application handler. This check takes
precedence over operation lookup and mapped header/payload-validation faults;
record those failures until mandatory-header inventory is complete. Fatal XML
structure errors may stop parsing immediately. After full-envelope validation,
process registered targeted blocks in document order, stopping on failure or an
explicit fault, then invoke the operation once. Record allocation is not a
business callback.

## Operation declarations and payload mapping

Each operation declares its input document name, input schema/binding, output
document name, output schema/binding, optional request/response actions, handler
and userdata. Names and bindings are explicit application metadata. The profile
requires both input and output documents; operations with empty logical results
declare an empty output record/document.

Reject an empty operation table, duplicate input document names, missing handlers,
invalid schema/binding combinations and invalid declared actions at registration.
Validate the entire declaration before adding the endpoint to the router; failure
must leave the registry unchanged. Operations share the endpoint's HTTP admission
and auth/before/after hooks; operation-specific checks run after decoding.

Use released public LoneJSON schema and record views. Add an XML binding
descriptor that associates a map field with an expanded element or attribute
name, or the element's text value. Child-map bindings recurse; array bindings
identify the repeated element. Element fields default to the binding's declared
payload namespace and map key; unqualified fields require an explicit empty
namespace. Attributes default to no namespace. Keys that are not legal XML local
names require explicit bindings. No inference from a colon-containing prefix or
an `@`-prefixed key may bypass an explicit binding.

Bindings must cover every mapped field, either through those defaults or an
explicit override. Reject colliding names/kinds, invalid names, multiple text
bindings and unsupported field representations during registration. Preserve
LoneJSON ownership and supported XML scalar/array/spool semantics. A field binding
defaults to `nullable = false`; registration may enable it only when the declared
record representation distinguishes null from absence. Required values must
exist. `xsi:nil="true"`/`"1"` requires that nullable binding and empty element
content; `"false"`/`"0"` follows ordinary value parsing. Invalid lexical values
fault. The `nil` attribute must use the XML Schema instance namespace. No XSD
validation is implied.

Unknown payload fields are rejected by default. An explicit
`unknown_fields_allowed = true` permits bounded subtree skipping; it never
relaxes envelope, namespace, header or required-field checks. Application range,
format and business checks execute after structural decoding. Schema validation
does not sanitize a value for HTML, SQL, filesystem or command use.

An unknown operation name or a payload mapping/validation failure produces a
Sender fault and never calls the operation handler. Output uses the registered
output name/binding, preventing a handler from accidentally answering with a
different operation's document.

## Public C and Lua surface

The following names are planned API requirements, not callable examples of the
current release. Put the SOAP declarations in `<knurl/soap.h>`, independent of
private backend/dependency types. Include LoneJSON's public declarations only
where schema/record interfaces require them.

| Surface | Planned operation and contract |
| --- | --- |
| Declarations | Initializers for `knurl_soap_route_config`, operation/binding/header declarations and fault/result configuration. Initializers establish all defaults. |
| Router | `knurl_router_soap_route` and `router->soap_route(router, &config, &route, &error)` register one endpoint and its operation table. |
| Group | `knurl_route_group_soap_route` and `group->soap_route(...)` apply the existing prefix/hooks. |
| Operation callback | Receives the request, a callback-scoped SOAP call view, the typed input record, the staged reply, userdata and error output. |
| Call view | Exposes the selected operation, supplied action and processed header record views; it contains no live XML cursor or private libxml2 data. |
| Replies | `knurl_reply_soap_result` / `reply->soap_result(...)` select the registered operation's live output record; `knurl_reply_soap_fault` / `reply->soap_fault(...)` select an explicit fault. |
| Lua | `server:soap_route{...}` and `group:soap_route{...}` register the same C declarations; no second protocol implementation. |

C callbacks return KNURL_OK after selecting one result or fault. Result
configuration supplies the public record view and its existing-style release
callback/userdata; successful staging pins it through source cleanup. Failure
preserves the previous reply and takes no ownership. Provide an explicitly named
`soap_result_materialized` operation if callers need synchronous output from
stack records; it must be documented as materialized and have an explicit bound.
Do not make materialization the implementation of the default result helper.

The call view binds result selection to the current operation. A successful
operation returning an unselected reply is a Receiver fault, not implicit 204.
Returning KNURL_INVALID from application validation maps to Sender; other
unexpected callback failures map to Receiver. Explicit fault selection is the
way to supply a public application reason/subcode/detail. Raw non-SOAP responses
belong to HTTP auth/before short-circuit paths, not successful operation results.

Lua declarations use the same operation/input/output/binding concepts. The
operation handler is `handler(request, input_record, call)`. It returns
`{value = table}` or `{record = record}` for the configured output, or
`{fault = {code = ..., reason = ..., language = ..., subcode = ..., detail = ...}}`.
Reject ambiguous result tables and nil. Application fault codes are Sender or
Receiver; protocol faults remain Core-owned. Detail requires a declared schema
and namespace binding. Do not accept raw XML fragments for output or details.

The binding assigns Lua values or supplied records into a private Lua-owned
output record through released public interop before staging a live result.
Retaining or mutating the original Lua value must not alter pending output.
Return registration failures as nil/error; protect handlers and conversion
against Lua exceptions/allocation failure. Lua header handlers use the same
owner-loop delivery and explicit fault semantics as their C equivalents.

## Streaming, execution and ownership

Request bytes flow through the existing bounded native-reader queue into the
namespace-aware parser and mapped records before transport EOF. Materializing
the declared record is expected. Collecting the entire HTTP body, envelope DOM,
payload XML, or a temporary input file behind `soap_route` is prohibited.

Operation selection happens after the parser sees the Body document name. If a
record provider needs owner-loop work, pause the native parser at that boundary,
request acquisition on the owner loop, and resume only after acknowledgement.
Apply the same handoff to registered header records. This is bounded internal
coordination, not a public job system. The owner loop must not join a parser or
wait synchronously for input. The native parser must never enter Lua or invoke
application request/header/operation callbacks. Allocators and native field
callbacks retain the existing XML native-thread requirements.

Validate the complete request before operation side effects. Keep callback and
hook execution on the owner loop. Disconnect or cancellation wakes paused
acquisition/parsing, prevents future callbacks and retains state until native
acknowledgement. Each acquire attempt receives its matching release, including
partial failure. No borrowed request remains usable after close.

Generate response envelope, payload and closing elements incrementally with
namespace-aware libxml2 writer operations and bounded chunks. Backpressure pauses
generation. Success and fault output must not serialize once into a full buffer
and then expose that buffer as a stream. Small copied names, reasons and protocol
metadata are bounded state; detail records follow the same live-output rules.

Registration copies route paths, operation/action/name strings, binding metadata
and callback declarations. Maps, runtimes and C userdata remain borrowed through
route destruction; Lua retains their owners. Selected result/detail records and
declarations remain pinned through transport/source cleanup, after-hook
replacement or cancellation. Producers do not mutate their borrowed records.

After hooks run after result/fault selection and before transport submission,
using the existing scoped staged reply interface. They may inspect/edit ordinary
headers or replace the result with another valid SOAP result/fault. They must not
leave an envelope/content-type/status mismatch. Explicit HTTP short circuits keep
their existing reply contract. A producer error after HTTP commitment terminates
the response and reports the original failure; it cannot append a second envelope
or replace the stream with a fault. Before commitment, select a Receiver fault
when the fault generator remains usable; otherwise use a finite ordinary 500.

If released public dependency APIs cannot provide any required record operation
without full-value XML serialization/reparse, stop and report that concrete gap.
Do not copy dependency-private layouts or hide materialization to complete a path.

## Limits and defaults

Use zero to select the following finite defaults or existing inherited limits.
Validate combinations before registration commits. Counts/bytes must use checked
arithmetic and actionable error diagnostics; no new unlimited sentinel is needed.

| Limit | Default and meaning |
| --- | --- |
| HTTP request bytes | Inherit the route/server body ceiling; enforce it independently of XML limits. |
| Native input queue | 256 KiB, matching XML upload-reader behavior. |
| XML nesting | 64 active element levels including the SOAP envelope. |
| Mapped text | 64 MiB aggregate decoded text, further constrained by request and runtime allocation limits. |
| Header blocks | 64 per envelope, including ignored blocks. |
| SOAP Header data | 64 KiB cumulative decoded names, attributes, namespace declarations and text across all header blocks, including ignored blocks. |
| Parser/namespace metadata | 64 KiB of live structural metadata, including in-scope bindings and active element/attribute state. |
| XML name/namespace/action bytes | 4096 per value; a length violation is not silent truncation. |
| Public fault reason | 1024 UTF-8 bytes; do not copy private diagnostic details into this field. |
| Response bytes | 64 MiB wire-byte ceiling for the whole generated envelope, including faults, independent of mapped text/runtime bounds. |

Decoded text and metadata limits count UTF-8 byte lengths supplied by libxml2;
HTTP admission counts incoming bytes and response limits count generated bytes.
Charge every header occurrence to its cumulative budget, without retaining skipped
values. Charge structural metadata while live and release that charge on scope
exit. Counts must be independent of input encoding and chunk boundaries.
Queue/parser/name state must be independent of total input length. Mapped
arrays/strings may grow only within declared record/runtime limits.
An explicit spool-backed mapped field is permitted under its documented provider
policy; it does not permit spooling the entire envelope as an input substitute.
Use heap workspaces and verify default musl worker stacks and guard regions.

## Fault and HTTP status contract

Build faults structurally through the XML writer. Code uses a bound QName;
Reason contains a language-tagged Text (`en` by default); optional application
subcode and typed Detail have explicit namespace bindings. Generic Receiver
reasons must not expose native paths, credentials or internal exception text.

| Condition | HTTP response |
| --- | --- |
| Successful operation | 200 with one SOAP 1.2 result envelope. |
| Malformed HTTP or invalid media-type parameters | 400 ordinary HTTP error, before SOAP processing. |
| Unsupported method | 405 ordinary HTTP error with `Allow: POST`. |
| Unsupported media type, charset or content encoding | 415 ordinary HTTP error. |
| HTTP body admission limit | Existing 413 behavior; operation never executes. |
| Accepted representation with wrong envelope version | 500 VersionMismatch fault with Upgrade metadata advertising SOAP 1.2 only. |
| Unhandled targeted mandatory header | 500 MustUnderstand fault. |
| Unsupported SOAP encoding style on processed data | 500 DataEncodingUnknown fault. |
| Malformed XML/envelope, XML limit, unknown operation, action mismatch or invalid input record | 400 Sender fault. |
| Explicit application Sender/Receiver fault | 400/500 respectively. |
| Unhandled application exception, allocation or internal operation failure | 500 Receiver fault when possible. |
| Disconnect, cancellation or post-commit generation failure | Existing close/abort behavior; no second response or handler retry. |

Use the binding's fault/status mapping rather than returning 500 for every SOAP
fault or HTTP 200 containing a Fault. Protocol-generated fault headers are
namespace-correct and bounded. Emit at most one fault. SOAP faults bypass generic
HTML/JSON error rendering; transport/auth short circuits remain ordinary HTTP.

## Verification and completion criteria

Tests are the executable contract. A first delivery is complete only when the
following observable behavior is proven for C free functions, receiver calls and
Lua preload/dynamic loading as applicable:

1. A real HTTP endpoint with multiple operations selects by Body expanded name,
   returns correctly named/escaped output and observes group prefixes and hooks.
2. Renamed/default prefixes and legal rebinding work. Wrong URIs, undeclared
   prefixes, namespace collisions on nested fields/attributes and QName fault
   values cannot bypass validation.
3. Envelope ordering/cardinality, trailing input, empty/multiple operations,
   DTDs, processing instructions, malformed XML, duplicate header blocks, nil
   semantics and encodingStyle rules produce the specified single fault/error.
4. Header targeting covers omitted role, next, ultimateReceiver, none and other
   roles. Unknown mandatory headers beat payload errors and prevent every
   application callback; supported blocks execute once in document order.
5. Media type parsing covers casing, quoted action, missing/empty/relative/
   mismatching actions, duplicate parameters, charset/BOM/declaration conflicts,
   UTF-8/UTF-16 input, SOAPAction-only requests and rejected SOAP 1.1 media types.
6. Tests inspect fault QNames, namespace declarations, reasons/languages,
   subcodes/details, Upgrade/NotUnderstood metadata, Content-Type and HTTP status.
   They also cover admission/auth failures and after-hook replacement cleanup.
7. Streaming tests withhold transport EOF until an allocator/field event proves
   incremental payload mapping. Output begins before generator completion;
   slow readers bound queues. Cover split code points, tiny chunks, large inputs,
   cancellation while awaiting provider acquisition and cancellation while parsing.
8. Inject failures at acquisition, parse growth, binding/result copying, faults
   and output generation. Prove no duplicate callbacks/releases, stale request
   access, leaks, retry after side effects or retained-record mutation of output.
9. Native GNU/musl C/Lua suites, constrained 128 KiB Debug stacks and guards,
   Valgrind, ARM QEMU and all supported target builds pass. Verify relocated
   static/shared consumers, public headers/exports, clangd/LuaLS and formatting.
   Darwin execution evidence remains distinct from cross-build/package evidence.
10. An independent SOAP client exchanges successful calls and Sender/Receiver
    faults with both C and Lua services, using a small checked-in WSDL/XSD fixture
    if its tooling needs one. Pin the selected test tool and document its setup;
    success cannot rely solely on Knurl generating and parsing its own messages.

New receiver method pointers are public ABI. Before implementation changes their
layouts, determine the latest released ABI and apply the project's required
single-step ABI bump when broken. Do not derive it from the current branch.
No feature flag, parallel SOAP 1.1 implementation or compatibility shim is part
of this plan. Update public C comments, Lua metadata, examples and shipped guides
with the implementation; retain this document as the implementation contract.
