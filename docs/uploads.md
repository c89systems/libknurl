# Upload callbacks

Include `<rotmos/upload.h>` or `<rotmos/rotmos.h>`. The stream facade lifts
Vectis's open/write/finish/close lifecycle onto the existing managed body plan.
It adds no transport, parser, Vectis state, or private dependency types.

Initialize `rotmos_upload_stream_config`, set method/path, callbacks and userdata,
then register with `rotmos_router_upload_stream` or
`rotmos_route_group_upload_stream`. Registration copies the declaration and path;
userdata remains caller-owned. It opens no resource and invokes no callback.
The default body policy retains Vectis's upload bound; set `body.max_bytes` for
the route and respect the server's hard ceiling. STREAMING_UPLOAD and STREAM
both select the same live STREAM delivery here. They never materialize or spool
the body. Managed POST/PUT/PATCH still require Content-Length.

Auth and before hooks run before body admission. An early staged reply bypasses
the upload callbacks. Open runs on the first admitted body callback, or at body
completion for an empty upload. Oversized or invalid framing/length declarations
cannot open a destination. Open is optional; without it state is NULL. Open
failure owns its own partial cleanup; successful open owns one close callback.

Write receives a bounded chunk and returns a positive consumed prefix, or AGAIN
with zero. The unconsumed suffix remains transport-owned. Both a partial prefix
and AGAIN pause input until `rotmos_request_body_resume`. A callback ready for
another turn may call resume before returning; delivery never recurses. A
callback waiting for external work instead resumes from its later owner-loop
notification. Zero-progress OK, overconsumption and bytes accompanying an error
are invalid. No callback may wait for another thread, perform blocking reader
work, or call Lua on a foreign thread.

Finish runs once after all admitted input has been consumed, including for an
empty body. It selects a staged reply; then normal after hooks may inspect or
replace that reply before submission. Failure or an absent reply takes the
normal bounded error response path. Close runs once on the owner loop after
success, callback failure, disconnect, timeout or shutdown, with still-valid
request metadata and the terminal cause. Cancel application timers and watches
there. The facade does not consume `rotmos_request_userdata`; hooks and handlers
retain its application meaning.

The [public-only fixture](../tests/upload-runtime.c) demonstrates registration,
group hooks, prefix consumption, a timer-driven resume, empty input, rejection
before open, error cleanup, and disconnect during input. It also checks the
stateless form with no open callback.

`rotmos_router_upload_reader` and `rotmos_route_group_upload_reader` lift Vectis's
native reader handler. Initialize `rotmos_upload_reader_config` and supply its
handler, route and body policy. The callback receives immutable copied
`rotmos_upload_metadata`, a borrowed `rotmos_source`, and an independent staged
reply seeded with before-hook headers. Metadata includes method/version/target,
raw and decoded path/query, authority, ordered duplicate-preserving headers,
query pairs and path parameters, listener, endpoints, TLS presence and declared
length. Text views have an extra trailing NUL; lengths remain authoritative.
The callback never receives a live request/backend handle or Lua state.

The same shared post-fork I/O executor used by [native output sources](responses.md)
runs the handler once. A finite pipe queue holds at most `buffer_bytes` (zero
selects 256 KiB) and the OS pipe capacity. Input accepts bounded prefixes and
pauses when full; native reads notify the owner through a public waker. Full
executor admission defers job submission. No HTTP callback waits or joins.
There is no complete-body buffer or hidden file spool.

Only the native handler may call `rotmos_source_read` on its borrowed reader.
It can wait for incremental input there and query `rotmos_source_cancelled`.
Reset, destroy and ownership transfer of this reader return STATE. Consume all
admitted input and select a reply. EOF is supported; a parser that finishes at
exactly the declared length need not add a redundant EOF read. Early return
without consuming the input, an absent reply, or a callback error fails the
request. After acknowledgement, the owner runs after hooks and submits the
result. Copied metadata and the reader/result views expire when the handler
returns. Caller userdata stays retained by the caller until server run returns.
Cancellation revokes HTTP delivery immediately and retains native work until its
acknowledgement; an uncooperative callback remains subject to the existing worker
shutdown deadline. Lua uses the owner-loop stream callbacks above.

`rotmos_router_upload_file` and `rotmos_route_group_upload_file` reuse that reader
path. Initialize `rotmos_upload_file_config`, set the route and explicit
`file_path`, and supply the body policy. The path is copied at registration.
The output is a caller artifact: native work opens it with `wb`, incrementally
writes input, checks flush/close, and leaves partial output on failure. An invalid
or oversized request cannot truncate it. The response preserves Vectis's actual
file helper behavior: 200 `text/plain` with the decimal uploaded byte count.
Empty uploads truncate the destination and return `0`. The counter uses checked
wide arithmetic on every target. No temporary file or transactional rename is
introduced. File queue capacity uses `body.memory_buffer_limit_bytes`.

The same [public fixture](../tests/upload-runtime.c) exercises a three-byte queue,
36 simultaneous readers with an unrelated control request, native versus owner
thread callbacks, metadata after cancellation, request/header/query copies,
borrowed-reader rejection, staged source results, empty uploads and file errors.
It runs against both installed static and shared SDK targets as well as musl
worker threads with default stack sizes.
