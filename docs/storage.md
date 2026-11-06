# Storage operations and filesystem execution

[`<rotmos/storage.h>`](../include/rotmos/storage.h) defines provider operations,
resource handles and acknowledged cancellation without Vectis, Lua, filesystem or
backend layouts. The native worker executor is private supporting machinery in
[executor.h](../src/executor.h); no job/pool SDK is installed or exported.
Storage providers own their execution policy and report completion through the
public storage operations. Built-in filesystem and upload/source helpers use the
private post-fork executor.

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
There is no independent fixed 256-operation/resource rejection threshold.
HTTP admission follows configured server capacity; native execution retains its
bounded executor queue and backpressure. Callers own their operation/resource lifetimes.
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
acknowledgement checks the absolute deadline even if an event batch has delayed
timer delivery. A late result retires as TIMEOUT, retaining its actual committed
state; an already finished provider is not cancelled again. Acknowledgement
before the deadline is independent of when the queued consumer callback runs.
The [deadline fixture](../tests/storage-deadline.c) expires inside synchronous
provider invocation and covers committed and empty late acknowledgements plus
an explicitly disabled bound. A pending operation's
waker/owner state keeps the worker drain alive until acknowledgement. Native
threads publish via an owner-loop job callback or public waker, never directly
through operation_complete. Immutable operation arguments and cancellation checks
may be used by independently retained native work; no live request handle is
part of that context.

The private executor explicitly starts detached default-stack threads in an
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
ownership. The filesystem provider below supplies mutable transactions. The
[WebDAV HTTP facade](webdav.md) connects these providers to mounts.
[Native output sources](responses.md) and [upload stream callbacks](uploads.md)
use the same public runtime. Native upload-reader/file helpers use the shared I/O
executor; their direct file artifacts retain Vectis's partial-output semantics.

The [internal fixture](../tests/storage-runtime.c) drives the real server and
post-fork executor: copied arguments/chunks, native versus owner thread callbacks,
queue admission, queued cancellation, synchronous/deferred results, duplicate and
malformed completion, provider capability rejection, partial writes, provider and
resource retention, iterator AGAIN, metadata/result transfer, deadlines and
cancellation before/after a reported commit. Independent HTTP requests complete
while native work is held. A separate short abandonment case verifies parent
escalation for an uncooperative thread. This fixture runs against the internal
static library, including musl default worker-thread stacks. Separate installed
static/shared filesystem and WebDAV consumers verify their public operations.

## Filesystem provider

`rotmos_storage_filesystem_new(server, config, out, error)` copies an inert
filesystem declaration. Providers in one HTTP worker share a bounded pool of two
native threads and 32 accepted jobs. The first operation starts that pool after
the final fork; worker stopping closes it. Full/busy queues retry on an owner-loop
timer under the operation's existing bound and deadline; cancellation can retire
an unsubmitted operation without starting native work. Declaring multiple mounts
does not multiply threads or consume one pool per provider. The server outlives
its providers. The [public filesystem fixture](../tests/storage-filesystem.c)
exercises more than 40 provider declarations in one worker, exceeding the shared
job queue to prove bounded admission retries and cancellation before submission.

The configuration preserves Vectis's `cache_dir`, `site_id`, optional `root_dir`,
`max_file_bytes`, `max_total_bytes` and `max_resources`. Paths are absolute;
site names have at most 63 letters/digits/hyphens/underscores. Zero quotas select
16 MiB per file, 128 MiB live total and 4096 resources. Resource paths preserve
the filesystem engine's 1024-byte normalized path limit and segment vocabulary
(letters, digits, dots, underscores and hyphens). Dot/parent segments, symlinks,
special files and internal transaction names cannot become user resources.

With `root_dir`, content operations use that direct mutable root. Without it,
content stays in `cache_dir/webdav/site_id/content`; the sibling `tombstones`
tree and `.lock` keep their existing roles and names. Managed deletes mask the
resource and descendants; recreation clears the relevant tombstone. Source
conditions, collection handling, quota accounting, COPY depth 0/infinity, MOVE,
Overwrite and source/destination ancestry rejection retain the existing storage
semantics. Host-owned packed content and authentication adapters remain outside
this filesystem engine. `rotmos_storage_filesystem_content_directory` calculates
the configured content directory without creating it, for host extraction and
hydration.

