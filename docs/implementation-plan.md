# Extraction implementation plan

The [small extraction contract](server-api-spec.md) governs the extraction.
The [audit matrix](server-api-audit.md) records the current operation-family
coverage; [verification](verification.md) records the completed development
checks and their limits. Preserve Vectis commit
`6b1e263e0436687f68a7768ca55f4361a6e59e19`; sibling repositories stay read-only.

Completed implementation includes the enhanced Kore backend, proxy, WebDAV,
static/providers, upload, request/body/reply/source, JSON/XML, TLS/ACME,
WebSocket/SSE, policy/logging, lifecycle/control and host-owned Lua binding.
The reference adapter maps baseline server, ordinary/typed route, body/source,
service/provider and codec declarations through installed public APIs.

The corrective work has removed the duplicate Lua typed-route path and second
REST JSON pass, restored synchronous ordinary JSON output before after hooks,
made XML routes parse live bounded upload readers, and restored caller-owned
selected-array iteration/rewriting. Lua codecs and REST phase sequencing reuse
public Core operations. The public framer and speculative ACME generation store
are removed; native executor and transport machinery remain private.

The current route batch completes fixed Lua text/JSON/redirect/authenticated-JSON
helpers, binds Lua REST to C REST registrations, preserves editable request bodies,
validation short circuits and error-policy failure behavior, and forwards the
standalone mapped XML parse/write/materialize helpers. No private dependency
layout or second codec enters the adapter.

The initial accumulated baseline compatibility and streaming/failure checks passed,
but Vectis's subsequent assessment identified G1–G5 beyond that gate's coverage.
The follow-up exposes standalone filesystem calls through the existing engine,
corrects mount/default admission and deadlines, removes fixed concurrency guards,
and forwards terminal proxy accounting. The [audit](server-api-audit.md) records
the specific regressions and scope; the initial results are not a full cutover certificate.
Combined restart/cancellation, live proxy pressure/reuse, HTTP/2 duplex delivery,
DAV replacement recovery and raw cleartext/TLS header guardrails pass alongside
the codec, route, ownership and lifecycle cases. Installed C/Lua consumers,
seven-target SDK final-byte/relocation checks and Lua source/rock reconstruction
also pass. Historical fault-injection limits remain documented in KORE.md.
Actual Vectis cutover and release signing/tagging/publication remain outside
this authorization.

Standalone C JSON validation, every DOM/XPath/SAX receiver operation and the
host readiness/control/fork-exit translation now have concrete adapters and
baseline cases. XML needed no new Core interface. Terminal worker callbacks can
reclaim their fork's server and host after native teardown; callback failures
still reach the parent. Host framing, shared readiness ownership, auth policy
and outer supervision remain outside Core.

Build only through explicit supported toolchains. The default selected target
uses `build/debug` or `build/release`; the other six use
`build/<target>/<configuration>`. Compile owned C, bindings and selected backend
with strict C89. Keep pristine Kore and replay external patches into disposable
build trees. Consume the pinned released SDK closure and the host Lua 5.5.1 VM.

Implement first, run affected invariants/failure cases once per coherent batch,
then broaden only for missing evidence or a new failure. Do not claim the Vectis
cutover is verified: actual consumer edits and its normal/packed regressions
need separate authorization. Do not push or release.
