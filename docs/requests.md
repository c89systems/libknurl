# Request plans and bodies

Include `<rotmos/dispatch.h>` and `<rotmos/body.h>`, or `<rotmos/rotmos.h>`.
The ordinary route facade still accepts staged handlers and middleware. The
router submits that handler's final reply after its selected body has completed.
Raw routes select `on_headers` instead of `handler` and own their admission plan;
staged middleware cannot be mixed into a raw registration.

To supply a dispatcher, select `config.dispatch.mode = ROTMOS_DISPATCH_CALLBACK`
and set `config.dispatch.on_headers` and its userdata. The callback runs once
after metadata/framing validation, before application body delivery, storage or
100 Continue. It receives an initialized plan. It can explicitly call
`rotmos_router_dispatch_headers`; the server does not invoke another router
after the callback returns. The runtime copies the returned plan, including its
callback/userdata fields, even on dispatch failure so assigned state gets one
terminal notification. Keep dynamically allocated state in `plan.userdata` and
release its request owner in `on_close`.

The plan's callback state is independent of `rotmos_request_userdata`, which
belongs to the application. [Global request lifecycle callbacks](policy.md)
can retain that association across private route plans and release it after
the plan's terminal callback.

MANAGED plans provide `on_body_end` unless an installed response/producer already
owns completion. NONE still delivers one empty completion event. REPLY requires
a successfully ended final response, including a successful complete-reply
convenience. An early response with unread input drains, reports cancelled input
separately and closes; it cannot turn unread input into EOF or reuse that
connection. Upgrade/takeover dispositions require their installed capability.

Body policies resolve zero limits through the configured default and enforce a
positive hard server ceiling. JSON, BUFFERED and MATERIALIZED retain completed
input up to a memory threshold, then spill unless `disk_spool_disabled` is set.
Zero threshold selects min(limit, 256 KiB); an explicit threshold above the limit
is invalid. Disabled spilling accepts fitting input and rejects input that
crosses the threshold with 413, matching Vectis. Spill failures also produce 413
with its diagnostic. An omitted spool directory selects `/tmp`; the application
can supply its resolved directory. The private mode-0600 file is removed after
request cleanup. Ordinary handlers may also use the legacy STREAMING_UPLOAD
policy with its 3 GiB default; disabling its spooling requires a fully fitting
memory budget. This policy still delivers a completed body, while live upload
registrations deliver the bounded reader/chunk callbacks.
The default-directory regression runs real spill requests and cleanup, rather
than checking registration alone.
STREAM never collects or spools the message. The [typed JSON facade](json.md)
decodes completed JSON/BUFFERED/MATERIALIZED bodies and retains its records through
response cleanup. [Native-reader uploads](uploads.md) use live bounded input independently of this
completed-body decoder.

For STREAM, `on_body` receives a borrowed chunk capped by `io.chunk_bytes` and
`io.input_queue_bytes`. OK consumes only the reported prefix; a short prefix
pauses reception. AGAIN consumes zero. The runtime retains the unconsumed suffix
and delivers it before reading new bytes. Resume on the owning loop with
`rotmos_request_body_resume`; repeated calls coalesce and never invoke a body
callback recursively. Other threads signal an owner-loop waker. Callback
side effects are never retried automatically. The initial coalesced bytes borrow
Kore's already bounded header allocation; subsequent socket reads use one bounded
chunk. Body-idle/minimum-rate accounting excludes explicit application pauses.
Independent application deadlines still apply.

Successful EOF invokes `on_body_end` once; it means input completed, not that a
reply drained. Truncation, disconnect, timeout and callback errors reach close
without a false EOF. Managed POST/PUT/PATCH require Content-Length; DELETE,
PROPFIND and MKCOL permit an absent length. The managed method rules
reject positive lengths for GET/HEAD/OPTIONS/COPY/MOVE. Chunked transfer coding
reaches the headers decision and requires takeover for actual body delivery.

A request remains live through asynchronous body/response work. Returning from
a low-level callback does not invent or end a reply. The deferred-response
deadline starts after body completion; downstream write-idle guards pending
output, including TLS retries. Source/file close notifications precede request
`on_close`. The close outcome includes input completion/cause, received/delivered
counts and the response's status, commitment and actual output counts. Metadata
and borrowed response views remain valid during close. Cancel application timers
and watches there; retaining a request pointer does not extend its lifetime.

`rotmos_request_body` borrows a successfully completed materialized body.
Memory/file kind and bounded read/rewind operations disclose its actual storage.
A file-backed body never becomes a hidden memory read-all. STREAM has no such
reader. `rotmos_request_body_materialize`, `copy` and `spill` create independent
caller results. A fitting caller buffer is borrowed; otherwise memory is owned
or the body spills under the chosen threshold. A positive buffer capacity caps
the memory threshold. Zero inherits the body's memory budget. Independent
materialization defaults an omitted directory to `/tmp` and an omitted prefix
to a library prefix. These explicit helpers can perform blocking file I/O.

Successful materialization rewinds before and after copying. Zero-length success
is a memory result without an allocation. Failure returns an empty result and
removes only partial files created by that failed operation. Result cleanup
frees owned memory/path metadata and leaves returned files for the caller to
remove. `rotmos_body_result_open_reader` returns an independently closed public
source: memory readers borrow the result/buffer; file readers pin an open FD.
Keep memory owners alive until reader close. File readers do not remove artifacts.

The [focused body fixture](../tests/body-runtime.c) is an executable public API
example. [Connection channels](channels.md) provide exclusive raw input and
tunnel takeover. The [full specification](server-api-spec.md) and
[integration contract](vectis-integration-contract.md) define the related
codec, provider and language-facade behavior.
