# Extraction audit

The [145-line contract](server-api-spec.md) governs scope. The behavioral baseline
is Vectis `6b1e263e0436687f68a7768ca55f4361a6e59e19`. This is an implementation
inventory. [Verification](verification.md) records the completed development
gates and their scope; it does not certify the actual Vectis cutover. Sibling
repositories remain read-only.

## Vectis integration assessment follow-up

Vectis's assessment of `f6bc8cb` identified concrete gaps not covered by the
initial family-level gate. The corrections below preserve demonstrated Vectis
operations and share the existing engines; they add no application requirement.

| Gap | Correction and regression |
| --- | --- |
| G1 | Standalone public filesystem lookup/read/put/delete/mkcol/copy/move/list and normalization share disk helpers, cursors and mutations without a server/loop. `vectis-filesystem.h` forwards the baseline calls; `vectis-filesystem.c` ports conditional, managed/direct, listing, recovery and default-stack-thread cases. `filesystem-failure.c` covers allocation, sync/close/write failure, old-value preservation and descriptor reuse. |
| G2 | C/Lua declarations forward file quotas into body admission, preserving zero inheritance/explicit overrides. WebDAV caps admission at the server hard ceiling. Separate small-limit C and Lua fixtures exercise acceptance between default and mount limits and rejection above mount/hard limits. |
| G3 | C server/mount translation and Lua defaults disable the extra deferred-response/storage deadlines. Explicit deadlines remain selectable. Effective-setting assertions and a deterministic storage expiry seam prove completion versus selected timeout, commit and retirement; cancellation and shutdown/abandonment remain covered. |
| G4 | The fixed 16-PUT and 256-operation/resource guards are removed. Configured server limits and bounded native execution/admission apply. The wire fixture holds 257 incremental uploads at staging barriers, then completes/cancels them and verifies content, cleanup and reuse. |
| G5 | Public terminal proxy snapshots provide the existing event's accepted-byte counters, selected target, kind, ID, duration, status and error/disconnect classification. C/Lua adapters forward them. HTTP/alternate/upload/disconnect/worker shutdown and WS/WSS fixtures check exactly-once reporting; public request observations retain upstream classification. |

Additional extraction defects found by the fault cases are recorded in
[KORE.md](../KORE.md). Installed-consumer and target checks distinguish runtime
execution on x86_64 GNU/musl from compilation/linking on the other five targets.

## Operation inventory

The subsequent sweep traced the operation inventory and G1–G5 against the same
baseline. It found four further compatibility cases: WebSocket EOF accounting,
metadata-only filesystem listing, literal names through asset/storage listing,
and content-directory/provider configuration. The corrections reuse existing
operations and reduce filesystem work; no public API or contract family was
added. I09–I12 in [KORE.md](../KORE.md) record the causes and failing regressions.

The next sweep checked configuration defaults, callback/source ownership,
error paths and C/Lua forwarding. It found the Lua provider-mount index default
missing from the C binding, although the C reference adapter already translates
it. I13 records that correction and its preload/dynamic wire regression. It adds
no Core API or server mechanism.

| Contract family | Implementation and forwarding evidence |
| --- | --- |
| Server | `tests/vectis-server.c` translates all 34 baseline fields; `vectis-config.c` covers effective values, host readiness, stop and shutdown. |
| Routes | `tests/vectis-route.c` translates baseline declarations/callbacks/errors; `vectis-route-runtime.c` covers order, masks, conflicts and hooks. Lua verb/group and fixed text/JSON/redirect/authenticated-JSON helpers use that same ordinary route engine. |
| Requests/bodies | `vectis-route.c`, `vectis-body-file.c` and `vectis-request-reply.c` cover strings/lookups, borrowed readers, materialization, independent copies, spill and cleanup. |
| Replies/sources | The same adapter covers staged replies and borrowed/taken sources. `vectis-response-head.c` covers shared Kore header generation, manual commitment/backpressure and bodyless replies; file opening occurs after hooks. |
| Uploads | `vectis-services.c` forwards chunk/file/native-reader callbacks. `vectis-services-runtime.c` checks progress before EOF, default 204 and cancellation ownership through native acknowledgement. |
| JSON | `vectis-forward.c` translates typed declarations/raw records, materialized/generated replies and array rewriting; `vectis-route.c` forwards request parsing/selected-array iteration. `vectis-typed.c` and `json-runtime.c` exercise ownership, limits, callback errors and completed ordinary output before after hooks. |
| XML | `vectis-forward.c` translates typed routes and standalone source/reader parse, sink write and owned-byte output. `vectis-typed.c` covers options, source precedence/current position, borrowed ownership, partial failure, sink errors and the baseline trailing NUL. `xml-runtime.c` observes live parsing before EOF. `vectis-xml.c` translates every DOM/XPath/SAX receiver operation; `vectis-xml-runtime.c` runs the baseline's original assertions through that adapter, including mutation, namespace handling, callback failure/reentry and foreign process/thread rejection. |
| REST/OpenAPI | Lua REST binds public C REST registrations for decode/validate/handler sequencing. Lua owns application error policy and the existing LoneJSON Lua value conversion. Replies encode once. `rest-runtime.c`, `tests/lua/rest.lua` and `openapi.c` cover errors, validation, groups, annotations and generation. |
| Static/assets | `vectis-services.c` forwards file/directory/packed-provider mounts; `openapi.c` and the Lua asset fixture cover indexes, MIME, HEAD/ranges/conditions and borrowed provider ownership. |
| WebDAV/storage | The service adapter forwards mount/auth/storage declarations. `vectis-services-runtime.c`, `webdav-runtime.c` and filesystem fixtures exercise the existing engines. Interrupted PUT creation leaves no file; interrupted replacement preserves the old file during and after cancellation, clears staging and permits a later replacement. Packed/durable clients stay in the host. |
| Reverse proxy | The service adapter forwards declarations, admission and edit/response/error callbacks. Proxy runtime/WebSocket/WSS and framing fixtures exercise native relay execution. The live HTTP pressure case stops client reads, observes upstream backpressure before the full body, checks unrelated request progress, resumes exact byte delivery, then cancels under pressure and verifies exchange reuse. No proxy state machine belongs to the adapter. |
| Application WebSocket/SSE | The service adapter forwards WebSocket callbacks/send/close and source-based SSE. Wire cases preserve Kore frame/close behavior and incremental source ownership. Lua delegates transport to Core. |
| TLS/ACME | The service adapter translates resolved file/memory/borrowed-source material and ACME host callbacks. TLS and local-CA issuance/renewal fixtures cover serving, persistence failure and error preservation, including the host Lua VM. Kore retains its runtime files; the invented generation store is removed. |
| Policy/logging | Baseline server translation drives policy/observation/log fixtures. Lua borrows released public pslog references; derived HTTP logging and independent access files use Core. |
| Host/backend | `vectis-host.c` forwards shared readiness, READY/control providers, parent maintenance timers and the terminal worker callback. `vectis-host-runtime.c` checks outer host ownership, 503-to-200 readiness, STOP/malformed/EOF shutdown and fork-exit cleanup that destroys host/server state. Callback failure after reclamation reaches the parent. Existing process/auxiliary/TLS fixtures cover restart and child roles; the config fixture covers foreground use. The opaque Kore accessor is forwarded. |

