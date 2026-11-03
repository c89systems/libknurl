# Vectis drop-in integration contract

This is a normative companion to the [server API specification](server-api-spec.md).
The first delivery must replace Vectis's current Kore HTTP/web/REST/API server
through a small, mechanical adapter. Providing transport primitives from which
Vectis could rebuild its server is insufficient. The existing Vectis server is
the behavioral reference; extracting it must preserve its application interfaces.

## Definition of a simple replacement

Before the cutover, librotmos must provide and exercise the complete public
facade needed by the existing Vectis integration. The switch consists of an
installed dependency/build change, forwarding wrappers and generic provider/
lifecycle registrations, followed by removal of the old runtime and bridge.
No handler, route declaration, REST endpoint, source callback, auth provider or
service configuration must be redesigned to accommodate the replacement.

The permitted Vectis adapter does four things:

1. Maps copied configuration, enums, errors and checked numeric values.
2. Wraps generic request/reply handles behind the existing Vectis handles and
   supplies the existing application/context pointer to callback trampolines.
3. Registers Vectis-owned auth, packed-asset, storage, logging/metrics and
   process-lifecycle callbacks through ordinary public rotmos APIs.
4. Connects the existing outer supervisor to the server's readiness/control hooks.

HTTP parsing/framing, dispatch order, response/source execution, connection
queues, reader backpressure, proxy transport, WebDAV protocol and worker runtime
must already be implemented in rotmos. Finding one of those mechanisms inside
the proposed Vectis adapter means the extraction is incomplete. Public low-level
channels/loops remain available to native extensions; normal Vectis server
features use the facade rather than reconstructing those mechanisms.

## Public facade required for that adapter

Use the proposed `rotmos_server`, focused router/configuration families and
public callback userdata. Do not introduce a `vectis_app` equivalent, application
service graph or second route registry inside rotmos. A callback's userdata can
carry a Vectis app pointer without the library knowing its type or dereferencing
it. Generic registration records retain the shape of the Vectis declarations:
method/mask, path/kind, body policy, callbacks and userdata.

Ordinary routes use a staged reply, separate from the transport writer:

```c
typedef rotmos_status (*rotmos_route_fn)(rotmos_request *request,
    rotmos_reply *reply, void *userdata, rotmos_error *error);
```

`rotmos_reply` is an opaque declaration owned by the request. Its public
status/header/text/bytes/file/source/JSON helpers construct a reply without
starting network output. Auth and before/handler/after processing complete
before the router submits the selected reply to `rotmos_response` for framing
and delivery. A before hook may short-circuit; an after hook may inspect or
replace the handler result. Superseded source owners close once. A handler error
can still select the existing error response before anything is committed.
Low-level `rotmos_response_write/begin/end` explicitly perform live transport;
calling a Vectis response builder must not implicitly opt into that timing.

The facade owns the body-policy executor, upload/file/reader helpers and source
execution. Its public source/reader callbacks use C89 values and userdata, with
read/reset/close and explicit ownership. Vectis adapts `lc_source` by forwarding
those operations; it does not build another queue, thread bridge or drain loop.
Native blocking-reader work uses the library's bounded post-fork facility and
immutable request metadata, with results returned to the owner loop. Lua chunk
callbacks remain on their owning Lua thread. DSV/MCP/business parsers and their
dependencies remain in Vectis and consume these same generic facilities.

## Coverage of the existing server facade

Names in the first column refer to the inspected Vectis baseline
`6b1e263e0436687f68a7768ca55f4361a6e59e19`. Every row is required for the
first replacement. The public rotmos spelling may differ, but it must provide
the stated operation without additional server logic in the adapter.

