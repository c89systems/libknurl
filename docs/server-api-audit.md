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

The standalone public framer had test callers but no production caller here;
its wrapper, public header and wrapper-only tests have been removed. The lifted
private proxy framer and its invariant checks remain. The executor/job interface
now lives in a private header and has no shared-library exports. Its native I/O
and filesystem consumers stay internal; job/storage ownership tests link the
internal static library instead of posing as installed SDK consumers. Installed
storage/filesystem/WebDAV tests continue using public APIs.

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
before EOF without transport spooling. Selected-array operations now use the
baseline pull loop, caller-owned record storage, zero-based indices and borrowed
sources. Request iteration rewinds on success/failure; its reference adapter only
forwards record/index/userdata. Lua binds these public operations with its own
record and the existing public record-to-table conversion. The unused push-parser
surface is removed. Lua typed routes now bind the C typed registrations using a
public record-owner adapter. C performs input parsing, dispatch, ordinary output
serialization and hook ordering; the facade only adapts request arguments. The
record adapter uses released LoneJSON constructors/views and retains owners
through response cleanup or native acknowledgement. Tests cover partial acquire
failure, invalid views, parse failure, owner-loop release and Lua record retention.
Lua logging now uses released pslog check/ref/unref operations and the public C
logging registration, preserving native-sink validation, severity policy,
derived sub=http and independent file access logs. Preload/dynamic checks cover
owner retention/release and coroutine collection. All seven target bindings use
the pinned shared Lua providers and C SDKs; no core-private headers are imported.
The remaining surface review and full reference adapter are still open.

The server-config reference now translates the 34 baseline fields and exercises
foreground/host-controlled readiness and worker shutdown through installed public
interfaces. It exposed two corrected restrictions: second-based deadlines now
saturate at 65,535 seconds as in Vectis, and mapped JSON records no longer acquire
a 10 MiB size ceiling absent from the baseline. Cross-target executable tests are
disabled consistently when that target cannot run locally; host-tool SDK/export
checks remain enabled.
Host asset/storage providers now have public Lua interop values for static-assets
and WebDAV mounts. They retain an optional owner without taking or closing the
provider. The isolated C host module compiles against installed public C/Lua
headers on all seven targets. GNU/musl preload/dynamic runs cover ownership,
range/conditional handling, binary data, bounded reads, listing and read-only
mutation rejection. Packed lookup stays in the host; no second HTTP/DAV engine
or application filesystem type enters the binding.

## Evidence limits and work state

No runtime edits or test runs were made during the stop-and-audit. The repository
has selected toolchains and build state for all seven targets, public-header
binding imports and installed consumers. Prior per-slice checks do not prove
Vectis parity. The complete installed Vectis reference adapter is still partial;
source/rock packaging and final compatibility/artifact
checks remain incomplete. The [implementation plan](implementation-plan.md)
orders correction work under the revised active implementation goal.

Feature documents are implementation notes under correction, subordinate to the
small contract. They must not be used to justify existing behavior that conflicts
with the baseline. This documentation rewrite does not certify or repair code.
