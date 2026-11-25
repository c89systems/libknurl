# C SDK and Lua application runtime

libknurl currently ships as a C89 library and C SDK, with C++98-compatible public
headers. Its C convenience wrappers around Kore remain active: routing, mapped
JSON/XML, REST/OpenAPI, streaming, static assets/ranges, WebDAV, proxies, ACL/MMDB,
TLS/ACME, logging and lifecycle. Native tasks and request resumption are public
C primitives; see [tasks](tasks.md). Shared-library ABI remains 0 for 0.2.0;
see [SDK delivery](sdk.md) for consumer build requirements.

The [loadable Lua module and `knurl` runner](lua-config.md) use Lua 5.5.1
from the existing cpkt SDK. Both use the same Lua binding, which calls the
public C API. The runner ships in its own tarballs with bundled dependencies.
The C SDK packages declare their dependencies through CMake and pkg-config.

Native routes serve entirely in C. Inline Lua routes and worker hooks use one
dedicated thread per Kore worker, with closures inherited from the paused
parent at fork. Parallel execution within one application VM is a future
Luthrum integration. Luthrum is C89 Systems' Lua runtime in development with
coroutine multiplexing onto OS threads.

The repository's [pre-Luthrum archive](https://github.com/c89systems/libknurl/tree/trunk/archive/pre-luthrum)
preserves the executable, bindings, examples, tests, package machinery and a
detailed handoff. That handoff distinguishes delivered prototypes from proposals,
including the HTTP-coroutine coupling in `async.curl`, missing same-handler
fan-out, the startup/shutdown investigation and the shared parent pslog requirement.
Archived sources are not a supported alternate build path.

Future runtime integration must preserve VM, thread, coroutine, callback, GC
and native-handle ownership. Application supervision stays with the embedding
host.

The C library retains its current [process lifecycle](processes.md), native
failure/cancellation contracts and [borrowed pslog registration](logging.md).
Future script examples should create one root logger and pass it into components,
retaining ownership until all work has retired.
