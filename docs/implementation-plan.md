# Extraction implementation plan

The [small extraction contract](server-api-spec.md) governs the active goal.
The [audit matrix](server-api-audit.md) records the current operation-family
coverage and concrete remaining gaps. Preserve Vectis commit
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

Continue in coherent implementation batches:

1. Finish the accumulated baseline compatibility and streaming/failure gate.
   Cover combined restart/cancellation, proxy pressure and DAV recovery with
   existing fixtures and short targeted additions. Resolve discrepancies against
   Vectis, rather than accepting the current implementation as its own oracle.
2. Complete installed C/Lua workflows and remaining lifecycle final-artifact
   evidence. Reuse current seven-target SDK and Lua source/rock reconstruction
   checks; keep release signing/tagging/publication outside this authorization.

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
