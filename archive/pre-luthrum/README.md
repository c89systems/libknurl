# Knurl Lua/executable handoff to Luthrum

Archived on 2026-11-15 by explicit product decision. libknurl ships as a C89
library and C SDK with Kore convenience APIs. The Lua binding and standalone
`knurl` server are deferred until Luthrum is ready. This archive is source
reference, not an additional build, install, package or release product.

## Decision and runtime boundary

C89 Systems is developing **Luthrum**, a modified Lua runtime intended to
multiplex coroutines onto OS threads. That runtime direction replaces the
attempt to solve general Lua scheduling inside Knurl. Luthrum is underway;
its public scheduling, C-binding, ownership and ABI contracts were not available
or evaluated in this work. Do not invent its API or assume compatibility.

libknurl retains its C receivers/free functions, route convenience APIs, JSON/XML,
REST/OpenAPI, streaming, static mounts/ranges/compression, reverse proxy with
upstream HTTP/2, WebDAV, ACL/IP/DNS/country MMDB, TLS/ACME, pslog integration,
worker lifecycle and native task primitives. HTTP/2/3 downstream listeners and
SOAP remain outside the delivered feature set. The shared-library ABI remains 0
under the existing unreleased-product decision. Application supervision and
domain-specific services remain host concerns.

The current release inventory is seven C SDK tarballs and one CHECKSUMS file.
There is no standalone server tarball, Lua source archive, rockspec or source
rock. Active builds acquire only cpkt, LoneJSON and libpslog SDKs. Lua provider
sources and libpid0 SDK inputs are recorded here for restoration and are no
longer acquired. The external cpkt SDK may contain unused runtime components.

## What is preserved

| Location | Snapshot |
| --- | --- |
| `binary/` | Static executable, embedded startup, four example servers, man page, package and CLI/HTTP/scratch-container tests. |
| `lua/` | Facade, editor metadata, public interop header, source manifest, LuaRocks/source packaging and provider build integration. |
| `src/lua/` | Native bindings, route coroutine prototype, handwritten curl wrappers and async adapter. |
| `tests/lua/` | Preload/dynamic hosts, integration fixtures, adversarial inputs, allocation failures and independent adapter example. |
| `docs/` | Previous product specification and Lua/concurrency documentation; affected C/Lua guides are preserved as historical references. |
| `scripts/`, `cmake/`, `workflows/` | Previous build recipes, dependency pins, release collector, check matrix and macOS job. |
| `tests/Lua*.cmake`, `tests/SourceInputs.cmake` | Deferred source-product packaging, privacy and consumer checks. |
| `PROJECT-README.md`, `licenses/` | Previous public entry point and executable-specific notice. |

The last async implementation commit was `80f5f995ef07f7307012095dc8d2e52ccb3a1a04`.
The executable and some TLS/ACME supporting work were still uncommitted then.
This archive captures the actual working files, including that executable work;
it is not merely an export of the commit. C improvements remain active with
their tests. Historical build scripts retain old source layouts; archive entry
points reject execution. Historical relative documentation links point to the
preserved file when available, otherwise to the current source guide.

## Executable product requirements to revisit

- Name: `knurl`; accept a user `.lua` script returning a configured server.
- Fully static Linux musl executable, x86_64 and AArch64 only. Embed the chosen
  runtime rather than requiring a system interpreter or dynamic modules.
- Separate `knurl-X.Y.Z-TARGET.tar.gz`, containing `knurl-X.Y.Z-TARGET/bin/knurl`,
  `share/doc/knurl/` and the section-1 man page under `share/man/man1/`.
- `--help`, `--version`, `--check FILE.lua` and `--dump-examples [DIRECTORY]`.
  Dumps must not overwrite existing files and must roll back partial creation.
- Four examples: static with IP ACLs; proxy; authenticated WebDAV with an
  explicitly illustrative key; combined static/proxy with IP/country ACLs.
  Country example allows SE, NO, FI, GB, DE, US, CL and denies KP, VE and unknown.
  GB represents the UK. Missing MMDB diagnostics explain DB-IP download,
  extraction, configuration and attribution. The database is not redistributed.
- Expose the C server capabilities through the future binding, including
  proxy/static/WebDAV, ACL/MMDB, TLS/ACME and security/time-limit configuration.
- Retain curl and LoneJSON integration for outbound API calls, including true
  incremental uploads, downloads and schema-mapped JSON. `json` is explicitly
  buffered; a streaming API must not collect the entire value behind the scenes.
- Native libpid0 integration permits PID 1 in a FROM-scratch container. The
  prototype used libpid0 0.6.0. Preserve signal forwarding, child/orphan reaping,
  exit status and bounded shutdown behavior when selecting its future version.
- System CA discovery must work in scratch when a Containerfile copies Alpine's
  CA bundle to `/etc/ssl/certs/ca-certificates.crt`. Explicit invalid trust
  overrides fail closed. Never embed fixture private keys or a production trust
  bundle in release artifacts.

See [the original executable specification](docs/knurl-executable-spec.md) for
the detailed historical requirements and [binary guide](binary/README.md) for
the implemented prototype surface. Those documents do not authorize shipping.

## Concurrency investigation and implemented prototype

Kore uses a fixed prefork worker pool. One event loop handles many connections
in each worker. Blocking synchronous work in that loop occupies the whole
worker; there is no automatic process/thread per request. A bare Lua coroutine
does not make a synchronous C call nonblocking.

