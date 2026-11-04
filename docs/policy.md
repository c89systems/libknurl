# Address policy, admission and request observations

Include `<rotmos/server.h>` and `<rotmos/request.h>`, or `<rotmos/rotmos.h>`.
These callbacks separate the server mechanisms from application policy. They
execute on the HTTP worker's owning loop. Configuration is inert: construction
and validation do not invoke them. Function and userdata pointers are borrowed
for the server lifetime.

`config.client_ip.trusted_proxies` retains Vectis's numeric-address trust list.
The server copies the list and its strings. Untrusted TCP peers cannot select
their effective address through forwarding headers. For trusted peers, the first
X-Forwarded-For field is walked from right to left, returning the nearest
untrusted address or the leftmost address when the entire chain is trusted.
Malformed chains fall back to a valid trimmed X-Real-IP, then to the TCP peer.
IPv6 trust comparisons use address bytes, so equivalent textual forms match.
Transport admission and observations always retain the actual TCP peer.

`rotmos_request_effective_address` exposes the resolved request address without
changing `rotmos_request_peer`. An optional `client_ip.resolve` callback sees
that default and writes a replacement numeric IPv4/IPv6 address into a supplied
46-byte buffer. An optional `correlation_id.resolve` writes at most 128 bytes.
It may return an empty identifier; NUL, control bytes and DEL are rejected.
Neither callback must append a terminator. The server validates and retains
successful results through request close. Correlation is empty by default:
applications choose their identifier source. Resolver errors become bounded 500
responses and still receive final observation/close notification. A failed
address override preserves the safe default for that notification.

`callbacks.on_request_start` runs after identity resolution, before admission
and dispatch. It can associate application state with
`rotmos_request_set_userdata`; it cannot begin response I/O. This association is
independent of `plan.userdata`, which belongs to dispatch callbacks. Internal
proxy, upload and other route plans cannot replace the application's association.
Callback failure still closes the request once, allowing partial state cleanup.

`admission.on_connection` receives a borrowed `rotmos_connection_info` before
TLS handshake work. ALLOW is the initialized decision; REJECT and CLOSE both
close the connection without an HTTP reply. The snapshot contains the actual
peer/local endpoints, listener index, TLS listener flag and process identity.
It has no forwarded address or HTTP request.

`admission.on_request` runs after validated headers and the start callback,
before body storage, upload callbacks or proxy allocation. It receives a
temporary staged reply builder and an initialized ALLOW decision. REJECT submits
that builder's copied bytes, headers and status; an unselected builder supplies
403. File/source payloads and copied bodies above the output queue bound fail
with a bounded 500. CLOSE sends no HTTP bytes. Callback errors and invalid
decisions also produce 500. The callback composes its decision through the
builder; direct response I/O is unavailable there. Early rejection closes the
connection without consuming an advertised body or issuing 100 Continue.

`callbacks.on_observe` receives borrowed, read-only `rotmos_observation`
snapshots. Events cover connection acceptance, TCP stall, TLS failure, native
HTTP status, managed request start/rejection, final response commitment,
request completion and application names. `rotmos_request_event(request, name)`
emits the last kind with a nonempty name and no control bytes. It borrows the
name synchronously without imposing a new length limit on Vectis event names.
Observers cannot perform request/response
I/O, recursively emit events or retain borrowed handles/views.

Transport-only events have a NULL request, empty effective address/correlation
and no outcome. Unknown status is zero. Request events retain resolved identity
and a valid request. TCP_STALL retains Vectis's classification for timeouts or
connections closed before their first response; the hook does not distinguish
the reason, so its cause remains OK rather than asserting a timeout.
Final commitment fires once, on actual positive final output
or a retained TLS retry; an interim 100/103 does not commit the final response.
Only REQUEST_COMPLETE has `has_outcome`, final monotonic duration and checked
wide counts. Queued output is never reported as transmitted output. Body counts
exclude framing; wire counts include plaintext HTTP headers/framing and interim
blocks, excluding TLS record overhead. `input_counts_known` distinguishes managed
input from rejection before input initialization and raw takeover. Unknown
disconnect side remains UNKNOWN; admission CLOSE identifies HOST and detected
downstream disconnect identifies DOWNSTREAM.

Final response cleanup and the plan's close callback precede the final
observation. `callbacks.on_request_close` then runs exactly once before metadata
expires, including resolver/start/admission failure before dispatch. Application
userdata is still available there. In-flight native sources retain independent
state until acknowledgement and can close after the request; their callbacks
cannot retain a live request handle. Parser failures before managed-request
initialization have transport events instead. Redirect listeners use managed
output for truthful terminal counts and preserve their raw target without a
routing decode. Their decoded path/query collections can be empty.

The [policy fixture](../tests/policy-runtime.c) covers trusted-chain behavior,
copied configuration, resolver failure, bounded admission, rejection before
upload open, independent application/plan state, callback ordering, response
replacement, interim output, post-commit abort and transport stalls/status.
The [TLS fixture](../tests/tls-runtime.c) covers handshake failures and redirect
accounting. The public policy fixture also runs against relocated static/shared
SDKs. [Structured and file access logging](logging.md) use the same resolved
identity without retaining application-owned request state.

## Process-shared autoblock

`config.autoblock` lifts Vectis's fixed-window table. Zero durations/capacity
inherit 1800 seconds, 3600 seconds and 8192 addresses. The existing limits are
16 status rules, 16 named-event rules and 65536 entries. Enabled configuration
allocates the table during run preparation, before workers fork. Construction
and validation do not map a table. Each new run starts empty; HTTP workers and
their replacements share the current run's counts and blocks.

TCP stalls, TLS failures and malformed transport statuses use the real peer.
Request statuses and `rotmos_request_event` use the resolved effective address.
Native 100 Continue also uses that resolved request key. Managed final status
counts once at close, retaining replacement-before-commit and abort semantics.
The engine works without an observation callback. Blocked accepted peers close
before TLS; blocked effective addresses receive a bounded empty 403 before body
storage or route dispatch. Application admission cannot override an existing block.

Reaching a positive threshold blocks the address for the configured duration.
Counters reset when the window elapses and saturate rather than wrapping.
Expiry makes an address available again. New addresses use a vacant entry or
the oldest unblocked entry; a full table never evicts an actively blocked entry.
Time uses monotonic wide milliseconds. As in Vectis, unavailable clock/lock
information allows admission and does not invent a block.

Linux uses a process-shared robust mutex. Recovery after owner death clears the
potentially partial table before marking the mutex consistent. Darwin retains
Vectis's process-local mutex plus an unlinked-file POSIX record lock, released
by the kernel when a worker dies. The parent destroys the mapping after workers
are reaped. There is no per-address heap allocation on the HTTP loop.

`rotmos_server_config_init_production_webserver` enables the mechanism with
Vectis's thresholds: 20 TCP stalls, 10 TLS failures, 10 responses each for
401/403, 50 for 404 and five for 429. It retains the default positive body
ceiling. Routes, authentication, TLS, persistence and application event rules
remain explicit declarations. The [table fixture](../tests/autoblock.c) covers
shared counters, capacity, expiry/window reset, wide duration, saturation,
worker-death recovery and fresh-table reuse. The
[public live fixture](../tests/policy-logs-runtime.c) covers blocking and identity,
including 100 Continue, with no observer installed.
