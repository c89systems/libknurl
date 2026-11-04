# Implementation constraints

The active implementation goal covers the complete
[server API](server-api-spec.md) and [Vectis integration contract](vectis-integration-contract.md).
The end state is a mechanical Vectis cutover through public C and Lua APIs.

- Keep Kore pristine; apply the external patch series only to owned generated copies.
- Compile owned C and the selected backend with strict `-std=c89`.
- Follow the lifecycle skill strictly, including exact released dependency SDKs,
  independent prefixes, toolchain selection, packaging and final-artifact evidence.
- Build the full cpkt matrix: GNU/musl Linux for x86_64, aarch64 and armhf,
  plus arm64 Apple Darwin. Each build selects its concrete architecture/OS and
  lifecycle toolchain. There is no host-selected or native product.
- Use `build/debug` and `build/release` for the default selected
  `x86_64-linux-gnu` target. Other targets use `build/<target>/debug` or
  `build/<target>/release`. Keep the editor database at `build/debug`.
- Prioritize large coherent implementation surfaces. Add short invariant and
  failure-mode tests; avoid lengthy checks, repeated broad suites and detailed
  per-slice audits. Complete comprehensive behavioral, cross-target, installed
  consumer, lifecycle and packaging verification after the larger surfaces land.
- Keep sibling repositories read-only. Commit coherent changes on the feature
  branch without pushing. No replacement-readiness claim until the full contract
  is implemented and verified.

The [ACME adapter](tls.md) connects the publication core to public startup,
key-manager recovery, certificate installation, prepared TLS policy and identity
observations. The runtime fixture drives the actual certificate/challenge IPC
boundary and verifies TLS activation, persistence failure/restart and hard
deadline recovery. The [protocol fixture](acme-protocol.md) drives real HTTPS
directory/account/order exchanges, validates JWS/CSR and live TLS-ALPN-01,
and verifies issuance plus renewal selection after ACME-process restart.
It also covers explicit/environment trust capture and transport bounds.

The [application WebSocket API and facade](websockets.md) use the public
request/channel/loop contracts, with atomic bounded output, held frame resume,
ping/pong, validated close and separate handshake/connection lifetimes. The
initial application path rejects continuation and fragmented data frames;
transparent tunnels retain opaque fragmentation, as specified. Plain/TLS
fixtures cover ownership, protocol failure and shutdown without Kore types.
The [HTTP framer and proxy core](proxy-core.md) now lift Vectis's generic
headers, framing, upload/response parsing, policy views and WebSocket wire/rejection
machinery. An owner-loop curl transport runs independently of Kore through public
watches/timers, with bounded live-transfer coverage. The body framer and proxy
declarations are public. The [HTTP/SSE adapter](proxy-http.md) connects copied
route registrations, raw-path fallback, 100/103 output, duplex/trailer delivery,
error/commitment handling and worker admission. The
[WebSocket/WSS adapter](proxy-websocket.md) connects raw curl socket handoff,
validated opening metadata, opaque duplex relay and streamed HTTP rejections to
the same worker. Its plain/TLS fixture exercises copied in-memory trust and client
identity, early frames, fragmentation, shared admission and failure after commitment.
The [JSON facade](json.md) now connects public schema/record views, completed-body
parsing, generated/materialized replies, typed/group routes and incremental
selected-array input. The [XML surface](xml.md) lifts Vectis's mapped parsing/routes, ownership,
limits and context-local parser controls, with a bounded resumable libxml2 output
generator, synchronous sink and explicitly materialized serializers. The
[read-only asset/static surface](assets.md) now connects filesystem roots,
custom packed providers, incremental listing, validators/ranges and static route
semantics. The [storage/executor surface](storage.md) now supplies copied/pinned operation
arguments, provider capability checks, resources/results and acknowledged
cancellation, plus a bounded post-fork native job pool and read-only adapter.
The filesystem provider now lifts Vectis's direct-root/managed-cache engine,
conditions, quotas, collections, copy/move and recovery, with incremental PUT
transactions and a shared worker pool. The [WebDAV facade](webdav.md) now connects
filesystem/read-only/custom mounts, staged middleware, scoped owner-loop authorization,
conditional/range file replies, incremental PUT and bounded PROPFIND. Next connect
the Lua product and the remaining conveniences. The
[source execution surface](responses.md) connects native readers to the existing
bounded executor and public source readiness. The [upload stream facade](uploads.md)
connects Vectis's callback lifecycle to managed STREAM input and ordinary hooks.
Native reader and file helpers now share its transport, with copied metadata,
bounded pipe queues and native acknowledgement before result publication.
[Buffered REST](rest.md) connects decode, validation, handler/error callbacks and
bounded synchronous JSON replies to the same staged route chain. [OpenAPI](openapi.md)
connects copied router annotations, LoneJSON maps and JSON/YAML document generation.
The [address/admission/observation surface](policy.md) lifts Vectis's trusted-proxy
resolution and typed transport hooks, with generic application admission,
request association/cleanup and truthful terminal snapshots. Global application
state remains independent of dispatch-plan state, including early failures.
The shared autoblock table now lifts Vectis's capacity/window rules and
Linux/Darwin worker-death lock recovery, enabling the production profile.
[Logging](logging.md) now supplies borrowed pslog roots, copied access rules,
native formatter reuse and independent file draining, including restart and
bounded malformed-worker-log coverage.
Retain upstream HTTP2 live coverage, forced transport faults and slow-peer memory
measurements for the combined proxy review. Automatic HTTP/1.1 100 Continue now
uses managed interim output; its public fixture verifies the final wire/body
counters and rejects unsupported HTTP/1.0 expectations before body delivery.
The storage deadline path now checks absolute expiry at acknowledgement, covering
completion delivered before its queued timer under scheduler delay. Its focused
public fixture proves committed/empty late acknowledgement and disabled bounds.
Retain the
remaining ACME in-flight fault and final integration checks for the combined
review rather than repeating the full protocol matrix for each new surface.

