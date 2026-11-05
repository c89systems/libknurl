# Response delivery

> Implementation reference under correction. The [extraction contract](server-api-spec.md) governs required behavior and scope; this document describes current code, not compatibility or an additional product requirement. See the [audit](server-api-audit.md) for known failures.


Include `<rotmos/response.h>`, or the umbrella `<rotmos/rotmos.h>`.
`rotmos_request_response(request)` borrows the request's response on its owning
loop. Ordinary routes normally use the staged reply argument: middleware can
replace it, and the router submits the final declaration after the chain returns.
Use low-level response operations for explicit bounded delivery. Mixing a
selected staged reply with a begun low-level response is a conflict.

Staged replies preserve Vectis's accepted 100..599 status range. Output suppresses
the body for 1xx/204/205/304 and HEAD; status 100 retains Kore's bare Continue head.
Final heads use Kore's shared private codec: configured Server/HSTS, keep-alive
metadata and request budgets, native cookies/headers and reason phrases. The
pristine vendor tree stays unchanged; the codec extraction is an external patch.
Handler/hook failure discards staged application headers/payload and emits its
diagnostic bytes with 400 for INVALID, 501 for NOT_SUPPORTED, otherwise 500.
Unselected success is 204.
The public error message/detail capacities are 256 bytes each, including NUL.
Configured pretty error pages use Kore's template for empty staged error replies,
missing/empty files and empty internal errors. Explicit diagnostic/body bytes
remain the payload. HEAD suppresses the HTML while retaining its length.

Staged headers follow the Vectis bridge: every Set-Cookie survives; other
duplicates replace the value at the first field's position and spelling.
Content-Length is filtered except for HEAD representations and 304, where its
supplied text is retained. Transfer-Encoding is filtered. Connection,
Keep-Alive, Trailer and Upgrade declarations retain their baseline wire behavior.
Low-level sender setters retain their stricter transport-owned-field validation.

Set status, copied headers, optional known length, and writable/close callbacks
before `rotmos_response_begin`. Begin queues headers without claiming delivery.
Header setters reject transport-owned framing and connection fields.
`limits.max_response_header_bytes` defaults to zero (no additional ceiling),
matching Vectis. A nonzero value bounds the complete backend head and trailers
independently of request headers. Headers are materialized metadata, drained in
bounded queue segments before body writes or producer reads. They can exceed
`io.output_queue_bytes`; end trailers and individual interim heads must still fit
that queue. Replacement/cancellation releases pending metadata.

`rotmos_response_write` accepts a bounded prefix and reports its length.
AGAIN always accepts zero bytes. Retain the unaccepted suffix and continue from
the writable callback after capacity returns. An offer exceeding a declared
length fails before accepting any bytes. A short end aborts the response.
Unknown-length HTTP/1.1 uses chunks; HTTP/1.0 closes the connection. Enable
trailers before begin and pass validated copied trailers to end. Repeating an
accepted end with identical trailers is harmless; a different set returns STATE.

`rotmos_response_producer` takes a source on success. OWNER_LOOP sources run
nonblocking reads on the HTTP loop. NATIVE_READER sources run their read callback
through the existing bounded native executor, with one bounded chunk and at most
one read outstanding per source. The shared I/O executor is created lazily in an
active post-fork HTTP worker (two default-stack threads, 32 admitted jobs). Full
admission defers submission without waiting on the HTTP loop.
Registration does not read, reset or close it. Begin separately, then let the
runtime pull into bounded chunks only when capacity is available. AGAIN pauses
the producer until `rotmos_source_resume` or `rotmos_response_producer_resume`;
repeated resumes coalesce and schedule a later pull. Other threads request resume through an owner-loop
waker. EOF is the only successful end indication. Manual writes/end are
exclusive with producer delivery. Source close runs once after drain, failure,
replacement or body suppression. Native callbacks receive independently retained
userdata and a chunk; never keep live request/response handles there. Cooperative
native readers query `rotmos_source_cancelled` during their read, including on
worker stop. Cancellation revokes delivery immediately; close waits for native
acknowledgement on the owner loop and can run after request closure. Keep the
source's independent userdata until close; request close does not free it.
Owner-loop source close runs before request views expire. HEAD and bodyless
responses never start a native read. Explicit resume also preserves readiness
arriving before an in-flight native callback returns AGAIN.

`rotmos_source_config.reset_before_reply` selects Vectis's delayed reset contract
for a staged live source: reset once on the owner loop after after-hooks and
before headers, when a reset callback exists. Suppressed output never resets or
reads it. Reset failure produces an uncommitted 500 (with Kore's error page when
configured); failed/unselected sources close before request-close callbacks.
Reset callbacks cannot mutate the
busy reply. The default remains false; manual, parsing and spooled consumers retain
their explicit reset policy. Lua legacy stream declarations use this same C
option; their separate first-read reset path has been removed.

