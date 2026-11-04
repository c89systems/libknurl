# Storage operations and native jobs

[`<rotmos/storage.h>`](../include/rotmos/storage.h) defines provider operations,
resource handles and acknowledged cancellation without Vectis, Lua, filesystem or
backend layouts. [`<rotmos/executor.h>`](../include/rotmos/executor.h) runs bounded
native work after an HTTP worker's final fork. Both are ordinary umbrella headers.

A provider declaration copies its callbacks, capability bits and commit description
without invoking work. Its first accepted operation binds it to one process,
thread and active HTTP worker loop/run. Operations and executor creation are
rejected in parent and pre-activation process-start hooks. Configuration and resources cannot migrate to another run.
Provider close revokes new operations; its close callback waits for operations,
resources and transferred read assets to retire. Resource close remains usable
on its owning thread after the run without dereferencing a released loop.

The operation vocabulary covers stat, open-read, list-open/next, PUT begin/write/
finish/abort, delete, collection creation, copy and move. Capabilities explicitly
permit writes, conditional mutations, copy/move, cross-resource atomicity and
durable commit. Zero capability bits select read-only. Unsupported writes,
conditions or requested cross-resource atomicity fail before invoking a provider.
This is a declaration contract, not an unlocked stat-then-write approximation.
Providers preserve their own commit/durability point in the copied description.
The result reports domain codes separately from interruption/protocol causes,
including precondition failure, tombstones, quotas and explicit listing END.

`rotmos_storage_operation_start` accepts and returns one operation, then delivers
consumer callbacks on a later loop turn. Failure before provider invocation takes
no retirement notification. Once accepted, even synchronous provider/protocol
failure receives one terminal retirement. All paths, conditions and bounded byte
chunks are copied. Paths use the confined normalized asset path rules. Chunks and
conditions are at most 65536 bytes; paths/recovery strings are bounded to 4096.
There are at most 256 active operations per loop and 256 resources per provider.
Only PUT_WRITE carries a nonempty chunk. A successful write acknowledges a positive
prefix at most the offered length; the next call contains only unaccepted bytes.
A resource permits one pending operation at a time and remains retained if its
caller closes the resource owner during that operation.

Provider `invoke` returns OK synchronously or AGAIN with responsibility for later
acknowledgement. Complete a result through `rotmos_storage_operation_complete`,
including from inside a synchronous invoke/cancel callback. Metadata is copied;
asset/resource ownership transfers only when complete returns OK. A failed
completion takes nothing and can be retried. Completion occurs once and promises
that native jobs no longer use the operation or its immutable argument storage.
An empty synchronous OK is sufficient only for operations whose successful
result needs no additional fields. Stat/open/list, write prefixes and mutations
validate their required metadata/resource/commit acknowledgement.

`completed` borrows result metadata through its call. Use
`rotmos_storage_operation_take_asset` or `take_resource` there for independent
ownership. Returned assets and resources retain their storage provider through
close. Untaken results close automatically. Provider retirement precedes consumer
retirement; metadata remains valid through both. Operation userdata is available
to the provider's retirement callback, which releases its operation context.
Caller userdata remains retained through consumer retirement. Neither callback
may longjmp or retain the borrowed operation afterward.

Explicit cancellation immediately revokes consumer completion/interruption
callbacks. Timeout or worker stopping may instead deliver `interrupted` once so
an attached HTTP adapter can abort or reject its still-live exchange. Explicit
cancellation during request close suppresses an already queued interruption.
The provider cancel hook returns OK to acknowledge immediately or AGAIN to
complete later. An error from cancel does not acknowledge work: arguments,
resource owners and operation context remain pinned until natural completion or
process escalation. A result racing cancellation retires without another HTTP
callback. Retired outcomes retain actual committed/partial flags and recovery
paths, including a mutation committed before an interrupted delivery; cancellation
does not imply rollback.

`rotmos_storage_operation_config_init` selects a 30000 ms deadline including queue
and lock wait. Zero selects that default; `timeout_disabled` explicitly removes
it. Disabling it does not remove bounded worker shutdown. A pending operation's
waker/owner state keeps the worker drain alive until acknowledgement. Native
threads publish via an owner-loop job callback or public waker, never directly
through operation_complete. Immutable operation arguments and cancellation checks
may be used by independently retained native work; no live request handle is
part of that context.

`rotmos_executor_new` explicitly starts detached default-stack threads in an
active post-fork HTTP worker. Parent creation is rejected. Default bounds are two
threads, 32 accepted jobs and 64 KiB copied arguments/results. Bounds may select
up to eight threads, 256 jobs and one MiB per argument/result, with at most four
pools in a worker. No thread or dependency runtime is created by server/provider
construction. The queue uses nonblocking owner admission and worker-side waiting;
HTTP callbacks never join a worker or wait for a native operation's lock.

A native work callback receives a copied argument span and retained application
context. It cannot access Lua or live transport handles. It returns a terminal
work status and can publish a bounded result with `rotmos_job_result_copy`.
Completed/interrupted/retired callbacks run only on the owner loop. Cancelled
queued work can retire without running; running work retains its context until
acknowledgement. Actual work status/results survive cancellation, so a caller
can report a commit that occurred before cancellation was observed. Jobs use a
30000 ms deadline by default and an explicit disabled flag. Queue-full AGAIN
from executor_submit takes nothing: a provider must retry under its own bounded
operation context or report a terminal capacity result, rather than confuse it
with the provider's accepted-pending AGAIN contract.

Pool close revokes intake and cancels work. Its terminal callback waits for all
job callbacks and worker acknowledgements. Worker stopping closes every pool and
keeps completion delivery available during drain. Acknowledgement means the
native callback has returned and the thread no longer accesses job/library
contexts; detached thread epilogues require no owner-loop join. A job that never
acknowledges is terminated by the parent's existing shutdown deadline and is
reported as abandoned, without a destructor-completion claim.

`rotmos_storage_provider_assets` adapts a borrowed read-only asset provider to
stat/open/list operations. Opened-object metadata, file sources and bounded
listing iteration keep their existing public ownership. A bounded iterator AGAIN
retries on the owner loop with the same retained resource; it does not collect a
directory tree. The caller retains the asset provider through storage-provider
ownership. Mutable filesystem transactions, the WebDAV HTTP facade and the live
native-reader body/source adapters remain subsequent surfaces of the full goal.

The [public fixture](../tests/storage-runtime.c) drives the real server and
post-fork executor: copied arguments/chunks, native versus owner thread callbacks,
queue admission, queued cancellation, synchronous/deferred results, duplicate and
malformed completion, provider capability rejection, partial writes, provider and
resource retention, iterator AGAIN, metadata/result transfer, deadlines and
cancellation before/after a reported commit. Independent HTTP requests complete
while native work is held. A separate short abandonment case verifies parent
escalation for an uncooperative thread. The ordinary fixture also runs against
relocated static/shared SDKs and on musl default worker-thread stacks.
