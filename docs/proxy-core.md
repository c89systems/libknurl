# HTTP framing and proxy core

> Implementation reference. The [extraction contract](server-api-spec.md) governs required behavior and scope. See the [audit](server-api-audit.md) and [verification](verification.md) for evidence and limits.


Proxy framing stays private in [proxy_framing.c](../src/proxy_framing.c), lifted
from Vectis `6b1e263e0436687f68a7768ca55f4361a6e59e19` under the repository's
[MIT license](../LICENSE). The unused standalone public framer and its wrapper
have been removed. Callers register proxy routes through the public proxy API;
their adapters own fixed/chunked body decoding, backpressure and trailers.
The [private framing fixture](../tests/proxy-framing.c) retains protocol and
partial-progress coverage. The installed wire fixture uses its own existing
small HTTP peer reader, so it does not depend on the implementation under test.

The private proxy core lifts Vectis's reusable server mechanisms:

- Bounded ordered headers, framing validation, sanitization and declared trailers.
- Raw URL/path validation, configured-target selection, sticky rewrite edits,
  response edits and explicit copied local replies limited to 64 KiB.
- A bounded upload queue and curl response parser supporting duplex progress,
  informational/final headers, body accounting and response trailers.
  Download storage holds one curl callback window, sized to the greater of the
  configured direction buffer and curl's maximum write callback chunk.
- WebSocket opening validation, bounded wire serialization and incremental
  non-101 rejection handling, without application frame/message assembly.
- Independent owner-loop curl pools with a shared admission default of 16 active
  exchanges and four retained idle connections per pool. Limits are explicit
  constructor inputs. HTTP auto mode prefers HTTP/2 over HTTPS with HTTP/1.1
  fallback; cleartext and raw connect-only exchanges use HTTP/1.1. TLS policy,
  in-memory trust/client identity and DNS use the selected cpkt curl/OpenSSL SDK.

The [transport](../src/proxy_transport.h) uses librotmos watches/timers and has no
Kore structures or process-global pool. Its creator owns destruction on the same
loop. Successful submission takes the easy handle; failed submission takes
nothing. Completion borrows it for one callback before cleanup. Cancellation
omits completion. [Connect-only](https://curl.se/libcurl/c/CURLOPT_CONNECT_ONLY.html)
success retains both its handle and admission
slot until cancellation; socket handoff removes the pool's watch before a relay
registers its own. The relay cancels that replacement watch before releasing the
transfer. Deferred watch retirement preserves userdata through an active event
batch. Destruction inside transport dispatch returns STATE; defer it instead.

The [live fixture](../tests/proxy-transport.c) transfers a 128 KiB producer through
real sockets and the public server loop, consumes bounded curl chunks in small
timed steps, and checks pool admission, pause/resume, cancellation and cleanup.
The lifted unit fixtures cover validators, upload framing, HTTP/trailers,
rewrite/response/local policy, and WebSocket handshakes/rejections. Wide wire
lengths format through the C89 public wide-integer implementation.

The [public HTTP/SSE route adapter](proxy-http.md) connects these components to
request takeover, managed 100/103 output, final-response commitment and worker
admission. Configuration and opaque hook views are installed in
[`<rotmos/proxy.h>`](../include/rotmos/proxy.h). The
[WebSocket/WSS relay](proxy-websocket.md) uses the same worker ownership and
admission, the lifted handshake/wire validators and the public raw channel.
Non-101 replies share the HTTP response writer. Vectis cutover still requires the
complete
[proxy specification](server-api-spec.md#executable-surface).

## Terminal exchange diagnostics

`rotmos_proxy_config.on_exchange` receives one borrowed terminal snapshot for
each admitted upstream exchange, before the host request-close callback. The
host can retain `vectis.proxy.exchange` formatting and its existing logger;
accounting stays in the proxy engine. The snapshot includes the selected
configured target, route, HTTP/WebSocket kind, final status, request ID, duration,
accepted upload/download counts, error category and disconnect side. IDs and
duration start at upstream admission, after rewrite/setup, as in Vectis.

HTTP upload counts are bytes consumed by curl; download counts are body bytes
accepted by the downstream queue, excluding heads, chunk framing and trailers.
HEAD output contributes zero. WebSocket counts are accepted opaque relay bytes,
excluding the opening handshake. These are accepted-byte counters, not bytes
proven delivered to a peer. Hosts do not need to recreate transport logic.

Lua's `proxy{on_exchange=function(snapshot) ... end}` copies those fields into a
table through the same public C callback. Callback exceptions and allocation
failure are protected and reported at server completion, after transport cleanup.
The callback cannot replace an already completed response. Upstream failures
also set the public request observation's upstream disconnect classification.
