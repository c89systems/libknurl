# WebSocket and WSS reverse proxy relays

> Implementation reference. The [extraction contract](server-api-spec.md) governs required behavior and scope. See the [audit](server-api-audit.md) and [verification](verification.md) for evidence and limits.


[`rotmos_router_proxy`](../include/rotmos/proxy.h) uses the same copied route
configuration for ordinary HTTP/SSE and WebSocket opening requests. A configured
HTTP target supplies a plain upstream socket; an HTTPS target supplies TLS.
Preflight, target selection, raw path/query rewriting, explicit Host and
forwarding policy retain the [HTTP proxy contract](proxy-http.md). Malformed
openings are rejected before transport allocation. Shared worker admission
precedes each exchange's buffers and remains held until downstream close.

The relay uses curl connect-only mode with HTTP/1.1, asynchronous DNS, normal
peer/hostname verification and the route's copied in-memory CA/client identity.
After connection, curl's watch is cancelled before the relay registers its own
borrowed socket watch. The easy handle and admission slot remain owned by the
worker transport until cancellation. The relay removes its replacement watch
before releasing the transfer. Cancelled watch userdata stays alive until event
retirement; a replacement can register the same descriptor before retirement.

The opening request is serialized under a finite 128 KiB bound. Upstream response
heads have a separate 64 KiB bound. 100/103 metadata shares the managed interim
response budgets. A 101 must match the original key and negotiated protocol and
extension offer. Sanitized end-to-end fields plus regenerated Upgrade/Connection
fields pass through the public channel's validated upgrade operation. The final
response rewriter does not run on successful 101 metadata. Early client bytes
remain held until that upstream validation succeeds; upstream bytes coalesced
with the opening response remain ordered after the downstream 101.

After upgrade, each direction carries opaque bytes in bounded route chunks plus
the channel's explicitly bounded queues. The relay does not parse application
frames, concatenate fragments, decompress extensions or collect messages. Large
frames and fragmented/extension traffic therefore retain their wire bytes even
when they exceed the application WebSocket API's frame bound. Each direction
pauses independently under pressure. Partial sends retain their exact allocation
and suffix until accepted. Network progress refreshes the shared proxy idle
clock; wakeups and retry timers do not. Total deadlines also remain active.

Downstream TLS retry direction belongs to the channel backend. Curl's public
connect-only interface does not expose the upstream TLS retry direction. The
relay retains a bounded retry delay starting at 25 ms and doubling to at most
one second until positive progress resets it. An ambiguous TLS retry suspends
socket interest during that delay, preventing an always-ready event from
spinning. Plain idle reads wait for readability. No private curl/OpenSSL
connection state is inspected.

Downstream read EOF drains accepted upstream output before shutting the borrowed
socket's TCP write half. Curl provides no connect-only TLS close-notify operation;
this is a TCP half-close, including for TLS upstreams. The receive direction
continues independently. Upstream EOF drains retained downstream bytes and its
channel queue before normal close. A protocol or transport failure after upgrade
aborts the connection. Worker shutdown cancels relay watches, retries and curl
transfers before releasing requests or the loop.

A non-101 response stays on the managed HTTP response path. The existing HTTP
header validator and fixed/chunked framer decode one bounded body prefix at a
time, preserving declared framing, trailers, duplicate end-to-end fields and
the final-response modification hook. A slow client pauses upstream reads. No
rejection body is collected or spooled. Fixed-length truncation and invalid
chunking select a gateway reply before commitment, or abort committed output
without a successful terminal chunk. Gateway edits retain the 64 KiB local-body
bound and sticky-failure behavior.

The [public wire fixture](../tests/proxy-websocket.c) runs plain and TLS variants.
It checks early-byte holding, 103/101 ordering, a 64 KiB opaque fragmented duplex
exchange, plain TCP half-close, a synchronized 64 KiB chunked rejection with
trailers, invalid acceptance, failed metadata edits, shared HTTP/WebSocket
admission, an ordinary concurrent request, idle expiry and truncation after
commitment. The TLS variant verifies both listener TLS and upstream mTLS using
copied in-memory material. A recovered worker crash fails the fixture. Both
variants also run against relocated static and shared installed SDKs.

Upstream HTTP2 live coverage, forced TLS retry-direction faults, allocation/close
fault matrices and slow-peer memory measurements remain combined proxy
verification under the [complete specification](server-api-spec.md#executable-surface).