The original audit found duplicate Lua typed execution, a second REST JSON
parse/emission, delayed ordinary JSON serialization, collected XML transport
input and incorrect selected-array ownership. Those paths are corrected. Lua
request/source JSON parsing and synchronous JSON/XML sink output now use public
Core operations. Array callbacks reuse caller-owned storage and released
LoneJSON interop; no upstream extension is required. The standalone public
framer is removed and the native executor is private.

The latest route batch also preserves before-hook body edits (including injected
empty-body JSON), deferred validation replies, error-policy failure diagnostics
and skipped after hooks. The fixed helpers copy their declarations, forward
preencoded JSON unchanged, preserve C-string boundaries and attach OpenAPI data.
Standalone XML forwarding only adapts public values, callbacks and ownership;
its NUL terminator adapts Vectis's owned-byte result and adds no XML codec.

Standalone `vectis_json_validate_cstr` now forwards to its direct Core lift of
the existing LoneJSON validation utility. Baseline success, malformed/empty/NULL
input, C-string termination and dependency diagnostics are exercised by the
typed consumer. No XML operation needed a new Core API. The worker terminal
callback now runs with its fork's server detached after native/logger teardown;
it can reclaim host/server state without a later Core access.
Completion is acknowledged only after the callback returns; the host fixture
also verifies deadline escalation and abandonment if cleanup hangs after reclaiming
the app. The protocol and DOM/SAX cases use installed static/shared consumers.
An intermittent startup failure exposed Kore's shared-PID fork race; external
patch 0062 and the coordinated auxiliary-child regression address it. This and
all other recorded backend defects are tracked in [KORE.md](../KORE.md).

The TLS fixture now ports the baseline prebody header guardrails: exact Host
matching despite a preceding Hostile field, embedded NUL, bare CR and the last
accepted/first rejected field count. Both cleartext and TLS run those inputs.
These guardrail, live proxy pressure/recovery and DAV overwrite/recovery checks
pass on GNU and musl; the same fixtures compile/link on the other five targets.
This closes those isolated coverage gaps without changing the server contract.
`proxy-h2.c` also ports Vectis's live HTTP/2 duplex input/result through the
public proxy registration. The TLS origin requires ALPN `h2`, validates the
rewritten raw path/query and authority, receives `pingrest` with its exact
content length and no Transfer-Encoding field, and returns `pong` before the
client sends `rest`, followed by `done`. This is live transport evidence,
separate from H2 header-codec unit checks. It passes on GNU/musl and builds on
the other five targets.

The initial development gate at `f6bc8cb` passed all 95 GNU Core checks
and 32 Lua checks; musl passed 90 Core checks and 31 Lua checks, reusing the
same-host backend-tool and target-independent source-package checks. Full
installed static/shared reference consumers pass on both runtimes. All seven
SDK archives pass final-byte validation and four relocated example consumers
each; the Lua source rock reconstructs against installed public SDKs.

Historical defects without isolated regressions remain explicit limitations in
KORE.md, rather than promises of exhaustive fault injection. Actual Vectis
normal/packed cutover regressions require separately authorized Vectis edits.
A release still requires the lifecycle's default-branch/signing/tagging and clean
release-configuration gates. Neither that initial gate nor these focused
corrections certify the actual Vectis normal/packed cutover.

Target evidence distinguishes executable GNU/musl checks from compile/link and
artifact inspection on the other five toolchains. Installed static/shared
consumers, the installed shared Lua binding and source-rock reconstruction use
pinned independent SDKs, never sibling build state. No consumer cutover,
publication or push is included in this extraction work.
