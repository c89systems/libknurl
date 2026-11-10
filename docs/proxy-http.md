# HTTP and SSE reverse proxy routes

Include [`<knurl/proxy.h>`](../include/knurl/proxy.h), or the umbrella
`<knurl/knurl.h>`. Initialize `knurl_proxy_config`, set a route pattern and
HTTP/HTTPS base target, then call `knurl_router_proxy`. Registration copies
patterns, target URLs, TLS PEM bundles and callbacks without connecting or
executing application policy. Callback userdata remains caller-owned through
router destruction. The returned route belongs to the router.

```c
knurl_proxy_config proxy;
knurl_proxy_config_init(&proxy);
proxy.path = "^/api/.*$";
proxy.path_kind = KNURL_PATH_REGEX;
proxy.target = "https://upstream.example";
status = knurl_router_proxy(knurl_server_router(server), &proxy, NULL, &error);
```

Literal, parameter and anchored regex patterns share router registration order.
Validated raw path/query bytes construct the upstream request target. Encoded
separators and trailing paths rejected by ordinary route decoding can select a
proxy-only raw-path fallback; ordinary routes retain their path rules. No decoded
parameter is silently substituted into the target. A rewrite can select one of
up to 16 configured targets, edit raw path/query, method and end-to-end headers,
or select an explicit Host. Targets cannot contain credentials, query, fragment,
control bytes or ambiguous path segments.

Preflight runs at headers time before exchange allocation or upstream connect.
It may select an explicit copied binary local reply of at most 64 KiB. The reply
drains in bounded output chunks, including when its body exceeds the output
queue. Raw proxy routes use this admission hook; ordinary staged middleware and
group reply hooks do not implicitly wrap takeover. Rewrite then receives the
validated inbound metadata and sanitized outbound fields. Final-response edits
run on the owner loop before commitment. Hook views and their byte/string fields
are borrowed for the synchronous callback. Failed edits remain sticky even if
the callback ignores their return value. Transport-owned fields cannot be
changed through generic header edits; repeated end-to-end fields retain order.

The worker rebuilds forwarding headers from connection metadata and the inbound
Host, removing spoofed forwarding fields and connection-nominated fields. The
forwarding source is the real TCP socket peer. Server trusted-client resolution
is available separately as request effective-address metadata; it does not
replace the proxy transport's socket-peer forwarding source. Configured default trust uses the selected curl
SDK; optional copied CA/client identity bundles are passed in memory. Peer and
hostname verification stay enabled. HTTPS auto mode prefers HTTP/2 with HTTP/1.1
fallback; cleartext uses HTTP/1.1. This adapter accepts HTTP/1.1 downstream
requests. Request trailers force HTTP/1.1 upstream.

Request framing and each direction use bounded chunks. The proxy never collects
or spools an HTTP/SSE body. A slow downstream pauses upstream reception while
uploads can continue, and upload pressure pauses downstream reads independently.
The upload framer retains the exact unread suffix and returns pipelined bytes to
the channel once. Declared request trailers and valid response trailers are
forwarded. Body-bearing downstream replies use chunked framing so late trailers
remain possible. HEAD and 204/205/304 preserve their payload-suppression rules.
Each completed proxy reply closes its downstream connection, matching the Vectis
proxy transport. The ordinary managed 10 MiB soft body default does not limit
takeover; an explicit `limits.hard_body_bytes` ceiling does apply, with known
oversize lengths rejected before connection and chunked decoded bytes checked
incrementally.

100/103 metadata uses the public [interim response API](responses.md), with finite
per-block, per-request and output-queue budgets. A locally accepted Expect request
can receive 100 before upload data arrives. Interims do not commit final status.
On an upstream/setup/framing failure, an untouched final response can be replaced
by 502, or 504 for deadline expiry; the gateway hook may select a bounded local
reply. An active final transport buffer is retained safely and cannot be
replaced. After final commitment the connection aborts without another status or
a successful terminal chunk. Gateway hooks run outside curl callbacks.

`server_config.proxy.active_exchanges` resolves zero to 16, and
`idle_connections` resolves zero to four per curl pool. These values are visible
in effective configuration. Admission is worker-wide across routes and precedes
per-exchange transport buffers. Its slot remains held through downstream close,
including while an already completed upstream response drains. Route direction
buffers default to 16 KiB and accept 8 KiB..1 MiB; curl's maximum callback window
also bounds the retained download allocation. Headers and explicit local replies
have independent finite budgets. No raised resource limit carries an unmeasured
RSS promise.

The route defaults are a 10 s connect deadline, 60 s idle deadline and disabled
total deadline. Positive transport progress refreshes idle timing; callbacks and
wakeups alone do not. Idle/total deadlines continue through downstream drain and
SSE waits. Independent server connection-idle, deferred-response and pending
write-idle limits remain active. Vectis's adapter must map its existing effective
deadline policy, including explicit disabled values, to avoid adding a shorter
standalone deadline. Cancellation retires curl watches before owner-loop teardown;
queued callbacks keep their exchange alive until retirement and never dereference
a closed request.

The [public live fixture](../tests/proxy-runtime.c) covers copied routes, raw
fallback, rewrite/Host/forwarding policy, repeated fields, binary and 64 KiB local
replies, HEAD, hard limits, 100/103 queue pressure and replacement, 64 KiB duplex
upload/download before upload completion, request/response trailers, SSE progress
alongside an ordinary request, worker admission, idle/total deadlines and failure
after commitment. The same fixture is compiled and run against relocated static
and shared installed SDKs, without private headers or Kore declarations.

The same registrations also execute [WebSocket/WSS relays](proxy-websocket.md),
with validated opening responses, opaque duplex bytes and streamed non-101
rejections. Their admission and deadlines use this worker policy. Upstream
The [HTTP/2 fixture](../tests/proxy-h2.c) proves live TLS/ALPN duplex delivery
before upload EOF. Forced retry/fault combinations and slow-peer RSS claims
require their own evidence; see [verification](verification.md).
