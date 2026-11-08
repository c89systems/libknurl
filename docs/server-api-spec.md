# librotmos extraction contract

This is the complete product contract. Feature guides describe the implementation;
they cannot enlarge this contract or establish compatibility.

## Purpose and boundary

Extract Vectis's working enhanced Kore HTTP server, reverse proxy, WebDAV and
server helpers into librotmos. Librotmos owns their implementation, its public
`rotmos_` C API and its `rotmos` Lua module. Kore is the private backend.
Vectis consumes the library and removes the extracted implementations.
The cutover preserves existing application C/Lua calls, handlers, declarations,
configuration, output, streaming and lifecycle behavior through forwarding.

Behavioral baseline: Vectis commit
`6b1e263e0436687f68a7768ca55f4361a6e59e19`. Its public declarations, implementation
and regressions specify existing defaults, accepted inputs, errors and lifetimes.
The [integration contract](vectis-integration-contract.md) identifies the sources.
A discrepancy is resolved by tracing that baseline, not by inventing a new default.

Vectis retains its app/service graph, outer supervision, business handlers,
credentials/sessions, application admission/proxy decisions, metrics aggregation,
packed-asset lookup, Lockd/Pouch/durable-storage clients, XDG path selection,
DSV/MCP adapters and outbound HTTP/REST/WebDAV/SSH clients. Librotmos exposes
callbacks/providers for these integrations; it never imports their private types.

## Executable surface

These are required operation families, not permission to invent independent
products. C registrations use configuration values, callbacks and userdata.
Lua normalizes arguments and binds the same C operations. Exact C spellings may
change before release; each operation must map to a concrete baseline consumer.

