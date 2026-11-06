# Vectis integration boundary

The [extraction contract](server-api-spec.md) is the sole normative product spec.
Baseline: Vectis `6b1e263e0436687f68a7768ca55f4361a6e59e19`.

Librotmos receives the existing enhanced Kore server, router/replies/body/source
execution, proxy, WebDAV/filesystem, static serving, codecs, REST/OpenAPI,
WebSocket/SSE, policy/logging and server lifecycle mechanisms. Vectis removes
those implementations when it switches. It keeps app services/business policy,
outer supervision, credentials, metrics aggregation, packed lookup, durable-store
clients, DSV/MCP integrations and outbound clients.

The adapter translates config/types/errors, forwards callbacks/handles, registers
host providers and connects readiness/control. Application declarations and
handlers remain unchanged. HTTP/codec/queue logic in that adapter is a missing
library operation, not permission to rebuild the feature in Vectis.

The baseline sources below define detailed accepted inputs/defaults/lifetimes;
they are read-only evidence, never build inputs:

- [Public app/server/request/reply/config declarations](../../vectis/include/vectis/vectis.h)
- [Server helpers, body/source/JSON/XML route execution](../../vectis/src/vectis.c)
- [Kore integration and callback/response ordering](../../vectis/src/vectis_kore_bridge.c)
- [Proxy API](../../vectis/include/vectis/proxy.h), [HTTP adapter](../../vectis/src/vectis_kore_proxy.c), [WebSocket adapter](../../vectis/src/vectis_kore_proxy_ws.c)
- [WebDAV implementation](../../vectis/src/vectis_webdav.c)
- [XML implementation](../../vectis/src/vectis_xml.c), [SAX](../../vectis/src/vectis_xml_sax.c), [Lua DOM](../../vectis/src/vectis_lua_xml_objects.c)
- [Lua bindings](../../vectis/src/vectis_cli.c), [REST helpers](../../vectis/lua/vectis/rest.lua)
- [Lua server contract](../../vectis/manual/lua-app.md), [XML contract](../../vectis/manual/lua-xml.md)
- [ACME host persistence](../../vectis/src/vectis_acme_state.c)
- [Runtime regressions](../../vectis/tests/unit/test_vectis_runtime.c), [Lua lifetime regressions](../../vectis/tests/lua/app_source_lifetime.lua), [interop regressions](../../vectis/tests/lua/upgrade_seams.lua)

Readiness requires a concrete installed reference adapter here and the affected
Vectis regressions during the separately authorized consumer cutover. A broad
library API alone does not establish a simple replacement.

## Installed reference adapter

The adapter is test-only consumer code compiled against installed public C/Lua
SDKs. It translates declarations, errors and handles, forwards callbacks, and
registers host providers. It contains no replacement transport or codec.
The operation inventory below names its implementation and behavioral cases.
The [verification record](verification.md) distinguishes tested library behavior
from the separately authorized Vectis normal/packed cutover.

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
| TLS/ACME | The service adapter translates resolved file/memory/borrowed-source material and ACME host callbacks. TLS and local-CA issuance/renewal fixtures cover serving, persistence failure and error preservation, including the host Lua VM. Kore runtime files are working state; durable storage remains host-owned. |
| Policy/logging | Baseline server translation drives policy/observation/log fixtures. Lua borrows released public pslog references; derived HTTP logging and independent access files use Core. |
| Host/backend | `vectis-host.c` forwards shared readiness, READY/control providers, parent maintenance timers and the terminal worker callback. `vectis-host-runtime.c` checks outer host ownership, 503-to-200 readiness, STOP/malformed/EOF shutdown and fork-exit cleanup that destroys host/server state. Callback failure after reclamation reaches the parent. Existing process/auxiliary/TLS fixtures cover restart and child roles; the config fixture covers foreground use. The opaque Kore accessor is forwarded. |

## Compatibility corrections and remaining gate

Vectis's integration assessment identified standalone filesystem calls, mount
admission/default deadlines, fixed concurrency guards and terminal proxy
accounting (G1–G5). The existing engines now supply those operations; subsequent
sweeps corrected WebSocket EOF attribution, metadata-only/literal-name listing,
content-directory configuration, provider index defaults and Lua integer
conversion. Causes and regression references belong in [KORE.md](../KORE.md).

Actual cutover still requires replacing Vectis's implementations with forwarding,
then running its normal and packed regressions. This repository's reference
adapter and target matrix do not certify that consumer cutover. No sibling
repository is a writable or compiled dependency of this project.
