# Response delivery

Include `<rotmos/response.h>`, or the umbrella `<rotmos/rotmos.h>`.
`rotmos_request_response(request)` borrows the request's response on its owning
loop. Ordinary routes normally use the staged reply argument: middleware can
replace it, and the router submits the final declaration after the chain returns.
Use low-level response operations for explicit bounded delivery. Mixing a
selected staged reply with a begun low-level response is a conflict.

Set status, copied headers, optional known length, and writable/close callbacks
before `rotmos_response_begin`. Begin queues headers without claiming delivery.
Header setters reject transport-owned framing and connection fields. The finite
`limits.max_response_header_bytes` bounds headers and trailers independently of
request headers. Any complete header/trailer set must also fit
`io.output_queue_bytes`; impossible sets return LIMIT.

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

`rotmos_response_file_take` takes an open regular-file descriptor on success and
checks its offset and length against the target's actual seek representation.
It uses bounded pread on the owner loop: filesystem I/O can block. It neither
maps nor materializes the complete file. Failed registration retains caller
ownership. The staged file helper pins the file when selecting the reply.

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
final response replaceable. Replacement preserves queued interim blocks and any
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
Producer-supplied trailer configuration and the remaining protocol helpers are
separate implementation surfaces under the [complete specification](server-api-spec.md).
[Connection channels](channels.md) support validated WebSocket tunnel upgrades
and bounded duplex output.
