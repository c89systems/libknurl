# HTTP framing and proxy core

The [public framer](../include/rotmos/framing.h) incrementally decodes fixed-length
or chunked HTTP request bodies. It reuses the proxy framing implementation from
Vectis commit `6b1e263e0436687f68a7768ca55f4361a6e59e19`, whose MIT notice is the
same as this repository's [license](../LICENSE). It has no transport dependency,
creates no threads, and never collects a complete body.

Initialize `rotmos_framer_config`, set the framing/length already validated from
request headers, and register a body callback. Constructor success copies this
configuration and borrows userdata until destruction. The callback receives a
borrowed byte span and reports its accepted prefix. OK requires positive
progress; AGAIN accepts nothing. A short accepted prefix also pauses the framer.
The caller waits for its consumer's capacity before feeding the retained suffix.

`rotmos_framer_feed` reports the exact consumed input prefix, including framing.
OK means more input is needed; AGAIN means consumer backpressure; EOF identifies
the complete message boundary. Any trailing bytes remain untouched. For a
[connection channel](channels.md), give precisely that suffix to
`rotmos_channel_body_complete`. An optional synchronous trailer callback receives
borrowed validated fields; it returns OK or a terminal failure. Declaration
policy stays with the caller, as it does in the proxy upload adapter.

Chunk-size lines are bounded at 128 bytes. Trailer limits are 8192 bytes per
line, 16384 per block and 100 fields. Forbidden trailer fields, malformed CRLF,
invalid extensions and overflowing chunk sizes fail. An optional checked wide
body limit can reject an offered body span before its callback. Zero body limit
permits an arbitrary checked 64-bit length while memory remains bounded.
Fixed-length limits are checked before construction. Framer state occupies a
bounded heap allocation; no body storage is allocated.

`rotmos_framer_finish` declares transport EOF: only an already complete body
succeeds. Truncation and callback/protocol failures are sticky. Destruction and
recursive feed/finish from a callback return STATE. The
[public fixture](../tests/framing.c) demonstrates partial progress, pipeline
suffixes, trailers, reentrancy rejection, body limits and wide lengths.

The private proxy core also lifts Vectis's reusable server mechanisms:

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

The proxy declaration/hook views remain private until the route adapter is
connected. `rotmos_router_proxy`, managed 100/103 forwarding, downstream
commitment/error handling, worker-owned admission configuration and full HTTP/SSE
and WebSocket/WSS relays remain required implementation work. This core and its
fixtures do not establish reverse-proxy route availability or Vectis cutover
readiness. The next adapter reuses these components and the existing public
request/channel/response contracts; it must preserve the complete
[proxy specification](server-api-spec.md#reverse-proxy).