PUT begin creates one bounded transaction resource and a randomly named
`.vectis-tmp-` file in the destination parent. Writes copy only the offered chunk
to that file; no complete upload is materialized. Concurrent uploads follow
configured server capacity, with each transaction capped by its file quota.
Partial files and preserved recovery directories do not count as live content.
Uploads hold no mutation lock between operations.
Finish reacquires the shared mutation lock, checks the original entity-tag
conditions again, resolves the same parent, recomputes quotas, hashes/syncs/closes data and
renames the transaction into place. A competing commit makes an old conditional
upload fail without overwriting the winner. Abort or resource close removes owned
partial output. `created` reports whether a committed destination was new; HTTP
facades can select 201/204 without an unlocked lookup.

## Standalone filesystem calls

The `rotmos_storage_filesystem_lookup/read/put/delete/mkcol/copy/move/list`
functions lift Vectis's standalone WebDAV filesystem operations. They require no
server, loop or executor and use the same disk helpers, mutation locks, quotas,
conditions, tombstones and recovery paths as HTTP. Pass NULL entity-tag arguments
for unconditional mutation. COPY also takes depth 0 or -1; MOVE is recursive.
`rotmos_storage_filesystem_path_normalize` uses the same baseline path rules.

These calls borrow configuration/conditions only for their duration. Lookup
returns the entry kind, size and storage path; read materializes malloc-owned
bytes and supplies the unquoted 64-character SHA-256 ETag. Empty file reads return
NULL/zero. A non-file entry follows the baseline read result with no body.
Listing invokes its callback incrementally and stops successfully on zero.
Both standalone listing and HTTP PROPFIND use directory metadata without opening
or hashing child files. Unreadable regular files and literal filename punctuation
retain the baseline listing behavior; HTTP encodes the returned names.
Scratch state is heap-backed for default worker stacks. The installed reference
adapter is `tests/vectis-filesystem.h`; its ported baseline cases execute on a
default-stack thread before any server starts.

Content-directory calculation needs only an absolute direct root for direct
storage; managed storage validates its cache/site/quotas. It retains the baseline
4096-byte storage-path capacity. Absolute storage configuration may contain dot
segments; component walks retain no-follow checks. Resource paths use their
separate traversal checks.

Native jobs perform lock waits, directory scans, SHA-256 entity tags, file writes,
copy/move and cleanup. Lock acquisition uses cooperative nonblocking flock
attempts on the native thread. Content hashing and copies use 2 KiB chunks and
check cancellation. Listing retains one directory cursor and returns one immediate
child per operation, with explicit END; it never constructs a tree/vector.
Opened read results keep one descriptor for metadata and subsequent bounded
pread calls. Those asset reads are ordinary synchronous filesystem calls; the
asset provider uses owner-loop execution, so its filesystem reads may block.

Conditions and quota accounting share the storage mutation lock with every
provider using the same cache/site. External filesystem writers must coordinate
separately. The provider advertises write, conditional, copy and move capabilities;
it also advertises owner-loop mutation scope checks, as used by the
[WebDAV facade](webdav.md). It does not advertise cross-resource atomicity or
directory-fsync crash durability.
A successful PUT acknowledges data fsync plus replacement, rather than a stronger
filesystem durability contract.

A direct MOVE can fail while removing its source after installing the complete
destination. The result reports `committed`, `partial` and a recovery path if an
overwritten destination remains under `.vectis-txn-.../backup` in the destination
parent. Direct-root recovery paths are resource-relative; managed rollback
recovery paths refer to the retained metadata transaction directory. Cleanup or
rollback failure must preserve that directory instead of deleting the backup.
The native-job acknowledgement preserves these outcomes after cancellation;
request close does not imply rollback or release a still-running job. Inspect and
recover these retained copies before retrying. Live-content quotas and the bound
on open transaction resources do not cap accumulated recovery copies or files
left by process/OS cleanup failure.

The short direct/managed fixtures cover incremental visibility/abort, copied
chunks, SHA-256 validators, retained old descriptors after replacement, competing
conditional uploads, total/file/resource quotas, finite listing/depth, tombstones,
symlink and malformed-condition rejection, and lock-wait timeout while owner-loop
timers continue. Unprivileged direct-root runs also force source and backup cleanup
permission failures and verify retained recovery copies. Both modes run against
relocated installed static/shared consumers.
