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
Next implement JSON/XML2 codecs and route facades, then storage/assets/WebDAV and
REST/OpenAPI surfaces. Retain upstream HTTP2 live coverage, forced transport faults
and slow-peer memory measurements for the combined proxy review. Retain the
remaining ACME in-flight fault and final integration checks for the combined
review rather than repeating the full protocol matrix for each new surface.

The full goal also retains WebSocket integration/fault coverage, upstream/reverse proxy and
HTTP framing, WebDAV, REST/OpenAPI/assets, JSON/XML2 codecs, admission/logging,
the native-reader executor, Lua facade/public C binding, installed-consumer
coverage and final lifecycle/packaging proof. ACME progress does not narrow that
scope or establish Vectis replacement readiness.
