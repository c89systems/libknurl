# Extraction audit and remaining work

The [145-line contract](server-api-spec.md) governs scope. The behavioral baseline
is Vectis `6b1e263e0436687f68a7768ca55f4361a6e59e19`. This is an implementation
inventory, not a compatibility certificate. Sibling repositories remain read-only.

| Contract family | Implementation and forwarding evidence |
| --- | --- |
| Server | `tests/vectis-server.c` translates all 34 baseline fields; `vectis-config.c` covers effective values, host readiness, stop and shutdown. |
| Routes | `tests/vectis-route.c` translates baseline declarations/callbacks/errors; `vectis-route-runtime.c` covers order, masks, conflicts and hooks. Lua verb/group and fixed text/JSON/redirect/authenticated-JSON helpers use that same ordinary route engine. |
| Requests/bodies | `vectis-route.c`, `vectis-body-file.c` and `vectis-request-reply.c` cover strings/lookups, borrowed readers, materialization, independent copies, spill and cleanup. |
| Replies/sources | The same adapter covers staged replies and borrowed/taken sources. `vectis-response-head.c` covers shared Kore header generation, manual commitment/backpressure and bodyless replies; file opening occurs after hooks. |
| Uploads | `vectis-services.c` forwards chunk/file/native-reader callbacks. `vectis-services-runtime.c` checks progress before EOF, default 204 and cancellation ownership through native acknowledgement. |
| JSON | `vectis-forward.c` translates typed declarations/raw records, materialized/generated replies and array rewriting; `vectis-route.c` forwards request parsing/selected-array iteration. `vectis-typed.c` and `json-runtime.c` exercise ownership, limits, callback errors and completed ordinary output before after hooks. |
| XML | `vectis-forward.c` translates typed routes and standalone source/reader parse, sink write and owned-byte output. `vectis-typed.c` covers options, source precedence/current position, borrowed ownership, partial failure, sink errors and the baseline trailing NUL. `xml-runtime.c` observes live parsing before EOF. DOM/XPath/SAX implementations and Lua bindings exist; their full baseline C wrapper translation remains to be demonstrated. |
| REST/OpenAPI | Lua REST binds public C REST registrations for decode/validate/handler sequencing. Lua owns application error policy and the existing LoneJSON Lua value conversion. Replies encode once. `rest-runtime.c`, `tests/lua/rest.lua` and `openapi.c` cover errors, validation, groups, annotations and generation. |
| Static/assets | `vectis-services.c` forwards file/directory/packed-provider mounts; `openapi.c` and the Lua asset fixture cover indexes, MIME, HEAD/ranges/conditions and borrowed provider ownership. |
| WebDAV/storage | The service adapter forwards mount/auth/storage declarations. `vectis-services-runtime.c`, `webdav-runtime.c` and filesystem fixtures exercise the existing engines; packed/durable clients stay in the host. |
| Reverse proxy | The service adapter forwards declarations, admission and edit/response/error callbacks. Proxy runtime/WebSocket/WSS and framing fixtures exercise native relay execution. No proxy state machine belongs to the adapter. |
| Application WebSocket/SSE | The service adapter forwards WebSocket callbacks/send/close and source-based SSE. Wire cases preserve Kore frame/close behavior and incremental source ownership. Lua delegates transport to Core. |
| TLS/ACME | The service adapter translates resolved file/memory/borrowed-source material and ACME host callbacks. TLS and local-CA issuance/renewal fixtures cover serving, persistence failure and error preservation, including the host Lua VM. Kore retains its runtime files; the invented generation store is removed. |
| Policy/logging | Baseline server translation drives policy/observation/log fixtures. Lua borrows released public pslog references; derived HTTP logging and independent access files use Core. |
| Host/backend | Public process/control/readiness/loop callbacks and the opaque Kore request accessor are implemented and tested. The config fixture forwards a subset of host lifecycle use; complete baseline host callback/control translation is still open. |

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

Remaining work is concrete:

- Map the standalone `vectis_json_validate_cstr` utility and complete the C
  DOM/XPath/SAX wrapper and host lifecycle/control forwarding evidence. Existing
  direct Core tests alone do not establish that those baseline calls are mechanical.
- Finish the accumulated compatibility/failure gate against the baseline inputs,
  particularly combined lifecycle/restart, proxy pressure and DAV failure cases.
  Reuse matching passed evidence; do not rerun every suite per edit.
- Complete the final clean release-configuration/artifact gate when preparing a
  release. Seven-target debug SDK packaging/relocation and Lua source/rock
  rehearsals exist; development builds do not establish release readiness.
- Actual Vectis normal/packed cutover regressions require separately authorized
  Vectis edits. No consumer cutover, publication or push is included here.

Target evidence distinguishes executable GNU/musl checks from compile/link and
artifact inspection on the other five toolchains. Installed static/shared
consumers, the installed shared Lua binding and source-rock reconstruction use
pinned independent SDKs, never sibling build state. The active goal remains open
until its implementation and required verification obligations are satisfied.
