# Connection channels

> Implementation reference under correction. The [extraction contract](server-api-spec.md) governs required behavior and scope; this document describes current code, not compatibility or an additional product requirement. See the [audit](server-api-audit.md) for known failures.


Include `<rotmos/channel.h>`, or `<rotmos/rotmos.h>`. In a headers callback,
initialize `rotmos_channel_config`, set the event/close callbacks and userdata,
then call `rotmos_request_takeover`. On success choose
`plan.disposition = ROTMOS_REQUEST_TAKEOVER`. Failure transfers no input or
userdata ownership and invokes neither channel callback. A second takeover or
a takeover after headers dispatch returns STATE.

Zero capacities inherit the server's finite receive/send bounds. Receive and
tail overrides must fit the server input bound; an explicit send capacity is
independent of the server default. Initial coalesced input must fit the receive bound before
ownership transfers. Keep the channel's state on the owner loop. Other threads
signal a loop waker. The accepted descriptor and TLS session have one owner;
applications never install a second watcher or perform parallel socket/TLS I/O.

HTTP_INPUT reads raw post-header bytes, including chunk framing, trailers and
early WebSocket bytes. The channel does not materialize or spool a body. Raw
writes return STATE until tunnel upgrade. Build final HTTP/SSE/rejection replies
through `rotmos_request_response`, with its existing framing, queue, commitment
and producer ownership rules. A zero-interest channel suspends ordinary events;
initial held bytes become READABLE when read interest is enabled.

Read returns a bounded prefix, AGAIN with zero bytes, EOF after readable data
has drained, or a failure. A read retry is independent of write readiness.
Drain to AGAIN or explicitly suspend while the consumer is blocked. Read/queue
progress rearms the relevant notification. Repeating unchanged interests does
not poll callbacks. Disable writable interest while the application has no
pending output to produce.

The host framer calls `rotmos_channel_body_complete(channel, suffix, length)`
once at the exact message boundary. The call copies that suffix followed by
unread prefetched input, under the configured tail bound. Failure consumes
nothing. Success disables HTTP_INPUT reads; a repeated declaration returns
STATE. The library neither guesses a boundary nor reparses the supplied tail.
HTTP resumes only after successful input completion, final response drain and
a policy permitting reuse. Incomplete or rejected input closes after the final
reply. Returned tails can exceed the native header buffer: restoration feeds
bounded prefixes and retains remaining bytes for subsequent headers/bodies.

After validating an upstream WebSocket handshake, pass its headers to
`rotmos_channel_upgrade`. The channel checks the downstream request/challenge,
Upgrade/Connection fields and selected subprotocol, then atomically queues one
101 and enters TUNNEL. The caller validates upstream negotiation, including
extension parameter semantics; negotiated fields are copied and preserved.
Invalid upgrades leave HTTP_INPUT and its response untouched. Non-101 upstream
replies remain managed responses. Validation includes headers staged on the
response; unrelated staged fields are preserved. Hold early client frames until
the upstream handshake succeeds. The channel does not use an application WebSocket
frame parser for tunnels.

TUNNEL writes copy a bounded accepted prefix. Retain the suffix on partial
acceptance; AGAIN accepts zero bytes. Queue capacity and drain notifications are
observable without backend buffers. The 101 and earlier queued output drain in
order before tunnel bytes. Existing queued-stream callbacks keep their owners.
Read EOF alone preserves pending output. `rotmos_channel_shutdown_write` drains
accepted output, performs bounded orderly write shutdown and emits
ROTMOS_WRITE_CLOSED once, even with ordinary interests suspended. Reads can
continue. New writes are rejected once shutdown was requested.

Close with OK only after queued output and any requested write shutdown have
completed; HTTP_INPUT also requires an explicit completed boundary. Error causes
cancel remaining work. Channel close notification is deferred past its active
event callback and occurs before the request's terminal callback. Metadata and
borrowed handles remain valid during those notifications. Keep one destructor
owner when both callbacks refer to the same application state. Worker stopping
cancels channels, and server idle/write-idle limits remain active. Actual I/O
progress resets activity timers; wake events do not.

The TLS adapter retains retry storage and calculates read/write readiness
separately, following OpenSSL's [read](https://docs.openssl.org/3.6/man3/SSL_read/),
[write](https://docs.openssl.org/3.6/man3/SSL_write/) and
[shutdown](https://docs.openssl.org/3.6/man3/SSL_shutdown/) contracts. The
[TLS fixture](../tests/tls-runtime.c) exercises a public HTTPS channel with early
input, bounded output and orderly half-close. Forced cross-direction retries and
slow/disconnected TLS peers remain integration coverage under
[development](development.md).

The [public fixture](../tests/channel-runtime.c) demonstrates takeover, raw
framing, bounded echo, half-close and reuse through the public shared API. The
[private fixture](../tests/channel-backend.c) adds queued native streams to prove
their ownership/order survives handoff. The complete requirements remain in the
[server specification](server-api-spec.md).