`rotmos_response_file_take` takes an open regular-file descriptor on success and
checks its offset and length against the target's actual seek representation.
It uses bounded pread on the owner loop: filesystem I/O can block. It neither
maps nor materializes the complete file. Failed registration retains caller
ownership. `rotmos_reply_file` instead copies a path at selection and opens/pins
it at submission after hooks. A hook may create, replace or remove that path or
replace the reply altogether. Missing/unreadable final paths produce a 404 while
retaining staged headers/type, with Kore's configured error page. HEAD resolves
the representation; 1xx/204/205/304 do not open the path.
Generated/spooled private-file replies have
already finished their file before returning and retain their existing FD.
Nonempty staged files share Kore's Last-Modified/date-matching code. Its exact
If-Modified-Since match selects 304 before body reads, including HEAD; a later
date is not an equality match. Empty files skip metadata and condition handling.
Kore's conditional-file 304 retains its zero length unless a HEAD representation
length was supplied. Generated/spooled files use the completed file's timestamp.

`rotmos_source_file(path)` opens a regular file once at its current length and
uses the same bounded reader as file-backed body results. The Source retains an
independent descriptor/cursor, supports reset, excludes later growth, and fails
if the file is truncated before its declared end. Renaming/unlinking the path
does not change the pinned file; close releases the descriptor without unlinking.
Opening a FIFO or directory fails. Filesystem reads run synchronously on the
owner loop. Mapped Lua file parsing uses this public reader. The short
[file-source fixture](../tests/source-file.c) covers these ownership/failure cases.

`rotmos_reply_source_spooled` lifts Vectis's synchronous `response_source`:
it borrows an unowned source, resets it once when supported, and copies bounded
8192-byte chunks into a mode-0600 temporary file before returning. The directory
argument defaults to `/tmp`; it has no new payload ceiling. File-offset and
filesystem limits still apply. AGAIN is unsupported for this synchronous call.
It never closes the caller's source. Success selects a FILE reply holding an
unlinked descriptor through delivery, suppression, replacement or destruction;
failure closes the partial file and preserves the previous payload and headers.
The borrowed source stays pinned during callback/replacement cleanup. Buffered
`rotmos_reply_source_materialized` retains its separate finite memory bound and
explicit reset flags. These are explicit materializations, with no live-streaming
claim. [Spooled JSON](json.md) shares that private file lifecycle.

The [public materialization fixture](../tests/reply-spooled.c) proves output
larger than the buffered default, descriptor privacy/lifetime, source reentry,
buffered replacement ownership, input/reset/AGAIN and actual file-size failures,
plus synchronous stack-record JSON serialization. The installed
[Lua fixture](../tests/lua/foundation.lua) covers the matching `spooled_source`
declaration, HEAD, chunk remainders and allocation/close failures.

`rotmos_response_bytes` copies a complete body under the output bound and
automatically begins/ends delivery. Failure leaves no partial reply. HEAD and
204/205/304 suppress payload copying or producer pulls; transferred owners still
close once. HEAD/304 preserve a supplied representation length, 204 omits length,
and 205 sends zero length. Large already-materialized staged payloads transfer
their existing allocation and drain in bounded chunks without another whole copy.

BUILDING, ACTIVE, FINISHING and terminal state are separate from the irreversible
commitment latch. Replacement requires an untouched final response with no
active final transport buffer. Actual positive final output or a retained TLS
retry commits it.
A producer failure before commitment selects a bounded 500; after commitment it
aborts without a second status or successful final chunk. Writable/close
callbacks and OWNER_LOOP source pulls run on the owner loop. Terminal notification
reports the selected status, cause, commitment, actual wire/body counts and elapsed
monotonic milliseconds. Handles expire after terminal callbacks return.

`rotmos_response_interim` queues copied HTTP/1.1 100 or 103 metadata while the
response is BUILDING. Each block has at most 100 fields and must fit both the
response-header bound and output queue. Each request accepts at most 16 blocks
and 128 KiB of serialized interim output. AGAIN accepts nothing; the writable
callback waits until the failed operation can fit. Invalid fields and
transport-owned headers fail without publishing a partial block.

Interim bytes preserve output ordering and count in both `wire_bytes` and
`interim_wire_bytes`. Sending an interim or retaining its TLS retry leaves the
final response replaceable. Automatic HTTP/1.1 `100 Continue` uses that same
managed queue, bound and counters after admission. Unsupported HTTP/1.0
expectations return 417 before body delivery. The
replacement operation preserves queued interim blocks and any
native buffer referencing them while removing untouched final output. The
interim budget remains cumulative after replacement. Pending interim output uses
the ordinary response write-idle deadline.

The [focused wire fixture](../tests/response-runtime.c) demonstrates manual
bounded writes, source resume, trailers, large staged replies and wide file
ranges. The [proxy fixture](../tests/proxy-runtime.c) covers interim queue
pressure, forwarding and subsequent final-response replacement.
The [native-source fixture](../tests/source-runtime.c) covers bounded executor
admission, HTTP responsiveness during blocking reads, explicit and early wakes,
invalid producer results, body suppression, disconnect and worker-stop cleanup.
Live replies enable trailers explicitly and supply their final fields through
`rotmos_response_end`; proxy trailer forwarding stays in the proxy engine.
[Connection channels](channels.md) support validated WebSocket tunnel upgrades
and bounded duplex output.