| Existing Vectis surface | Required public rotmos coverage |
| --- | --- |
| `vectis_server_config`, TLS/listener declarations and production profile | All 34 server settings, body-limit derivation, effective values, TLS sources and presented-port behavior; Vectis resolves its own XDG/storage choices and passes ordinary values. |
| `start`, `stop`, `restart`, `run`, `wait`, `close` | Server run/stop/result/teardown and ordered role/readiness hooks fit Vectis's existing direct or supervised runtime. Its outer supervisor and service lifecycle stay in Vectis. |
| `route`, `prefixed_route`, Lua `route`/`group` and fixed JSON/text/redirect helpers | One registry; the same method masks, literal/parameter/regex order, captures, body policy, auth/before/after order and short-circuit outcomes. |
| `json_route`, `json_typed_route`, `xml_route`, `prefixed_json_route`, `prefixed_json_typed_route`, `prefixed_xml_route` | Corresponding public router registrations and codecs, typed input/output handlers and multiple output schemas, without text round trips or private record access. |
| `upload_stream`, `upload_file`, `upload_reader`; Lua `upload`, `dsv` and `mcp` | Generic chunk/file/reader execution, finite backpressure and completion/cancellation/close callbacks. Vectis retains the DSV/MCP integration and forwards its parser callbacks. |
| `dsv_route`, `prefixed_dsv_route`, `cai_mcp_route` | These Vectis-owned format/protocol adapters register ordinary routes with public body/source helpers. Their handlers and dependencies stay in Vectis; no custom transport executor is needed. |
| Request method/path/IP/param/query/header, `query_count`, `query_at` and `request_kore` | Public views separate raw query from decoded ordered pairs and exact-name first lookup, preserve absence behavior and path decoding, and provide the explicit Kore escape hatch. |
| `request_body_reader`, `body_materialize`, `body_read_all`, `body_spill`, `body_bytes`, `body_copy`, `body_path`, `body_is_spooled` | Distinguish borrowed request storage from independent caller materialization/copy/spill results, including caller buffers, reader rewind and explicit returned-file removal. No read-all implementation or hidden transport spool in Vectis. |
| `response_status`, `response_header`, `response_text`, `response_bytes`, `response_file` | Staged `rotmos_reply_*` helpers preserve copying, replacement, validation, content type and preparation-before-transmission behavior. |
| `response_source`, `response_stream_source`, `response_json`, `response_json_generated`, `json_reply`, `json_reply_status`, `response_error_json` | Explicit materialized/live/file-backed reply helpers with the same source transfer, serialization timing and typed-reply behavior, as detailed below. |
| `static_file`, `static_directory`, `static_embedded` | Generic filesystem and public asset-provider mounts preserve methods, indexes/redirects, media types, conditional/range behavior and path confinement. Packed lookup remains Vectis-owned. |
| `proxy_route`; Lua `proxy` | Public proxy config and copied hook views cover all rewrite/response/error hooks, streaming HTTP/SSE, HTTP/2 upstreams and raw WebSocket relays. No direct curl/Kore/TLS transport remains in Vectis. |
| `websocket`; Lua `websocket` and `sse` | Existing frame/source callbacks and public C SSE encoding/route helpers preserve event fields, size bounds, default headers and lifecycle. An SSE route does not acquire a new buffering layer or binding-private formatter. |
| `webdav`, `webdav_embedded_site`, `webdav_embedded` | Public filesystem/read-only/custom provider mounts, generic auth decisions, conditional mutations, quotas and recovery. Vectis supplies asset/Lockd/credential providers. |
| `openapi_doc`, `openapi`, `route_count` | Public registry metadata, count and OpenAPI JSON/YAML generation over that same registry. Business schema descriptions come from Vectis; no duplicate route registry is needed for documentation. |
| Lua `vectis.rest` server helpers | Buffered JSON decode, validation, error mapping and response composition fit the route/reply facade unchanged. Ship generic server helpers through `rotmos.rest`; Vectis keeps its error-envelope/application policy and its outbound REST/HTTP clients. |
| `auth_routes`, `auth_json`, `metrics`, application-named admission events | Ordinary public routes/middleware plus generic admission/observation hooks support these endpoints unchanged. Credential/session flows and metrics aggregation/persistence stay in Vectis. |
| ACME restore/persist, worker resources, parent timers, control channel, `logger` and pslog | Typed public callbacks carry role/context and existing host decisions. No Lockd client, Vectis protocol implementation or service primitive enters rotmos. |

## Behavior that forwarding must preserve

The facade must encode these distinctions explicitly; adapter code must not
infer them from private response tags or backend state:

- `response_bytes` copies the caller's bytes before returning; it stages the
  selected reply. Later header/payload changes occur before transport submission.
- `response_header` appends, including repeated Set-Cookie fields. Status-only
  selection clears the old payload/content type while preserving added headers.
  Staged framing declarations retain their existing submission-time normalization;
  they are not passed to the stricter low-level response-header setter.
- A successful ordinary C handler with no reply produces 204. Callback failures
  preserve phase-specific bridge mappings, independent selector/body rejection
  statuses and host error-envelope policy. Auth/before short-circuiting skips
  later hooks; an unselected Lua source declaration acquires no native ownership.
