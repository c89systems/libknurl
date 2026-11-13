# HTTP and application-input completeness specification

Status: implemented and verified on 2026-11-13. The
[verification record](verification.md#http-completeness-2026-11-13) identifies the
completed gates and their limits. This document defines the delivered behavior
from the 2026-11-13 implementation request.
It extends the [product contract](server-api-spec.md) for the operation families
below. Existing [SOAP work](soap12-spec.md) remains deferred.

## Scope and boundaries

Deliver the C/Lua input-validation conveniences, static HTTP refinements,
managed chunked request bodies and application WebSocket completeness identified
in the feature inventory. Use the existing Kore backend, route registry,
request/reply/source lifetime machinery and released dependency SDKs.

Listeners remain HTTP/1.0 and HTTP/1.1. HTTP/2 and HTTP/3 listeners, QUIC, SOAP,
Vectis consumer-repository edits, release signing/tagging and publication are
excluded. Preserve and test HTTP/2 proxy upstream support. MIME inference already
has 125 extension mappings; preserve that catalogue and its override precedence.

Implement in strict C89, with public C receiver methods for handle operations
and a thin Lua binding over the same Core implementation. No feature flags or
legacy implementations for these corrections. New user-selectable compression
policy is ordinary product configuration, not a refactoring switch.

## Input validation and context-specific escaping

Add `<knurl/input.h>` and `knurl.input` for stateless span-based operations.
Use `knurl_bytes` and explicit lengths. Embedded NUL, truncation and trailing
garbage must never turn invalid input into a successful shorter value. Expected
failures return status and bounded diagnostics; validation leaves input intact.
No helper silently trims, repairs, case-folds or changes Unicode normalization.

The initial public operations are:

| Operation | Required behavior |
| --- | --- |
| `long` | Exact optional leading minus and decimal digits, checked native C long range and caller-supplied min/max. No plus, whitespace, fractions or exponents. |
| `u64` | Exact unsigned decimal using the existing portable `knurl_u64` representation and caller-supplied bounds. Lua uses its existing exact count representation when needed. |
| `boolean` | Only `true`, `false`, `1`, `0`; exact case, no coercion. |
| `text` | Validate UTF-8, explicit byte-length bounds and optional rejection of control characters. No grapheme-count promise. |
| `allowlist` / `choice` | Exact allowed-byte membership or exact match against declared span values, with explicit length bounds. |
| `email` | Documented ASCII mailbox subset: unquoted dot-atom local part, DNS-style domain labels, no comments/display names, whitespace, empty labels or leading/trailing/consecutive local dots. Local <=64 bytes, domain <=253, total <=254. This checks syntax, not deliverability. |
| `http_url` | Absolute HTTP/HTTPS URL with a nonempty host and valid port; reject controls, whitespace, embedded NUL and credentials. Use libcurl's public URL parser; do not connect or resolve DNS. |
| `filename` | One ASCII component of at most 255 bytes using letters, digits, dot, underscore and hyphen; reject empty, dot/dot-dot, separators and control/NUL bytes. This does not establish filesystem confinement. |
| `html_escape` | Escape ampersand, angle brackets, double and single quotes for HTML text or quoted attributes; validate UTF-8, return an owned byte value and enforce an explicit output bound. This is not a JavaScript/CSS/URL encoder. |

Provide matching `knurl_input_*` C functions; use `parse_long`/`parse_u64` as
Lua spellings where primitive names would be unclear. Owned outputs use the
existing owned-byte cleanup contract. Zero output bounds select a documented
finite default, 1 MiB for HTML escaping. Size checks use checked arithmetic.

Add `request->query_long`, `query_u64` and `query_boolean` receivers and Lua
`request.query_long(name, ...)`, `query_u64`, `query_boolean` conveniences.
Absent input returns EOF; duplicate names and invalid input return INVALID;
representational overflow returns OVERFLOW. Do not silently use the first of
multiple values. Lua returns nil/error on absent or invalid input, with the
status available in the error. Bound/default conversion must not lose precision.
Callers apply defaults explicitly after checking absence. Typed path-parameter
variants use the same validators and absence/error conventions.

Application validation remains explicit. These helpers do not promise a universal
sanitized string, SQL escaping, command safety, HTML markup filtering or Unicode
email/IDNA normalization. JSON/XML codecs continue to own their output escaping.

## Static ranges, validators and compression

### Multiple byte ranges

Extend static files, directory mounts and custom asset providers identically.
Parse up to 16 requested byte ranges with checked wide counters. Preserve suffix
and open-ended ranges. Drop unsatisfiable members when at least one member is
satisfiable. Malformed lists, excess members and wholly unsatisfiable byte ranges
produce 416 with `Content-Range: bytes */size`. Unknown units remain ignored.

One satisfiable member produces the existing single 206 representation. Multiple
members produce `multipart/byteranges` with a generated valid boundary, per-part
Content-Type and Content-Range, CRLF framing and the closing delimiter. Do not
emit a top-level Content-Range for multipart output. Preserve request order;
duplicate/overlapping members need not be coalesced. The 16-member limit bounds
amplification. The source retains the one opened asset and reads each selected
offset directly; it must not collect, spool or scan preceding file bytes.

Compute multipart representation length with checked wide arithmetic before
commitment. Header/framing state is bounded; reset restarts the complete selected
representation; cancellation/replacement closes the asset exactly once. HEAD
continues to ignore Range and report the full selected representation's length.
Exercise offsets and multipart totals above 4 GiB on armhf.

### Date conditions

Add date-based If-Range using the opened asset's Last-Modified metadata and
libcurl's public HTTP-date parser. Require exact timestamp equality and a date
usable as a strong validator under RFC 9110; filesystem/provider metadata must
explicitly establish that strength rather than inferring it from one-second
timestamp resolution. An invalid, stale or insufficiently strong date falls
back to the complete 200 response. Strong ETag If-Range remains supported; weak
ETags never qualify. Apply If-Modified-Since's not-modified comparison to dates
at or after Last-Modified, preserving If-None-Match precedence. Use metadata from
the same opened asset used for delivery.

Evaluate If-Unmodified-Since when If-Match is absent, before If-None-Match,
If-Modified-Since and ranges. A known modification later than the supplied date
returns 412 for GET and HEAD. Ignore invalid dates or unavailable modification
metadata; If-Match takes precedence even when its condition succeeds.
Accept the three HTTP-date forms and ignore repeated modification-date fields
or date lists. Repeated or invalid If-Range selects the complete response.
Apply the [HTTP recipient list rules](https://www.rfc-editor.org/rfc/rfc9110.html#section-5.6.1.2)
to range and ETag lists: ignore up to 32 empty members per list, combine repeated
If-Match/If-None-Match fields, and still validate every nonempty member. Empty
ETag lists do not match; mixed wildcards remain invalid. Empty range members do
not consume the 16-range budget.
Admit syntactically valid empty header values through the backend, including
empty Accept-Encoding and conditional lists. Keep empty names, missing colons,
invalid controls and empty Host values rejected.

### Negotiated compression

Static configuration gains `compression_disabled`, default false, and
`precompressed_disabled`, default false. Inherited configuration must have the
same meaning in C and Lua. Apply only to eligible successful representations;
never compress ranges or multipart responses, and honor Cache-Control no-transform.
An actual Range request selects identity before conditional/range evaluation.
If identity is explicitly unacceptable for that request, return 406.

Parse Accept-Encoding token/qvalue lists, wildcard and identity exclusions.
Reject malformed qvalues rather than guessing. Missing Accept-Encoding selects
identity. Ignore up to 32 empty list members across all Accept-Encoding fields;
reject a larger count. Select the best acceptable available representation;
equal-quality preference is Brotli sidecar, gzip sidecar, dynamic gzip, identity.
Return 406 when no available representation is acceptable. Merge Vary: Accept-Encoding
without losing existing Vary fields. HEAD selects the same representation as GET
without reading file data or running a compressor to determine its length.
Preserve repeated Vary fields through response submission. Include negotiated
Content-Encoding on successful asset replies and 304, and omit it from plain
error responses, including pretty error pages.

For filesystem and asset-provider mounts, look for `original.br` / `original.gz`
only through the same confined provider/path policy as the original resource.
Require a regular sidecar whose modification metadata is not older than the
original; providers lacking usable freshness metadata fall back to identity or
dynamic gzip. Keep original-resource MIME inference, set Content-Encoding, use
the selected representation's byte length and representation-specific validators.
Pin the selected asset and close all unused opened assets. Index selection,
authorization, explicit types and 404 policies remain attached to the original
resource. A compressed extension alone still does not imply Content-Encoding.

Dynamic gzip uses the already pinned cpkt zlib provider. Add its public CMake
dependency/link closure explicitly. Use incremental deflate with bounded input
and output chunks and deterministic gzip headers, preserving pause/resume,
reset and exactly-once cleanup. Never compress the whole file to calculate
Content-Length or stage output in a temporary file. Omit unknown compressed
Content-Length and use existing HTTP framing. HEAD does the same. Dynamic gzip
uses a distinct weak validator derived from the original when available; never
reuse a strong identity ETag as a strong compressed ETag.

Eligible dynamic types are text/*, JSON and +json, XML and +xml, JavaScript, SVG
and WASM. Skip already compressed audio/video/images/archives and empty files.
Brotli generation is excluded: negotiate existing .br files without adding a
Brotli SDK. All response-byte streaming remains real producer-to-consumer flow.

## Managed chunked request bodies

Accept HTTP/1.1 `Transfer-Encoding: chunked` through ordinary managed body plans,
including STREAM, BUFFERED, MATERIALIZED, native uploads and typed JSON/XML routes.
Do not require a synthetic Content-Length and do not rewrite headers to invent
one. Content-Length plus Transfer-Encoding and duplicate/ambiguous framing remain
400/close errors; unsupported transfer coding remains 501. Keep method/body-plan
admission rules, NONE-body rejection and auth/Expect handling before body delivery.

Factor the existing private proxy chunk framer into a shared private HTTP framer.
Use one parser for fixed/chunked framing; retain proxy behavior and tests. Decode
sizes/extensions, chunk terminators and trailers incrementally. Bound metadata
using the existing framer limits. Reject overflow, malformed hex/extensions,
invalid CRLF, forbidden trailer fields and premature EOF without completing the
body or invoking an ordinary completed-body handler. Do not decompress content
codings as a side effect of transfer decoding.

Callbacks and stored bodies receive decoded payload bytes only. Enforce route
and server byte limits on decoded bytes as they arrive; body-rate accounting is
based on payload progress, so metadata-only traffic cannot defeat the deadline.
Count received decoded bytes independently of consumer acceptance, including
consumer failure and pending retries. Exclude framing, trailers and pipeline
tails, and never count a retained suffix twice.
Transport loss with incomplete managed input reports IO consistently across
request-release and response-queue notification orderings.
Honor existing pause/partial consumption/resume semantics at every chunk split.
Zero chunk plus the complete trailer block establishes EOF. Bytes beyond that
boundary belong to the next pipelined request and must be preserved exactly once.

Retain legal trailers separately from initial headers, with bounded count/bytes.
Expose request trailer count/at/first receivers and Lua `trailer`/`trailers`
lookups after body completion; early lookup returns STATE (count is zero).
Trailers cannot alter framing, routing, authentication or initial headers.
Disconnection, limit errors and cancellation retain existing terminal ownership
and unread-input close behavior. Parse metadata on heap-owned bounded state.

## Application WebSocket protocol completeness

Implement fragmentation, incremental UTF-8 validation and valid close handling
in the existing application WebSocket engine. Keep proxy tunnels opaque.
Do not add WebSocket compression/extensions or collect entire messages behind
an incremental callback.

Extend the C on_frame callback to include a boolean final flag; Lua on_frame
receives the equivalent final argument. Deliver TEXT/BINARY as the initial frame
and CONTINUATION for subsequent fragments. Preserve complete bounded frame
delivery and AGAIN/resume retention. Message state spans fragments; control
frames may interleave. Reject a continuation without an active message or a new
data opcode before a fragmented message ends. Add max_message_bytes, with zero
inheriting max_frame_bytes; count all fragments with checked arithmetic and
close with 1009 on excess. No message-assembly buffer is introduced.
Both directions enforce the configured data-frame and message ceilings.

Validate text UTF-8 across fragment boundaries, rejecting overlong sequences,
surrogates, invalid scalar values and incomplete final sequences with close
1007. Binary messages are unaffected. Reject unmasked input, reserved bits/
opcodes, nonminimal length encodings, the high bit of a 64-bit length and invalid
control FIN/length with 1002. Frame and message limits remain separate.

Validate incoming CLOSE payload length, status and UTF-8 reason before exposing
them. A one-byte close payload or reserved/invalid code yields 1002; an invalid
reason yields 1007. Protocol errors queue the correct bounded close frame when
possible, then use the existing close deadline; they do not call application
data handlers for rejected input. Ping/pong handling and clean/abrupt outcomes
retain explicit accounting.

Add send_frame(opcode, final, bytes) as a receiver and Lua method. Validate
outgoing fragment sequencing, text and controls, updating message state only
after atomic queue acceptance; AGAIN changes no state. Existing send/text/binary
are complete-frame conveniences. Sending CLOSE initiates the same validated
closing handshake as close(); sending a data frame after closing returns STATE.
The zero output-queue budget resolves to a finite max_frame_bytes + 14 bytes
(checked), large enough for one permitted frame and control minimum. Explicit
budgets remain finite and authoritative.

This is a clean pre-1.0 callback/API correction. Migrate all C/Lua call sites,
fixtures, examples and editor declarations together; do not retain a second
legacy frame engine or alternate callback shape.

## ABI, implementation sequence and acceptance

Receiver fields and callback/config layouts change during unreleased development.
Knurl has not been released, so keep shared-library ABI 0 and verify installed
Linux/Darwin metadata against it. Local tags do not establish a published ABI
commitment. Keep repository release/version markers independent; do not create
a release tag for this work.

Implement and verify in this order: input helpers/ABI boundary, static ranges and
dates, negotiated static compression, shared framing/managed chunked bodies,
WebSocket corrections, then integrated C/Lua/docs/package checks. The implementation
must update the product contract and public guides as behavior is delivered.

Unit and integration tests must prove valid, invalid, boundary and allocation-
failure behavior, not just the presence of functions. In particular:

- C/Lua validation parity, exact counts, duplicate/absent parameter handling,
  malicious/control/NUL bytes, context-specific escaping and output limits.
- Filesystem and provider multipart wire bytes, length, wide offsets, resets,
  conditional precedence and date strength. Compression tests decode delivered
  bytes independently, test negotiation/freshness/validators/HEAD/ranges,
  cancellation and bounded first output before full source consumption.
- Chunked bodies split at every framing boundary, with extensions/trailers,
  pause/resume, pipelined successor, decoded limits, malformed input, disconnect,
  native/Lua delivery and no full-body buffering for STREAM/XML uploads.
- WebSocket fragmented text/binary with interleaved controls, UTF-8 code points
  split across frames, malformed sequencing/length/close cases, bounded budgets,
  output retry invariance and exact terminal close codes in C and Lua.
- Existing proxy HTTP/1, live TLS/ALPN HTTP/2, SSE and opaque WebSocket relay
  regressions continue to pass.

Run GNU/musl C/Lua suites, constrained Debug worker-stack/guard checks, Valgrind,
all seven target builds/packages and four ARM QEMU gates. Verify installed
static/shared CMake/pkg-config consumers, public symbols/receivers, C89/C++
headers, clangd/LuaLS and formatting. Keep Darwin cross-build/package evidence
distinct from native execution. Checks/builds run serially with the configured
job limit. Commit verified coherent changes and leave the worktree classified;
do not push or perform the separately governed consumer cutover/release.

Protocol references: [HTTP semantics](https://www.rfc-editor.org/rfc/rfc9110.html),
[HTTP/1.1 framing](https://www.rfc-editor.org/rfc/rfc9112.html) and
[WebSocket](https://www.rfc-editor.org/rfc/rfc6455.html). These define wire rules;
the scope, public interfaces, finite limits and ownership above are Knurl's
product decisions.
