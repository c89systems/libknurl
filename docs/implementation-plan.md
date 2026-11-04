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

The next broad surface is the application WebSocket API and facade, using the
existing public request/channel/loop lifetime contracts. Preserve Vectis handler
behavior, bounded messages/output, fragmentation, ping/pong, close/error
notification and owner-loop execution without exposing private Kore structures.
Add short observable framing, ownership and failure cases. Continue with the
upstream HTTP/framing/proxy surface and provider/codecs afterwards. Retain the
remaining ACME in-flight fault and final integration checks for the combined
review rather than repeating the full protocol matrix for each new surface.

The full goal also retains application WebSockets, upstream/reverse proxy and
HTTP framing, WebDAV, REST/OpenAPI/assets, JSON/XML2 codecs, admission/logging,
the native-reader executor, Lua facade/public C binding, installed-consumer
coverage and final lifecycle/packaging proof. ACME progress does not narrow that
scope or establish Vectis replacement readiness.