- `response_source` is explicitly materialized/file-backed. `response_stream_source`
  takes a live source on success and closes it when output finishes or aborts.
  The source's initial reset behavior and position follow the existing Vectis
  contract; the low-level producer must not add another implicit reset.
- `response_json` materializes JSON during the builder call. Vectis's
  `response_json_generated` currently serializes into a temporary response file
  before returning; preserve that explicitly file-backed helper. Neither may
  silently retain a handler's stack record for later serialization. True live
  JSON generators remain a separate operation with an explicit owner lifetime.
- Lua route handlers retain their request-table and `nil`/string/response-table
  result contract, including status aliases, headers, `file_path`,
  `spooled_source` and `stream_source`. Nil means the existing 204 response.
  Hooks see the unprocessed handler result before reply conversion, as today.
- Existing Lua source tables retain `read(max_bytes)`, optional `reset/close`,
  nil/false EOF and oversized-chunk remainder handling. The facade normalizes
  them to the public producer contract without requiring source rewrites or
  concatenating the full output. Native low-level AGAIN/EOF conventions do not
  become mandatory new return values for existing Vectis Lua sources.
  Keep the existing protected numeric-string conversion and zero-length chunk
  EOF behavior when normalizing those source tables.
- Keep the working public LoneJSON schema assignment/record ownership policy.
  Callback-writer input restrictions, buffered external-record support and
  schema/map selection remain as in Vectis.
- Preserve XML's distinct mapped-codec and DOM defaults: mapped depth 64 and
  text bound 64 MiB, versus DOM document bytes 16 MiB and XPath result nodes
  4096. A DOM limit must not silently become a smaller streaming-route limit.
- Map Vectis's effective configuration, status/error envelope, logging policy,
  body/spill defaults and callback order centrally. New standalone rotmos
  defaults or convenience limits cannot silently tighten an existing Vectis
  route or change its output. Changes beyond an API adaptation need a concrete
  reason and separate specification; do not accumulate redesign into extraction.
- A route callback may request whole-server stop through its process-local server
  handle. Rotmos forwards that request to the parent and drains under the shutdown
  policy; it must not merely exit/restart the calling worker or require a new host
  control protocol. Request close revokes handles even if an independently pinned
  cancelled native operation still has context to retire.
- Write-idle guards pending downstream output, not an empty queue waiting for a
  producer/upstream. Existing route/proxy deadlines remain effective; new default
  storage-operation deadlines can be explicitly disabled where Vectis had none.

## Cutover readiness and verification

Build a reference adapter and representative host callbacks inside librotmos
while implementing the facade, against installed public headers/libraries.
It must exercise the mapping above with plain host context and mock asset/auth/
durable-storage providers, without Vectis includes or a sibling source dependency.
Develop these consumers alongside each feature, so missing operations are fixed
here before the dependency is offered for the Vectis switch.

The acceptance evidence includes staged reply edits/replacement after handler
return, sources larger than output capacity, stack-backed JSON values whose
scope ends on builder return, source reset/close/error ownership, every body
materialization mode, prefix/middleware ordering, REST envelopes, OpenAPI and
the existing TLS/proxy/WebDAV/process scenarios. Installed C and Lua workflows
must cover the same facade. A low-level transport example alone is insufficient.
Include caller materialization results retained after request close, explicit
returned-file removal, decoded duplicate query lookup, empty C replies, staged
header normalization and whole-server stop from an active worker. Test SSE event
encoding from public C and Lua and a quiet producer with no pending output.

Before declaring the library ready for the Vectis replacement, review the exact
proposed Vectis adapter/build delta against this contract and run the affected
existing Vectis regressions, including normal/packed Lua paths and representative
site/REST applications. That consumer change requires its own repository
authorization. During implementation here, port the applicable observable cases
and prepare the adapter design; do not postpone API coverage until switch day.
Tests must keep existing application inputs and expected behavior. Rewriting
handlers or weakening expected results to fit rotmos does not prove a drop-in.

Source evidence: [public facade](../../vectis/include/vectis/vectis.h),
[runtime configuration](../../vectis/src/vectis_internal.h),
[reply/reader implementation](../../vectis/src/vectis.c),
[Kore bridge](../../vectis/src/vectis_kore_bridge.c),
[Lua app contract](../../vectis/manual/lua-app.md),
[Lua REST contract](../../vectis/manual/lua-rest.md),
[HTTP regression](../../vectis/tests/lua/http.cmake) and
[source lifetime regression](../../vectis/tests/lua/app_source_lifetime.lua).