The full goal also retains WebSocket integration/fault coverage, upstream/reverse proxy and
HTTP framing, WebDAV, REST/OpenAPI/assets, JSON/XML2 codecs, admission/logging,
the native-reader executor, Lua facade/public C binding, installed-consumer
coverage and final lifecycle/packaging proof. ACME progress does not narrow that
scope or establish Vectis replacement readiness.

The initial [Lua facade/binding](lua.md) now lifts Vectis's ordinary route,
group/default, request-table and live-source contracts through the installed
public shared C API. It uses the host VM and public process snapshots, protects
the entire callback conversion/cleanup boundary, and rejects unbound options.
Its installed-runtime fixture covers preload/dynamic loading and allocation
failure without adding a private router or source queue. The remaining Lua
families, source packaging and final consumer/artifact checks are still required.

The Lua configuration surface now maps deadlines, worker death policy, production
defaults, proxy bounds, trusted proxies and autoblock declarations to the same C
normalization path. Manual TLS file/memory/bundle materials, mTLS, HTTPS redirects,
effective snapshots and the serving gate are connected. Callback auth lifts the
existing decision/principal/challenge behavior through public staged route hooks;
credentials and sessions remain host-owned. Its combined fixture covers the live
wire behavior, copying, invalid decisions, protected getters and constructor
allocation unwinding. Material openers/ACME callbacks, remaining policy/control
hooks and the other Lua families remain part of the full goal.

Explicit [file-backed replies](responses.md) now lift Vectis's source and
generated-JSON materializers through public `rotmos_reply_source_spooled` and
`rotmos_reply_json_spooled`. They complete before returning, retain an unlinked
file descriptor and preserve the prior reply on failure. Borrowed inputs stay
pinned across callback/replacement cleanup, including the buffered helper.
Lua `spooled_source` uses that public operation with the existing source-table
contract and protected cleanup. Its focused fixtures cover actual file-size
failures, large output without a new ceiling, stack-record lifetime, HEAD and
Lua allocation/close failures. The remaining binding and packaging scope is
unchanged.

The Lua body/upload surface now reuses public body materialization/readers and
the owner-loop upload executor. It preserves Vectis's callback aliases, scoped
request table/state, bounded chunk delivery, default POST/204 and close timing.
Borrowed readers expire with their request; explicit copies retain independent
memory/file ownership. The installed fixture withholds the rest of a socket body
until Lua acknowledges its first chunk, and covers prefix/resume, lifetime and
callback/allocation failures.

Native Lua route conveniences now bind public static-file/directory,
upload-file and filesystem WebDAV registrations. They share route declarations,
auth decoding and callback ownership with ordinary Lua routes. Scoped staged
reply views preserve native payloads through after hooks and expose only public
builder/snapshot operations. WebDAV resource decisions remain in its C engine;
Lua is never called from native file/storage work. The installed fixture covers
wire behavior, scope/exception/OOM cleanup, principal refresh and registration
allocation unwinding.

Lua reverse-proxy configuration and all four callbacks now reuse the public C
proxy API, retaining Vectis's metadata copies, target indexing, helpers and local
response declarations. Scope retirement is protected through getters/OOM, and
argument failures latch in the binding without private proxy access. HTTP/SSE/
WebSocket forwarding and upstream TLS remain in the existing C engine. The short
installed fixture covers callback workflows, actual upload flow before EOF,
mutual TLS, metadata edits, scope expiry, local responses and failure mappings.
Standalone Lua WebSocket and SSE routes now use public acceptance, frame/send,
event encoding and live source operations. Public loop defer/timer/watch/waker
registrations provide owner-thread readiness without a second scheduler.
Userdata roots retire with their native operation; inherited parent loop tokens
are invalidated before child callbacks can access discarded handles. The combined
fixture covers real HTTP/mTLS connections, frame replay/output pressure, SSE
pause/resume, fork scope, callback failures and allocation unwinding.
Custom/packed asset/storage providers, typed routes and the other remaining
binding/packaging surfaces are still required.