| Family | Operations and preserved behavior |
| --- | --- |
| Server | Initialize/copy/validate configuration; create, run, request stop, destroy; effective settings, bound listeners and run result. Preserve foreground and supervised Vectis use, worker restart/terminate, readiness/serving gate, signals and graceful shutdown. Vectis retains start/restart/wait supervision wrappers. |
| Routes | Register ordinary and verb routes, literal/parameter/regex paths, method masks, prefixes/groups, fixed text/JSON/redirects, route count and captures. Preserve registration order/conflicts, auth/before/handler/after order, short circuit, empty 204 and phase-specific failures. One registry serves routing and OpenAPI. |
| Requests/bodies | Method/version, raw target/path/query/Host, decoded captures and ordered duplicate headers/query pairs, first lookup, peer/local/TLS/listener metadata, resolved IP/correlation ID, host userdata and close outcome. Preserve NONE/BUFFERED/MATERIALIZED/STREAM/upload policies, limits, live chunks, read/rewind, borrowed readers, caller-buffer materialization, independent copy/spill/path results and explicit cleanup. |
| Replies/sources | Staged status, append/set headers, text, binary bytes, files, materialized sources, file-backed sources and live sources; read/reset/close and cancellation. Copying, replacement, reset timing and exactly-once cleanup match Vectis. After-hooks finish before transport submission. Explicit live output retains write/end/abort, backpressure and commitment; ordinary builders do not acquire live-output timing. |
| Uploads | Chunk open/write/finish/close, file destination and bounded native-reader handlers; partial input, completion, cancellation and reply publication. The library owns queues and transport execution. Native parsers consume bounded readers; Lua callbacks execute on their owning Lua thread. |
| JSON | LoneJSON mapped parse/validate, request/source selected-array iteration and source rewriting with caller-owned item storage, typed input/output routes and per-response output schemas/statuses; materialized serialization, synchronous sink output and explicit live generators. Ordinary JSON replies and ordinary typed-route output finish serialization before after-hooks. Existing generated-file replies finish writing their private file before returning. |
| XML | Lift libxml2 mapped parse/parse-record, serialize/write, typed routes, owned DOM/root/node/name/text/attribute/edit/append operations, namespace-aware XPath and incremental SAX feed/finish/close. XML routes parse live bounded upload input into their record; they do not first collect or spool the whole transport body. Preserve mapping options and separate mapped/DOM/SAX limits. |
| REST/OpenAPI | Buffered REST JSON decode, validation, handler/error handling, response and error-envelope helpers, route/group convenience; encode replies once. Route annotations include summaries, operation IDs, tags, request/response schemas; generate OpenAPI JSON/YAML from the route registry. Vectis retains application error policy and outbound REST clients. |
| Static/assets | File, directory and read-only custom/packed-provider mounts; open/stat/read/list/close, broad built-in MIME inference and copied instance/route extension overrides, indexes, redirects, conditional requests and ranges. Explicit route content type wins, then route mappings, instance mappings, provider metadata and built-ins. Filesystem serving works independently. Vectis supplies packed lookup through the provider boundary. |
| WebDAV/storage | Filesystem, read-only and custom mounts; standalone filesystem operations without a server; OPTIONS/GET/HEAD/PUT/DELETE/PROPFIND/MKCOL/COPY/MOVE, finite depth, Destination/Overwrite, auth/principal, ETags/conditions, quotas, locking, tombstones and partial-failure/recovery outcomes. Lift existing direct-root/managed-cache semantics unchanged. PUT input and PROPFIND listing flow incrementally. No LOCK/UNLOCK or new storage format. Vectis supplies packed/durable-storage adapters. |
| Reverse proxy | Lift HTTP/HTTPS/SSE proxying, HTTP/2 upstream support and WebSocket/WSS opaque relay. Preserve configured targets, raw path/query handling, forwarding-header sanitation, TLS verification/CA/client identity, admission, preflight/rewrite/response/error hooks, interim responses, duplex queues, trailers, deadlines, half-close, terminal exchange accounting and failure before/after commitment. No body collection/spooling. |
| Application WebSocket/SSE | WebSocket route/accept, connect/message/disconnect, send text/binary/frame, close and existing limits/protocol handling; bounded connection lifetime separate from HTTP. SSE route/source open/read/close plus event encoding for comment/id/event/retry/data/preformatted frame, headers and existing per-event bounds. |
| TLS/ACME | HTTP/HTTPS listeners, TLS versions/cipher list, manual bundle or separate key/certificate/chain from files/memory/sources, client CA/mTLS, SNI/Host policy and 308 redirects with presented ports. Lift Kore ACME account/order/challenge/renewal/recovery; expose restore/persist/error hooks for the host's durable store, preserving activation/failure ordering. Runtime files are working state, not a new durable product. |
| Policy/logging | Trusted-proxy resolution, admission, shared autoblock status/event/window/capacity rules and named events; request/lifecycle/transport observations. Borrow public pslog root/access loggers, preserve derived sub=http, enable/disable, category/status levels and independent file access logs. File logs default to 10 MiB per file and three total files, preserving whole records; size/count/disable are configurable. File write/rotation errors, including ENOSPC, report an error and drop that record while HTTP continues; later records retry. Host metrics and policy decisions stay external. |
| Host/backend integration | Parent/worker/key-manager/ACME start/ready/stopping/stopped/exit/error notifications, process context, control FD/readiness and parent timers; request start/close and TLS-state observations. Supply only the loop/watch/timer/defer/wake operations needed by these consumers. An opt-in Kore request accessor preserves vectis_request_kore(). |

## Configuration and ownership

Preserve all baseline server fields, normalization and effective values:
max_connections, request_limit, worker_count, worker_accept_threshold,
worker_rlimit_nofiles, worker_set_affinity_disabled, worker_shutdown_timeout_ms,
max_request_header_bytes, max_request_body_bytes, request_body_spool_dir,
request_header_timeout_ms, request_body_idle_timeout_ms,
response_write_idle_timeout_ms, request_body_min_rate_bytes_per_sec,
request_body_min_rate_grace_ms, idle_timeout_ms, keepalive_disabled,
keepalive_timeout_ms, keepalive_max_requests, kore_curl_timeout_seconds,
kore_curl_recv_max_bytes, kore_quiet, socket_backlog, request_process_budget_ms,
hsts_max_age_seconds, websocket_max_frame_bytes, websocket_timeout_ms,
server_header, access_log_path, access_log, pretty_error_pages,
worker_death_policy, client_ip and autoblock. Preserve route-specific body,
proxy, upload, filesystem, XML and TLS settings from their baseline declarations.
Vectis resolves app paths/storage endpoints before passing generic configuration.

