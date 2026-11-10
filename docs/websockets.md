# Application WebSockets

The [public API](../include/knurl/websocket.h) accepts application WebSockets
from a headers callback or registers them through `knurl_router_websocket`.
The handle, frames and terminal outcome are borrowed. Send, close, resume and
userdata changes require the connection's owner loop. Callbacks execute on that
loop; no application callback is invoked recursively by these operations.

`knurl_websocket_accept` validates HTTP/1.1 GET, Upgrade/Connection, the canonical
16-byte challenge, version 13, absence of a body and any selected subprotocol.
It preserves unrelated staged response headers and copies supplied fields.
Failure leaves the managed response, input ownership and caller userdata intact;
no WebSocket close callback is acquired. Success requires the request plan's
`KNURL_REQUEST_WEBSOCKET` disposition and owns exactly one terminal notification,
including failure before open. Accepted handshake ownership prevents managed
reply replacement; abort remains available to cancel it. Extensions are never
negotiated on this path.

The handshake response drains before upgrade completion. The request terminal
outcome records `upgraded`, then request callbacks run and its native request is
freed. Only then can `on_open` run. WebSocket callbacks never receive the expired
request; copy needed metadata during headers dispatch. Request userdata and
WebSocket userdata are independent. Sharing a pointer requires explicit transfer
so only WebSocket close releases the shared state. The WebSocket handle expires
when `on_close` returns. The library never frees application userdata.

The route helper owns copied path, handshake headers and protocol declarations.
Its optional `on_headers(request, config, plan, userdata, error)` receives a
per-request configuration copy: it may set connection userdata, protocol or
callbacks. A MANAGED plan proceeds to acceptance; a completed REPLY rejects the
upgrade. Request close/userdata remain separately configurable. The helper is a
GET route with literal, parameter or regex matching. Admission owns this raw
headers path, as for other raw routes; staged handler/group middleware does not
implicitly run. Applications can perform policy there through public APIs.

```c
static knurl_status echo(knurl_websocket *ws, knurl_websocket_opcode opcode,
                          knurl_bytes frame, void *data, knurl_error *error)
{
    (void)data;
    return knurl_websocket_send(ws, opcode, frame.data, frame.length, error);
}

static void retry(knurl_websocket *ws, void *data)
{
    (void)data;
    (void)knurl_websocket_resume(ws, NULL);
}

/* During inert server configuration: */
knurl_websocket_route_config route;
knurl_websocket_route_config_init(&route);
route.path = "/events";
route.websocket.on_frame = echo;
route.websocket.on_writable = retry;
/* Check the returned status. */
knurl_router_websocket(knurl_server_router(server), &route, NULL, &error);
```

TEXT and BINARY callbacks receive one complete bounded frame. OK consumes it.
AGAIN consumes nothing, holds the same bytes and pauses input. Explicit resume
redelivers that frame before any later frame; resume inside a callback schedules
later delivery. A send returning AGAIN queues nothing and arms `on_writable`
when the last attempted frame fits. Capacity can change before a retry; handle
AGAIN again. The example uses that notification to resume its held input frame.

Each accepted output frame is copied atomically into a finite queue, including
its 2/4/10-byte wire header. LIMIT means the frame can never fit the selected
queue bound. The receive payload limit does not restrict outgoing frames.
A partial transport write retains its allocation across
plain/TLS retries. Its full allocation remains charged until released, even
after a partial write; `queued_bytes` reports that retained wire storage.
Zero receive size inherits the server's frame limit. Zero output budget retains
Kore's send behavior (SIZE_MAX); an explicit budget must hold at least a maximum
control frame (127 bytes).
Inbound storage holds one configured maximum frame, a small header and the
bounded channel input queue. Output budgets are independent of the server's
ordinary HTTP queue bound. No message assembly is introduced.

The receiver retains Kore's masking, reserved bits/opcodes, receive payload
limit and control-frame FIN/length checks. Extended nonminimal lengths are
accepted. Text bytes and peer close payloads are not UTF-8/code validated.
Ping queues a bounded pong;
input pauses if control output cannot yet fit. Pong is consumed internally.
TEXT/BINARY callbacks receive each frame even without FIN; frames are not
assembled. Sending CONTINUATION is accepted and sets FIN, as does Kore's sender.
Receiving continuation, malformed frames or oversized input disconnects without
inventing a protocol close reply. Unsupported extensions/reserved bits are rejected. Transparent
[channel tunnels](channels.md) remain opaque to framing and fragmentation.

`send(CLOSE)` only queues a raw frame: it leaves the connection open, with no
new deadline or send restrictions, matching Vectis. Lua's no-argument `close()`
uses that operation. The explicit `close(code, reason)` helper instead
initiates an idempotent bounded handshake, retaining at most 125 bytes of pending
control data while accepted output drains. Code zero sends an empty close.
The explicit close helper validates its supplied code/reason; raw send retains
Kore's byte behavior. The default deadline is 5000 ms, capped during worker
shutdown. Peer close receives an empty close reply; both frames must drain for
`clean` to be true. Abrupt EOF,
protocol errors and deadline expiration remain distinguishable in the outcome.
A missing peer close reports code 1006 locally; that code is never transmitted.
Worker stopping sends 1001 and retains CANCELLED as the lifecycle cause.
WebSocket idle uses `timeouts.websocket_idle`; managed HTTP request and native
keepalive timers expire with the handshake request. Output write-idle bounds
continue to apply.

The [runtime fixture](../tests/websocket-runtime.c) exercises the public API over
HTTP and HTTPS: route configuration copies, failed transactional acceptance,
request/open/close ordering, atomic queue pressure, capacity notifications,
early frames, held frame redelivery, extended lengths, control frames, malformed protocol,
close timeout and worker drain. The private channel fixture also checks that
reused Kore connection objects start with cleared event flags and retained frame
allocations continue to consume queue capacity. The [installed Lua cases](../tests/lua/events.lua)
cover callback failures, replay and backpressure through the host VM. Exhaustive
TLS retry/fault combinations are not established by these fixtures; see
[verification](verification.md).
