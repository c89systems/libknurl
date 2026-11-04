# Build availability and backend maintenance

The [README](../README.md) describes the server library's product contract.
The build produces a C89 static/shared library with wide counters, errors, SSE
framing, owned sources and staged reply builders. Reply builders copy text/bytes
and ordered headers, pin regular files, and distinguish live source ownership
from explicitly bounded synchronous materialization. Source registration never
reads or resets a producer. It also compiles the selected private Kore backend
objects under strict C89, applying the 42 original patches, an external C89 port
and the private library lifecycle patch. The selected backend excludes the
standalone CLI configuration parser/assets and uses typed callbacks. Queue
declarations use named heads and C89 layout macros, retaining platform queue
operations without Clang's empty-macro-argument extension. Its native
fixture serves HTTP, handles worker-initiated stop, repeats a run, and recovers
from an injected error after bind. Inert server construction copies listeners,
TLS material, callbacks, trusted-proxy declarations and autoblock rules, and
resolves effective settings.
The router owns stable route/group handles, validates declarations and preserves
registration order, prefixes and exact method/path conflicts. Construction and
registration execute no user callbacks or network operations.

Both library variants now include the private backend. The initial public runtime
binds cleartext listeners, reports ephemeral addresses, forks HTTP workers and
runs on the calling parent thread. It supports process hooks, a shared serving
gate, owner-loop route dispatch, copied byte replies, bounded in-memory bodies,
thread-safe stop notifications and worker-to-parent stop requests. Request views
retain the raw target, header order including Host, decoded ordered query pairs,
parameters and socket endpoints. Parameter matching keeps optional omission
ordering without recursive C calls. Runtime reply replacement queues superseded
sources for cleanup rather than invoking close inside a setter.

The [response API](responses.md) exposes a bounded queue, explicit known lengths,
chunk framing, trailers, commitment, writable/terminal callbacks, pinned file
ranges and nonblocking source producers. Staged file and live-source replies
use this transport. Requests and their metadata remain alive through source
close and terminal notification; completed buffers retire after native queue
callbacks return. Source AGAIN pauses until explicit owner-loop resume. A quiet
producer with no queued output does not trip the downstream write-idle guard.
Large staged byte payloads move into bounded delivery without a second whole
copy. File delivery uses bounded owner-loop pread and can block on filesystem I/O.
`rotmos.response-runtime` covers these paths, including a sparse range beyond
4 GiB, queued replacement, producer errors, bodyless replies and HTTP pipelining.

[Request plans and bodies](requests.md) support explicit CALLBACK dispatch and
raw router routes alongside the ordinary staged facade. All enter the prebody
boundary before storage, application body callbacks or 100 Continue. Fixed-length
STREAM bodies use bounded chunks with partial consumption, pause/resume and
owner-loop callbacks. BUFFERED stays within its declared memory budget;
MATERIALIZED can spill only into the configured transport spool directory.
Completed bodies expose memory/file readers and independent copy/spill results,
including public source readers. Request terminal callbacks retain metadata and
distinguish successful input completion, cancelled unread input and failure.
`rotmos.body-runtime` exercises partial upload/response progress, concurrent
requests during a pause, deferred replies, deadlines, file/result ownership and
callback failure. Immediate and ordinary replies preserve pipelined surplus.

[HTTP framing and proxy core](proxy-core.md) provide a public incremental
fixed/chunked body decoder and Vectis's lifted generic proxy validators,
upload/response parsing and policy machinery. Private curl pools use public
loop watches/timers, with a live bounded transfer fixture. The
[public proxy facade](proxy-http.md) registers copied routes and executes bounded
HTTP/SSE duplex exchanges with interim responses, trailers, progress/total
deadlines and shared worker admission. Its live fixture also runs against the
relocated static and shared SDKs.
[WebSocket/WSS relays](proxy-websocket.md) validate the upstream opening, preserve
early and opaque fragmented/extension bytes, stream non-101 rejections, and share
worker admission with HTTP. Plain/TLS fixtures cover duplex flow, upstream mTLS,
bounded rejection output, half-close, deadlines and failure after commitment.

[Connection channels](channels.md) add exclusive headers-time takeover, bounded
raw HTTP input, explicit tail return and restoration, and validated WebSocket
tunnel upgrades. Tunnel output uses bounded copied prefixes, preserves earlier
queued streams, and drains before orderly write-half shutdown. Read EOF remains
separate from output completion. Logical readiness and terminal notifications
run on the owner loop; channel close precedes request close. The public and
private queue fixtures cover raw chunked input, early frames, queue pressure,
half-close, idle timeout and pipeline tails larger than the native header buffer.
The TLS fixture exercises the same channel over a public HTTPS listener,
including early bytes, bounded output and orderly TLS half-close. Forced TLS
retry-direction faults and slow/disconnected peers remain final integration
coverage.