Expected failures return status and bounded diagnostics. Preserve baseline
ownership explicitly: copied declarations/bytes, borrowed callbacks/schema/logger
owners, successful take operations, failed-transfer retention, partial cleanup,
and request/connection scope expiry. Do not add restrictive limits, deadlines,
threads, storage layouts or callback timing changes merely to simplify a wrapper.
Necessary C89/thread-affinity fixes must preserve observable behavior and have a
specific baseline case; material product changes require a separate decision.

Streaming means bounded producer-to-consumer flow before source EOF. Full-value
buffering or spooling is allowed only by an explicitly selected materialized or
file-backed operation. Blocked consumers pause producers; cancellation retains
native work owners until acknowledgement. Keep supporting machinery private
unless a demonstrated consumer requires it as public API. In particular, a
standalone framer and general-purpose job SDK are not extraction deliverables.

## Lua and dependency reuse

Ship `rotmos` as a Lua facade and C binding against installed public shared
librotmos. Use the host's Lua 5.5.1 VM, including embedded hosts; supply a public
preload entry point. The binding owns neither a second VM nor a private runtime.
Expose the operation families above with existing Vectis argument/result shapes:
server/verb/group helpers; scoped request tables; nil/string/response declarations;
source read/reset/close; uploads, codecs, DOM/SAX, REST/OpenAPI, mounts, proxy,
WebSocket/SSE, logging and lifecycle callbacks. Errors, binary strings, exact
integer counts, owner retention, callback protection and finalizers are preserved.

LoneJSON owns JSON parsing, mapping and generation. Reuse its public schema/record
interop: create Lua-owned records, assign private writer inputs through schema
assignment, retain owners and clear records. Preserve buffered external-record
support and callback-writer input restrictions. No encode/reparse workaround,
private userdata cast, parallel route engine or speculative dependency extension.
libxml2 owns XML parsing; pslog owns logging. Vectis's packed/static executable
keeps one runtime and its existing Lua facade over the C adapter; loading a second
shared runtime beside it is not a supported cutover strategy.

## Build and delivery

Keep pristine pinned Kore in vendor/kore; keep ordered patches outside it and
apply them only to disposable build/test copies. Preserve all 42 Vectis patches;
additional patches need a concrete extraction, C89 or parity reason. Upgrade by
replacing the pristine revision and replaying/fixing the external patches.

Compile owned C, binding and selected Kore backend with strict -std=c89. Public
headers support C89/C++; dependency/private layouts stay private. Consume pinned
released c89systems cpkt, LoneJSON and libpslog packages, preferring the newest
compatible closure. Current pins are cpkt 0.6.1, LoneJSON 0.46.0 and pslog 0.13.0;
cpkt provides OpenSSL/curl/nghttp2/libxml2 and the Lua runtime/link closure.
No sibling checkout is a build input or writable under this task.

Follow pkt-systems-cmake-lifecycle: explicit selected toolchains, serial operations,
static/shared installed SDKs and relocated consumers for x86_64-linux-gnu,
x86_64-linux-musl, aarch64-linux-gnu, aarch64-linux-musl, armhf-linux-gnu,
armhf-linux-musl and arm64-apple-darwin. Default selected-target builds use
build/debug or build/release; others use build/<target>/<configuration>.
Ship the Lua source archive, rockspec and source rock, using the same shared
closure. Complete required final artifact checks before release; no push or
release is authorized by this contract. HTTP/2/3 listeners, Windows, Kore's
standalone CLI/optional language/database modules and new application services
are outside this extraction.

## Completion gate

Prepare an installed-public-API reference adapter inside librotmos before further
API expansion. It must map the baseline operation families above using only
configuration/error/type translation, callback forwarding, handle association and
provider registration. It must contain no replacement codec, HTTP state machine,
queue, proxy, WebDAV implementation or new application requirement.

Port baseline observable cases, including JSON serialization/after-hook timing,
live XML parsing before request EOF, selected-array caller storage, one-pass REST
encoding, source cleanup, proxy pressure, DAV conditions and lifecycle failures.
Tests must preserve the baseline inputs and results. Add short invariant/failure
checks; batch broader checks after coherent implementation work. Prove installed
C/Lua workflows and all target builds. Vectis cutover verification needs separately
authorized Vectis edits and its existing normal/packed regressions. An existing
rotmos test pass or the previous implementation is not a substitute for parity.
