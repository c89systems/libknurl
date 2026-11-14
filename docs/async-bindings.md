# Native async bindings

Knurl runs one Lua VM on each HTTP worker's owner thread. Route coroutines may
suspend while other connections and timers continue on that worker. Lua itself
and Kore's event model are unchanged.

`require('async.curl')` exposes `perform`, `json` and `stream_json`. Calls wait
from the route's perspective, but native libcurl work runs in a bounded thread
pool. `require('curl')` remains synchronous. Arbitrary blocking Lua code and
unmodified synchronous modules do not become asynchronous automatically.

## Adapter boundary

The public Core task API in `knurl/task.h` reuses the existing executor. Pools
are created only in an active HTTP worker, after the final fork. Admission is
bounded; full queues reject work without taking ownership. Cancellation is
cooperative. A task and its context remain alive until native work acknowledges
completion, including after timeout, disconnect or shutdown. No thread is killed
and no owner-loop callback waits for a thread to join.

The Lua extension API in `knurl/lua.h` separates three phases:

1. Prepare and validate on the Lua owner thread. Put native state in an owned
   userdata, with a GC destructor, before submitting it.
2. Execute native code on a task thread. Never access Lua, a request, or its live
   transport here. Poll cancellation. `knurl_task_call` can request a synchronous
   owner-thread callback; only the task thread waits for its result.
3. Convert the result on the Lua owner thread after native acknowledgment.

`knurl_lua_async_run` roots the supplied owner until acknowledgment, copies the
operation descriptor and yields the current route. It requires an active,
yieldable Knurl route coroutine. It is not a generic wrapper for arbitrary Lua
closures. A different host, such as Vectis, needs its own request scheduling
integration; it can reuse the native task contract and copy this adapter pattern.

Different requests can execute native work concurrently up to the thread limit.
Calls in one handler await sequentially. This API does not provide Lua
`spawn`/`join` or fan-out within a handler. The C pool API accepts multiple tasks
independently.

Owner callbacks must contain Lua errors with `lua_pcall` or equivalent protected
execution. Never unwind a Lua error through a Core task callback. An owner
callback may cancel its task; native execution waits for that callback to return
before releasing the borrowed chunk.

The worker's shared Lua task pool defaults to two native threads and sixteen
admitted tasks, including queued and cancelling tasks. Configuration is explicit
and precedes first use. Buffers belong on the heap; native threads use platform
default stacks, including musl's small default stack.

Configure this with `knurl.async_configure{threads=2,max_tasks=16}` before first
use. Zero selects defaults. Curl's `timeout_ms` also bounds queue residence;
zero explicitly disables its task and transfer deadlines. Other adapters set
their own deadline in `knurl_lua_async_operation` (zero selects 30 seconds).

## Curl reference

`src/lua/curl.c` is the handwritten reference adapter. Each transfer owns its
easy handle exclusively. Lua read/write/progress callbacks and LoneJSON record
operations execute on the owner thread through the task callback bridge. These
callbacks must return promptly and cannot yield. Native file and network I/O
run on the task thread. Read/write callbacks exchange bounded chunks; they do
not collect the complete message. Buffered `perform` responses have an 8 MiB
body limit and a 64 KiB header limit. `json` is buffered; `stream_json` uses
incremental LoneJSON serialization and parsing.

Downstreams should implement their own `async.openssl`, `async.ssh2`, or other
wrappers against their libraries' ownership and thread-safety contracts. Knurl
ships only curl as the reference. There is no code generator and no implicit
offloading of arbitrary existing bindings.

Verification covers same-worker connection progress, bounded admission,
streaming callbacks, JSON mapping, failures, allocation failure, cancellation,
shutdown and a separately compiled adapter using only installed public APIs.