[LoneJSON codecs and typed routes](json.md) parse completed memory/file bodies
directly into public record storage and produce bounded JSON through the public
pull generator. Per-request record owners survive output and staged replacement.
Selected-array input uses incremental item callbacks. [XML DOM/SAX](xml.md) expose
owned, confined document/views and incremental context-local parsing without
libxml2 types in public headers. Their API and wire fixtures also run as relocated
static/shared consumers. Mapped XML and resumable output remain the next surface.

[Application WebSockets](websockets.md) expose transactional acceptance, a copied
route facade, bounded complete frames and atomic output, pause/resume and
capacity callbacks. They use the channel transport with an independent lifetime:
the HTTP request closes before open, and each accepted WebSocket owns one close
notification. Plain/TLS fixtures cover framing, validation, ownership and drain.
Continuation frames/extensions are unsupported on this application path; proxy
tunnels remain opaque. Connection reuse clears native event readiness through
the external patch series.

[TLS listeners](tls.md) support manual identities from bounded files, copied
memory and synchronous source callbacks, including bundles, extra server chains
and independent client-auth trust. Preparation checks key pairs and protocol/
cipher policy before forks and installs in-memory contexts without temporary key
files. HTTPS domains, SNI, required/optional mTLS and checked 308 redirects use
the configured listener identity. ACME identities use a private key manager,
acknowledged state generations and cached TLS policy on context updates.

Parent and HTTP-worker loops expose borrowed FD watches, monotonic wide timers,
deferred callbacks and coalescing wakers. Readiness is dispatched after native
event batches; cancellation retires userdata after outstanding batch references.
Worker draining retains these registrations until cancellation/completion. The
owned supervision descriptor runs on the parent loop, keeps host framing, drains
readable bytes before EOF notification, and is consumed when the run ends.
`rotmos.loop-smoke` covers the basic loop operations, source FD ownership,
coalescing, control EOF and rejection of a supervised rerun without a fresh FD.

`rotmos.server-runtime` exercises this public surface over TCP, including staged
middleware replacement, framing rejection, parent/worker hooks, signal restoration,
startup cancellation and repeated runs. The installed-consumer check repeats the
HTTP fixture through both static and shared imported targets after relocating
the library and its three released dependency prefixes.

This is partial runtime coverage. Preflight explicitly rejects
access logs, trusted proxies, autoblock,
typed JSON and native-reader upload facades until those paths are connected.
Native-reader sources require an executor that is not connected yet. Interim
responses, producer trailer declarations, application WebSockets, HTTP framing helpers and gate observations,
complete fatal/allocation-failure unwinding, the protocol and
provider helpers, the opt-in native declaration component, and the Lua product
remain implementation work. [Process observations](processes.md) cover child
start/exit/restart/escalation, returned hook errors, abandonment and bounded
failure/recovery results. Native key-manager/ACME loops use the same loop and
teardown hooks; their fixture proves auxiliary restart/terminate policy. A
dedicated child-operation budget wakes the parent and remains enforced during
shutdown. ACME preparation/persistence/publication uses that budget.
The [ACME generation store](acme-state.md) stages complete bounded
snapshots, runs restore/persist/error callbacks with real process context, and
publishes acknowledged certificate/key pairs through one atomic selection.
Its focused fork fixture covers interruption on both sides of acknowledgement
and preservation of active state on failure. The runtime fixture proves private
key-manager signing, actual TLS activation, persistence failure/restart and hard
timeout/recovery across two workers and repeated listeners. Public pending/usable
observations track HTTP worker contexts; challenges remain pending and work
while application serving is paused. The [ACME protocol fixture](acme-protocol.md)
consumes the shared public API and drives a local HTTPS CA through account
recovery, JWS/CSR, challenge verification, issuance, restart and renewal selection,
with captured trust and bounded transport cases. The contract is in the
[server API specification](server-api-spec.md) and
[Vectis integration contract](vectis-integration-contract.md).

## Commands

Prerequisites: CMake >= 3.24, Ninja, Git, Make, Bash >= 4.4, curl, tar, flock,
SHA-256 tools and clang-format. Linux compilation uses the lifecycle's pinned
Bootlin GCC/binutils/sysroot collection, with no host compiler fallback.

