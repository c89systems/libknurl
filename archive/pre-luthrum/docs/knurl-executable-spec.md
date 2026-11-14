# Knurl executable

Archived pre-Luthrum reference; this product is deferred. See the [handoff](../README.md).

## Product and deployment

Build a fully static `knurl` executable for `x86_64-linux-musl` and
`aarch64-linux-musl` using the pinned cpkt runtime and dependency SDKs. Embed the
Lua runtime and Knurl, LoneJSON, pslog and curl modules. Require no shared objects,
external interpreter or installed Lua module tree. Keep the existing Core SDK
and Lua source products independent. Keep the unreleased Core ABI at zero.

Use the checksum-pinned released libpid0 SDK to wrap the executable entry point.
Outside PID 1 it runs directly; as PID 1 it forwards signals, reaps adopted
children and propagates the managed child's exit status. Knurl retains ownership
of HTTP workers, readiness and graceful shutdown. Run foreground without
daemonization or automatic configuration reload in this first implementation.

`knurl server.lua` loads a text Lua script and requires one configured Knurl
server result. The executable validates, runs and closes that server, with
actionable diagnostics and nonzero failure status. Provide `--help`, `--version`,
`--check FILE` and `--dump-examples [DIRECTORY]`; default dump destination is the
current directory. Never overwrite an existing example or follow its symlink.
Lua remains a programmable trusted application, not a sandbox.

## Configuration and defaults

Retain the public Lua routes and configuration: HTTP/HTTPS listeners, static
roots/ranges/streaming, reverse proxy, WebSocket/SSE, JSON/XML, WebDAV, IP/DNS and
MMDB country ACLs, logging and TLS/ACME. Provide usable file-backed ACME state
through an explicitly configured writable directory, retaining custom state
callbacks. Missing directories/material/databases must fail with useful errors.

The executable uses the production profile unless explicitly overridden by the
application. All protective settings remain configurable through C and Lua.
The production profile suppresses diagnostic text in automatic 5xx responses;
explicit application response bodies remain under application control.
Preserve finite header, body-idle, output-idle, keep-alive, minimum-body-rate and
shutdown controls. Add an independent absolute TLS handshake deadline. Explicit
deadline disable remains distinct from zero-duration inheritance. Account for
long-lived output explicitly rather than applying an accidental total stream
lifetime. Document effective settings and test failure paths.

Do not embed a CA bundle. Honor explicit trust configuration and discover system
CA bundles consistently for outbound curl, reverse proxy and ACME. Inspect
Vectis's existing CA behavior while adapting it. Validate TLS peers and hostnames
by default; do not silently disable verification when trust is missing. The
scratch test Containerfile copies a CA bundle from a pinned Alpine image into
the conventional system path. Operators may replace or mount their own bundle.

## Outbound curl and route continuation

Adapt the Vectis curl Lua surface and LoneJSON integration without importing its
application/supervisor machinery or making another checkout a build input.
Preserve provenance/licenses. Sequential-looking calls such as
`local response = curl.perform{url=...}` wait from the route's perspective while
the runtime suspends that coroutine and continues other connections in the same
HTTP worker. Resume on the owning Lua thread. No process/thread per request,
busy waits, blocking curl polling or sleep-based retry delays on that loop.

Expose generic request suspension/resumption through public Core operations so
the Lua facade uses public interfaces. Preserve route/hook ordering, staged
response semantics and exactly-once cleanup across success, failure, disconnect,
timeout, shutdown and allocation failure. Bound retained registrations, transfers
and bytes. Reject unsupported yield contexts explicitly. Standalone startup
transfers may execute synchronously outside a running HTTP route.

Keep buffered operations explicitly bounded. Curl/LoneJSON streaming operations
must feed bounded chunks directly between producer and consumer, with terminal
error propagation and no hidden whole-body materialization or staging files.

## Dumped examples

Dump four self-contained Lua server scripts named `knurl-example-*.lua`:

- Static content with loopback/private IP ACL examples.
- Reverse proxy only, with explicit upstream configuration and deadlines.
- WebDAV over filesystem content, protected by an obviously illustrative example
  key that the operator must replace.
- Combined static and proxy routes with differing IP and country ACLs. Include
  the allowlist `SE, NO, FI, GB, DE, US, CL`, explicit denies `KP, VE` and unknown
  handling. Use ISO country codes: `GB` is the United Kingdom; `KP` is North Korea.

The country example loads a configured MMDB path and logs a missing-database
error using pslog. Explain the DB-IP country-lite download page, gzip extraction,
placement and configuration, and applicable attribution. Do not redistribute or
silently download the database. Include runtime examples of programmable routes,
outbound curl/LoneJSON and native pslog configuration in the documentation.

## Distribution

Produce deterministic, separately checksummed
`knurl-<version>-<target>.tar.gz` archives with a matching root containing
`bin/knurl`, `share/doc/knurl/` and `share/man/man1/knurl.1`. Ship the relevant
README, examples, dependency versions and license notices. Verify the extracted
final bytes, executable mode, exact version, relocation and clean metadata.
Prove ELF architecture, absence of an interpreter and absence of dynamic library
requirements; successful linking alone does not prove static deployment.

## Verification

Use existing serial CMake/CTest lifecycle tooling with `JOBS=1`. Add observable
unit and integration tests for CLI argument/error behavior, returned-server
lifecycle, dump contents/no-overwrite, normal/failing requests, configuration,
examples, trust discovery/override and packaging. Preserve existing Core/Lua
tests and ABI/public boundary checks.

With one worker and a local upstream held behind an acknowledged gate, require
a fast second request to finish before releasing an outbound curl request. Prove
same-worker identity, resumed result isolation, bounded streaming and cancellation
including stale completions, disconnects, shutdown and allocation failures.

Use rootless Podman for real scratch PID 1 tests: static runtime/modules, HTTP,
HTTPS/system trust against a controlled fixture, signal forwarding, child
reaping, graceful/forced stop and exit propagation. Copy the CA bundle within the
Containerfile, using pinned images. Supply only declared writable state and keep
test resources under owned build paths. Verify AArch64 execution with the
existing QEMU capability; distinguish it from native execution. Finish with
relevant memory checks and final tarball consumer tests. Do not publish or push
as part of implementation.
