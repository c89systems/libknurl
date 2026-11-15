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

`work(task, userdata, error)` executes without Lua or live request/transport
access. Poll `task->cancelled(task)`. `task->call` requests a short owner-loop
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