```sh
make help
make toolchain             # Explicit provisioning; unnecessary when already prepared.
make build                 # Compile the selected x86_64-linux-gnu target in build/debug.
make build-all             # Compile the full seven-target matrix, serially.
make build-all CONFIG=release
make test                  # Run checks for the selected toolchain target.
make install               # Install to owned build/install/<target>/debug by default.
make format-check          # Check project sources; excludes pristine/patched Kore.
make prepare-kore          # Source preparation only; no compiler or SDK acquisition.
make clean                 # Remove all local build/, dist/ and .cache/ state.
```

`make build` reuses checksum-verified local archives or acquires missing released
SDKs from c89systems. Once prepared, builds and tests are offline. SDKs have
independent prefixes under `.cache/sdk/<target>/`; archives stay in this
checkout's `.cache/`. Existing global cpkt archives may be read and copied.
Toolchain discovery is read-only; only explicit provisioning prepares compilers.
One operation owns the checkout; run operations serially.

Every build selects a concrete architecture/OS and the lifecycle toolchain for
that target. The default is `x86_64-linux-gnu`, with `build/debug` and
`build/release` directories. Other targets use `build/<target>/debug` or
`build/<target>/release`. The editor database is
`build/debug/compile_commands.json`, matching `.clangd`; the ordinary build
prepares its actual dependencies and generated headers. There is no host-based
product selection.

The full matrix is GNU/musl Linux for x86_64, aarch64 and armhf, plus arm64 Apple
Darwin. `make build TARGET=<target> CONFIG=release` selects one concrete build;
`make build-all` selects all seven. Matching CMake presets use
`<target>-debug` / `<target>-release`, with `debug` / `release` aliases for the
default selected target. Prepare SDKs with `make deps TARGET=<target>` before
direct preset configuration. Linux uses pinned Bootlin collections; Darwin uses
the selected Apple tools on macOS or approved osxcross on Linux. Discovery never
acquires a compiler or falls back to a host compiler. Runtime checks execute only
when the selected target matches the execution host; other configurations supply
compile/link evidence. Full runtime and artifact validation follows the larger
implementation surfaces. The [implementation constraints](implementation-plan.md)
record this scope and cadence.

## Source ownership

- `vendor/kore/` is an unmodified upstream source export, including its license.
- `vendor/kore.revision` and `vendor/kore.url` identify its exact upstream commit
  and repository. There is no nested Git checkout or submodule.
- `patches/kore/series` orders patches stored outside the source snapshot.
- `build/debug/kore/` is a disposable patched copy; source-only preparation
  uses `build/source/kore/`. Preserve changes by editing
  or adding a patch, updating `series`, then rebuilding.

CMake tracks upstream sources, patches, `series`, provenance and the preparation
script. A changed input regenerates the copy from pristine sources; an unchanged
build reuses it. Patch failures name the patch and stop the build without
publishing a partially patched copy.

Backend objects depend directly on successful source preparation. A patch edit
must prepare and recompile its changed source in one build invocation; CTest's
`kore.backend-invalidation` proves that behavior with a compiled fixture.

## Upgrading Kore

Start with committed, pristine `vendor/kore/` and provenance files:

```sh
make update-kore KORE_REVISION=<full-40-character-upstream-commit>
make test
git diff -- vendor/kore vendor/kore.revision patches/kore
```

When staging an accepted snapshot, use `git add -f -- vendor/kore` so Kore's own
unmodified `.gitignore` does not hide newly introduced public headers. That
directory must contain only the pristine export; generated files belong in
`build/`.

The updater fetches the recorded upstream into owned `build/` scratch, exports
the exact requested commit and tests the current patch stack against that
candidate. Only a successful replay replaces the tracked pristine snapshot and
its revision. A conflict leaves both unchanged and reports the failing patch.
Rebase or repair the patch files against the candidate revision, then repeat the
update. Patch application alone does not prove runtime compatibility: C89,
behavior and installed-consumer checks also apply once the library is built.
The upgrade command does not commit, tag or push changes.

The initial source pin is `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b`. The
original patches were imported from Vectis commit
`6b1e263e0436687f68a7768ca55f4361a6e59e19`; that provenance is recorded
separately in `patches/kore/vectis.revision`. Fixes belong in this repository's
external patch stack.

The [ownership boundary](extraction.md) and [specification audit](server-api-audit.md)
provide implementation context and evidence limits. Imported Kore sources retain
their notices in [vendor/kore/LICENSE](../vendor/kore/LICENSE).
