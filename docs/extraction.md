# Vectis extraction boundary

This is the implementation direction, not an available API reference. Only the
source-vendoring foundation described in the README is currently implemented.
The [server API specification](server-api-spec.md) now supplies the researched
contract and supersedes this document's preliminary API direction.

## Agreed ownership

Vectis will consume a librotmos-owned C API. Kore is its backend, with the pinned
pristine implementation in `vendor/kore/` and ordered external patches applied
only to generated build copies. Retain an explicit backend escape hatch so the
existing `vectis_request_kore()` capability can be carried through the cutover.

librotmos owns the HTTP runtime, including connection/request limits, timing,
workers, TLS and ACME machinery, WebSockets, body delivery, response writes,
static-file transport and structured/access logging. Its API needs explicit
ownership, lifetimes, startup errors and teardown semantics. Preserve every
runtime enhancement currently used by Vectis.

The selected surface also includes the reusable router/profiles, upload/static
helpers, typed JSON and libxml2 XML codecs/routes, reverse-proxy engine, WebDAV
protocol/filesystem provider, and generic trusted-proxy/autoblock mechanisms.
Ship a Lua 5.5.1 facade over a C binding of the installed public shared API; that
binding must neither include private headers nor embed another runtime copy.
It uses the host's existing Lua VM, including embedded Lua, with a public preload
path; the host owns VM creation and destruction. Separate packaging does not
mean a separate interpreter. The [specification audit](server-api-audit.md)
records the ownership, async progress and packaging corrections.

Vectis retains its public application C/Lua facade, route declarations/business
handlers, auth/credential providers, selected admission/proxy policy, metrics
aggregation/persistence, outer supervision/services, packed assets and Lockd
storage integration. Generic server engines move to librotmos; Vectis provides
application adapters rather than maintaining duplicate routing/proxy/WebDAV
implementations. Vectis types/globals and storage clients stay outside the library.

The integration should read as a Vectis adapter configuring a librotmos server,
registering callbacks, translating request/response operations and handling
lifecycle events. It should not require Vectis to compile Kore sources, supply
Kore-specific build definitions, resolve private runtime symbols or mutate Kore
globals. Direct Kore access must be deliberate and backend specific.

## Current seams to replace

The inspected Vectis baseline is commit
`6b1e263e0436687f68a7768ca55f4361a6e59e19`:

| Existing seam | Required extraction outcome |
| --- | --- |
| Private `vectis_kore_runtime` object target and Kore compile definitions | Consume the library's exported CMake target and installed public headers. |
| `vectis_kore_bridge.c` runtime configuration and global writes | Translate Vectis configuration into the librotmos interface. |
| `kore_vectis_runtime_symbols` callback table | Library-owned callback registration, without required Vectis symbols. |
| `vectis_kore_autoblock_*` native hooks | Generic native rule engine and admission/events in rotmos; Vectis supplies configuration/application events. |
| Parent timers, worker exit and teardown hooks | Defined parent/worker lifecycle callbacks with fork-safe ownership. |
| `vectis_kore_acme_state_persist()` | A persistence callback preserving Vectis's Lockd workflow. |
| Prebody handoff and body-chunk callbacks | Preserve borrowed bytes, backpressure, rejection and cleanup behavior. |
| `vectis_request_kore()` | Explicit backend escape hatch with a documented borrowed lifetime. |
| Public Vectis C/Lua application surface | Preserve it across the replacement. |

The bridge is approximately 3,900 lines and directly uses Vectis internal types.
The Kore proxy adapters also depend on Vectis-owned route selection, telemetry,
curl transfers and policy. Copying those files into librotmos would not establish
an independent library boundary. Trace and translate the seams rather than
blindly importing that application layer.

## Dependency and language baseline

The recorded compatibility baselines match Vectis's pins. Recheck published
releases before locking implementation SDKs and prefer the newest compatible
complete dependency closure:

| Dependency | Initial candidate | Responsibility |
| --- | --- | --- |
| [cpkt](https://github.com/c89systems/cpkt/releases/tag/v0.6.1) | 0.6.1 | TLS/curl/libxml2 closure, test support and Lua 5.5.1 for the separate binding product. |
| [lonejson](https://github.com/c89systems/lonejson/releases/tag/v0.45.0) | 0.45.0 | JSON request helpers and ACME parsing. |
| [libpslog](https://github.com/c89systems/libpslog/releases/tag/v0.12.0) | 0.12.0 | Structured runtime/access logging. |

Acquire exact published SDK assets with target identities and SHA-256 pins from
`github.com/c89systems/`. Keep independent installation prefixes and consume
upstream imported targets with complete static/shared link closure. Do not build
against sibling checkout sources, headers or generated state. Application-level
Lockd, database, audio and other Vectis dependencies do not automatically become
librotmos dependencies.

The requested implementation standard is strict `-std=c89`. Vectis currently
compiles its Kore object runtime and main bridge as GNU99. The imported snapshot
and patches are therefore source preservation, not C89 compliance evidence. Port
the selected backend and its public headers through explicit patches and prove
the real compilation commands. There must be no hidden GNU99 exception for the
backend. An unchanged external dependency's implementation standard is separate
from librotmos's own C89 compilation contract.

## Required evidence before Vectis replacement

1. Implement the specified librotmos API with public documentation and runnable
   C examples. Prove request, response, configuration, ownership, failure and
   callback contracts through observable tests.
2. Compile the backend under strict C89. Preserve Vectis-used HTTP, TLS/mTLS,
   ACME, WebSocket, static-file, method, parser and timing enhancements. Initially
   preserve the existing feature selection; Kore's optional Python, Lua,
   PostgreSQL, JSON-RPC and standalone tools are not automatically new products.
3. Prove true producer-to-consumer body/response flow with bounded memory,
   backpressure, cancellation and prebody handoff. Do not substitute full-body
   collection or spooling for streaming.
4. Test restart, signal preservation, worker death, bounded shutdown, parent
   timers and cleanup across process/fork boundaries. Include relevant musl
   default-thread-stack regressions for substantial stack use.
5. Export and test static/shared library metadata and installed consumers,
   including the backend escape hatch and absence of required Vectis symbols.
6. Deliver the reusable toolkit and packaged Lua facade/public-shared-API C
   binding, including installed-consumer tests and the Lua source archive,
   rockspec and source rock. C-only consumers do not acquire a Lua runtime.
7. Migrate Vectis in its own authorized change: remove the vendored runtime build
   and patch stack, connect the librotmos adapter, and run its HTTP/TLS/proxy,
   streaming, Lua, lifecycle and packaging regressions. Preserve its independent
   public application contract.

No Vectis files have been modified by this bootstrap. This is neither a release
nor evidence of a completed runtime extraction.
