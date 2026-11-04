# Extraction audit and correction status

The [extraction contract](server-api-spec.md) replaces the previous specification
and its audits. This report records findings, not additional product scope.
Reviewed baseline: Vectis 6b1e263e0436687f68a7768ca55f4361a6e59e19;
librotmos committed implementation 3f0e75c plus three unverified array edits.

The initial error was specifying a general server runtime instead of preserving
Vectis's existing mechanisms through a new library boundary. Tests then checked
parts of that new design rather than baseline compatibility. The owned public
API and Kore backend architecture are correct; they do not require this redesign.

## Findings at the audit boundary

| Finding | Evidence | Required outcome |
| --- | --- | --- |
| Lua REST encodes to a complete buffer, wraps it as a JSON reader and reparses/emits through another writer/buffer. | [binding](../src/lua/rest.c), [second output pass](../src/rest_reply.c), [baseline](../../vectis/lua/vectis/rest.lua) | Preserve one-pass reply encoding. No DOM is involved; the extra parse/emission and buffers are still unnecessary. |
| Lua typed routes prepare records and register ordinary routes instead of binding the C typed registrations. | [facade](../lua/rotmos.lua), [binding](../src/lua/server.c), [C routes](../src/json_route.c) | Share feature execution through public C, retaining public dependency interop and Lua owner protection. Direct LoneJSON input parsing already exists and must be retained. |
| XML routes require a completed buffered/spooled body. | [rotmos](../src/xml_route.c), vectis_xml_route_dispatch/register_xml_route in [baseline](../../vectis/src/vectis.c) | Preserve live upload-reader parsing; materializing the typed record does not justify collecting the transport body. |
| Ordinary C typed JSON routes automatically select a live generator. | [rotmos](../src/json_route.c), vectis_json_route_dispatch/response_json in [baseline](../../vectis/src/vectis.c) | Preserve synchronous reply serialization before after-hooks and its failure timing; retain explicit live output separately. |
| Selected-array storage ownership created an apparent need for new LoneJSON interop, while request/source iteration is missing. | [array wrapper](../src/json_array.c), [public header](../include/rotmos/lonejson.h), baseline request_json_array_each/json_array_each_source and public Lua record interop | Use existing caller-owned record/storage behavior. The earlier upstream conversion request is withdrawn. The latest local ownership rewrite is not verified or accepted. |

## Surface and architecture review

The standalone public framer has test callers but no production caller here.
The public executor/job API has internal production consumers, without a shown
need for a separate general-purpose SDK. Review/narrow these public surfaces;
keep genuinely required native execution and proxy framing internals.

Review response/channel/loop machinery, asynchronous storage operations and ACME
generation publication against baseline adapter needs. Existing functionality
and necessary streaming/thread-affinity fixes stay; incidental new products,
formats, limits and behavior do not become requirements by being implemented.
Proxy, WebDAV/filesystem, DOM/SAX and server helpers have concrete Vectis origins;
line count alone does not establish that they should be removed.

At audit time, owned C/header/Lua source counted 77,350 physical lines: 51,802
library/header/Lua lines and 25,548 test lines. Vendored C/headers were another
40,284 lines. The proxy accounted for 7,769 owned lines versus 9,119 in Vectis's
proxy-named files, including application adapters/telemetry there.

## Correction progress

The reference JSON forwarding case in [vectis-forward.c](../tests/vectis-forward.c)
maps the baseline app/request/raw-record/userdata callback shape without codec or
transport work. The runtime fixture verifies completed JSON bytes before an
after-hook replaces the result. Ordinary C JSON routes now materialize before
after-hooks; explicit live typed replies retain their own lifetime contract.
Lua REST now forwards one encoder output directly, without the extra reader,
JSON runtime, writer or output buffer. C XML routes now parse live upload input
through the existing reader engine and return the record/handler to the owner
loop after native ACK. Their reference forwarding case observes mapped parsing
before EOF without transport spooling. Lua typed-route sharing, selected-array
iteration and the remaining surface review are still open.

## Evidence limits and work state

No runtime edits or test runs were made during the stop-and-audit. The repository
has selected toolchains and build state for all seven targets, public-header
binding imports and installed consumers. Prior per-slice checks do not prove
Vectis parity. The concrete installed Vectis reference adapter is missing; Lua
provider/logging operations, source/rock packaging and final compatibility/artifact
checks remain incomplete. The [implementation plan](implementation-plan.md)
orders correction work under the revised active implementation goal.

Feature documents are implementation notes under correction, subordinate to the
small contract. They must not be used to justify existing behavior that conflicts
with the baseline. This documentation rewrite does not certify or repair code.