We inspected Kore's task subsystem and reused Knurl's existing bounded executor
instead of adding another native pool. Native work receives retained C state;
Lua conversion and callbacks run on the owning Lua thread. Native thread stacks
use platform defaults, including musl's small stack. Large buffers live on the
heap. Lua callbacks and LoneJSON record operations never run on foreign threads.

The prototype keeps one Lua VM per HTTP worker and runs ordinary route handlers
and before/after hooks as coroutines. Native `knurl_task_call` hands bounded
chunks/callback work back to the owner thread. Cancellation retains userdata and
native state until acknowledgment; a disconnected request is not resumed.
Pool retirement follows all task retirements. There is no pthread cancellation
or owner-loop join. Uncooperative native work remains subject to worker/parent
shutdown escalation.

Handwritten `async.curl` uses that adapter; plain `curl` remains synchronous.
The prototype defaults to 2 threads and 16 admitted tasks per worker, configurable
up to 8/256. Queue time counts toward the deadline. Buffered bodies are capped
at 8 MiB and headers at 64 KiB; streaming callbacks exchange bounded chunks.
The old curl multi/reactor prototype was replaced by the native-task version.
There is no code generator. Downstreams such as Vectis could use the curl adapter
as a reference for their own `async.openssl`, `async.ssh2`, etc.; those additional
wrappers were not implemented in Knurl.

**The unresolved coupling:** `async.curl` requires a yieldable, registered Knurl
HTTP route coroutine. It cannot run during top-level startup, shutdown, arbitrary
Lua coroutines or nonyieldable callbacks. Native auth, proxy rewrites, mapped
codec handlers, producers and lifecycle callbacks do not gain async behavior.
Different requests can execute native operations concurrently; two calls in one
handler await sequentially. Lua spawn/join/fan-out was not implemented.

Considered approaches included arbitrary C-binding wrappers, code generation,
whole-handler offloading with isolated VMs, explicit async modules and a single
context-aware curl facade. A context-aware facade was recommended last but was
**not implemented**. Generic execution of arbitrary Lua closures on background
threads was never delivered. Do not treat the archive as a general Lua scheduler.

Luthrum must define how execution, VM/GC ownership, callbacks, userdata, shared
module state and native handles behave across OS threads. Runtime multiplexing
alone must not be assumed to make every C module or native handle thread-safe.
Reassess this prototype against those actual contracts before restoring it.

## Startup/shutdown and pslog discussion

Vectis's controller was inspected read-only. Its useful lifecycle patterns are:
inert declarations; fork before creating service threads; keep the Kore parent
thread-quiescent for replacement forks; gate serving on readiness; roll back
partial startup; close intake; drain/cancel work; retain resources through native
acknowledgment; apply one overall shutdown deadline; preserve the primary failure
and report cleanup failures and abandonment accurately.

The user explicitly rejected importing a supervisor, logical scopes, service
graph or additional process layer into libknurl. Vectis's serial owner callbacks
and process-local mailbox broker also do not solve general coroutine execution
or cross-process request/reply. Keep application supervision downstream.

An earlier Vectis change request discussed yielding outbound calls. The user
subsequently rejected implementing that change there because it would break
existing behavior. It is historical investigation, not an authorized Vectis
change or a prerequisite for the current C-only product.

The intended logging contract is one root pslog instance created at the top of
the user-provided script and each example. Pass that handle into the server and
other components wherever logging is idiomatic; derived context loggers may
share its sink. Existing Core/Lua server logging retains a borrowed native-sink
logger and supports access-log overrides. Never close a borrowed root during
component teardown. Forks have process-local state; this is not a shared Lua VM
or a new cross-process logging service. Foreign threads must not enter Lua sinks.

The last lifecycle/logger task stopped after inspection. No new lifecycle
controller, first-error repair or logger propagation into curl/tasks was
implemented. Most examples still construct their logger inline; the combined
example already names its root. Carry the shared-root requirement forward.

## Historical evidence and restoration gates

Before archiving, the async worktree passed 150 GNU Core checks, 42 GNU Lua
checks, 41 musl Lua checks, targeted musl C/receiver/header tests, Valgrind task/
continuation/curl checks, formatting and static-musl CLI/HTTP/package plus Podman
scratch HTTPS/system-trust/PID-1 tests. Tests prove two native operations from
different requests start before either is released; gated chunks prove live
streaming while other requests progress. Callback failures/OOM, queue limits,
timeouts, disconnect cancellation and shutdown were exercised. Musl exposed a
duplicate-binding load in the standalone adapter fixture; its installed-module
search path was corrected. These are historical results, not a Luthrum validation
or evidence that the executable is release-ready. Latest packaging/lifecycle
checks target only the active C product.

Restore only after the Luthrum dependency/runtime contract is available, then
specify and prove scheduling isolation for every supported callback context,
startup/shutdown ordering, root logger ownership, true streaming/backpressure,
cancellation and late completion, memory limits, musl default stacks, public
interop and source/SDK packaging. Recheck static linking and both architectures,
system CA behavior, scratch PID 1, example dumps and final artifact privacy.
Use the old sources and tests as evidence inputs, not a parallel legacy runtime
or a promise that the future binding will keep the archived ABI or spellings.
