# C product and deferred Luthrum integration

libknurl currently ships as a C89 library and C SDK, with C++98-compatible public
headers. Its C convenience wrappers around Kore remain active: routing, mapped
JSON/XML, REST/OpenAPI, streaming, static assets/ranges, WebDAV, proxies, ACL/MMDB,
TLS/ACME, logging and lifecycle. Native tasks and request resumption are public
C primitives; see [tasks](tasks.md). Shared-library ABI remains 0 for 0.2.0;
see [SDK delivery](sdk.md) for consumer build requirements.

The optional [Lua configuration module](lua-config.md) registers C routes and
ACLs using Lua 5.5.1 from the existing cpkt SDK. Request handling stays in C.
Mixed deployments can proxy application paths to a separately run Lua process.
The module has a separate opt-in build, test and install component. Release
checksums cover the seven C SDK archives.

The standalone `knurl` executable and Lua request callback binding are deferred
until **Luthrum**, C89 Systems' Lua runtime in development with coroutine
multiplexing onto OS threads, is ready.

The repository's [pre-Luthrum archive](https://github.com/c89systems/libknurl/tree/trunk/archive/pre-luthrum)
preserves the executable, bindings, examples, tests, package machinery and a
detailed handoff. That handoff distinguishes delivered prototypes from proposals,
including the HTTP-coroutine coupling in `async.curl`, missing same-handler
fan-out, the startup/shutdown investigation and the shared parent pslog requirement.
Archived sources are not a supported alternate build path.

The future runtime integration must establish VM, thread, coroutine, callback,
GC and native-handle ownership before restoring bindings. A scheduler must not
be presumed to make arbitrary C libraries thread-safe. Application supervision
stays with the embedding host; libknurl does not add a supervisor framework.

The C library retains its current [process lifecycle](processes.md), native
failure/cancellation contracts and [borrowed pslog registration](logging.md).
Future script examples should create one root logger and pass it into components,
retaining ownership until all work has retired.
