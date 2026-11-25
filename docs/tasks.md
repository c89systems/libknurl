# Native tasks and request resumption

Kore's HTTP workers each serve many connections on one event loop. Blocking in
an ordinary route callback occupies that worker; forking workers does not create
a process per request. Native tasks let a route wait for its own work while the
owner loop continues serving other connections.

The C API in `<knurl/task.h>` uses the library's bounded executor implementation.
Create a pool with `knurl_task_pool_new` or `server->task_pool` in an active HTTP
worker after its final fork. This starts native threads; constructor calls in the
parent or outside a server run fail with STATE. Zero configuration selects 2 threads and
16 admitted tasks per pool, up to 8 threads and 256 tasks. Each task pool owns
separate threads and admission capacity. A worker permits at most four executor
pools in total, also used by internal native I/O, filesystem and dynamic DNS
services. Pool-slot exhaustion returns LIMIT; invalid bounds return INVALID.
Closing a pool releases its slot only after native work and the executor's loop
registrations retire. Pool retirement follows all task retirement callbacks.

Initialize a `knurl_task_config`, supply `work`, `retired` and retained C userdata,
then call `pool->submit`. Success borrows the context through exactly one owner-
loop retirement callback. AGAIN means capacity or transient queue contention;
rejection takes no ownership. There is no implicit whole-message buffering.
Task threads use the platform's default stack: keep large state on the heap.
Queued cancellation can skip `work`; retirement still runs for admitted tasks.
Work must return a terminal status; AGAIN and EOF become INVALID. A negative
timeout is invalid even when the timeout is disabled.

`work(task, userdata, error)` runs on a native thread with retained userdata.
Keep live requests and transport handles on the owner loop. Poll
`task->cancelled(task)`. `task->call` requests a short owner-loop
callback and waits only on the native thread. Borrowed callback data remains
valid until it returns. Owner-loop callbacks must return promptly; blocking
inside one occupies the worker again. Callbacks cannot yield or unwind a foreign
exception.
An owner callback can cancel its own task; the native waiter remains retained
until that callback returns.

Timeout defaults to 30 seconds including queue time; `timeout_disabled` explicitly
disables it. Timeout/cancellation are cooperative, not forced thread termination.
The context survives until native acknowledgment. Closing the pool rejects new
intake and cancels work asynchronously; pool retirement follows all task
retirement callbacks. Worker shutdown closes pools automatically. Uncooperative
work is subject to the existing parent shutdown deadline and abandonment policy.

An ordinary C route/hook can return KNURL_AGAIN and later use
`knurl_request_resume` / `request->resume` on the owner loop with the terminal
status and hook finish decision. Cancel owned tasks on request close and never
resume a closed request. The staged reply remains borrowed until close. One
request can coordinate multiple C tasks through its own retained context; these
primitives do not impose a Lua coroutine model.

The public [task fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/task.c)
checks native/owner thread placement, bounded admission, queued cancellation,
callback lifetime and pool retirement. The
[continuation fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/route-resume.c)
checks staged before/handler/after continuation while other requests progress.
These tests remain part of the active C suite.

## Blocking C route handlers

Register with `knurl_router_blocking_route` or `router->blocking_route`, using
`knurl_blocking_route_config_init` and the usual method, path and ACL fields.
The group variant applies the group's prefix and ACL. The handler takes a
borrowed `knurl_request_snapshot`, a live input reader and an independent staged
reply. Capture lookup uses `knurl_request_snapshot_path_param` with the names
from the route declaration. Input uses the [native reader contract](uploads.md)
with bounded streaming. A handler can select a staged reply and return before
reading the full body. Kore stops receiving unfinished HTTP input and closes
the connection after sending that reply. Kore publishes staged replies on the
event loop after the native thread returns. Owner-loop hooks still need to
return promptly.

In the absence of lifecycle hooks, the first blocking route request in each
HTTP worker creates its dedicated pool.
Configure it before running the server with `knurl_server_set_blocking_pool` or
`server->set_blocking_pool`. Zero selects two threads, 16 admitted tasks including
queued work, and a cooperative 30-second deadline including queue time. The
maximums are eight threads and 256 tasks per worker. Pool bounds are copied.
Ordinary file and upload I/O use their own executor. The application pool uses
one of the worker's four executor slots.

Optional `on_thread_start` and `on_thread_stop` run on each pool thread,
before readiness and after the thread's last job, respectively. Hooks on
different pool threads can run concurrently. Start failure prevents readiness.
Stop hooks run for successfully initialized threads and report errors through
worker teardown. This supports thread-affine resources and runtimes owned by
the handler thread. C process hooks retain their owner-loop contract.

Use `knurl_server_submit_application_task` to submit to the same pool from
the owner loop. A zero task timeout inherits the pool deadline. This lets an
embedder share its thread-owned runtime across HTTP handlers and detached
callbacks. `knurl_request_snapshot_copy` provides owned request metadata for
such callbacks, released with `knurl_request_snapshot_destroy`.

`knurl_route_set_async_hooks` and the group equivalent support owner-loop
admission and response continuations. The hook installs a cancellation
callback with `knurl_request_hook_wait` and returns KNURL_AGAIN.
On native acknowledgment, finish the wait using `knurl_request_hook_resume`
on the owner loop. Cancelling the wait expires its request pointer and
detached work retains its own context through acknowledgment. A suspended
after-hook holds final response headers and payload output until completion.

When the pool cannot admit a task, the route returns HTTP 503. The pool deadline
governs the admitted job and Kore sends 504 on expiry while cancellation
proceeds. The HTTP body and write deadlines apply independently. A disconnected
or timed-out request retains its task slot and copied data until the native
thread returns. Long-running handlers can check
`knurl_source_cancelled` on the borrowed input. An uncooperative thread remains
subject to the existing worker shutdown deadline.
