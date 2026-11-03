# librotmos server API specification

Status: proposed implementation contract, researched on 2026-11-03. No API in
this document is implemented yet. The agreed product boundary is a librotmos
API with Kore as its backend and an explicit backend escape hatch. This document
supersedes the preliminary API direction in [extraction.md](extraction.md).
The [specification audit](server-api-audit.md) records the resolved contract gaps,
source corrections and remaining implementation proof obligations.

The selected product is a reusable server toolkit: public C API, Kore backend,
generic routes, format routes, static serving, reverse proxy, WebDAV, and a
shipped Lua facade/C binding. Vectis keeps its application facade and integrations.
Opaque handles, operation families, lifecycle semantics and defaults below are
the recommended design for implementation; they are not a released ABI commitment.

## 1. Objective and evidence boundary

Provide an independently usable C89 HTTP server library which can replace
Vectis's private Kore object runtime and bridge without losing Vectis's server
features. A normal consumer must not compile Kore, resolve callback names,
manipulate Kore globals, include Kore headers, or link private Kore symbols.

The investigation used:

- librotmos baseline `cfe94d9ee255271708db6da26958f04f3a4553a4`;
- Vectis baseline `6b1e263e0436687f68a7768ca55f4361a6e59e19`;
- pristine Kore `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b` and all
  [42 imported patches](../patches/kore/series);
- Vectis's public C header, native server/HTTP/proxy implementations, Lua
  contracts, runtime documentation, and regression test sources listed below.

Findings describe source behavior. Existing Vectis tests were inspected, not
rerun, and their presence does not establish fresh runtime proof. All acceptance
tests below are requirements for the future implementation.

Vectis's existing implementations and regressions are the extraction baseline.
Reuse their working mechanisms and supported-input contracts through the public
rotmos boundary. Verification of the relocated integration is a cutover task,
not an unresolved capability in Vectis or a reason to require new dependency APIs.

### Findings which determine the API

| Finding | Consequence |
| --- | --- |
| Kore is a private object runtime compiled as GNU99; its bridge also has a GNU99 exception. | A C89 facade alone is insufficient. Port the selected backend and installed headers through external patches. |
| The approximately 3,900-line bridge embeds application state, routing, autoblock, metrics, TLS preparation, control messages and storage. | Split mechanisms from Vectis policy; copying the bridge is not the extraction. |
| Proxy admission occurs after headers and before Kore consumes body or surplus bytes. | Headers-time dispatch and an exclusive, bounded connection handoff are mandatory. |
| Proxy code directly uses accepted-socket TLS, event registration, timers and send-queue state. | Public nonblocking channel and loop interfaces must replace those accesses. |
| A queued response can still be replaced until transmission or a TLS retry makes that unsafe. | Queued, committed and completed responses must be distinct public states. |
| Ordinary Kore bodies use Content-Length. Proxy handoff implements chunk framing and trailers separately. | Do not advertise generic incoming chunked-body support based on the proxy alone. |
| Kore's application WebSocket receiver rejects continuation frames; the proxy relays frames opaquely. | Bounded application frames and transparent proxy tunnels are separate contracts. |
| Vectis normally forks a runtime parent, then starts its supervisor services; Kore forks HTTP and auxiliary workers. | Keep the outer supervisor in Vectis, expose process-domain lifecycle hooks, and require fork quiescence. |
| ACME persistence runs in the key-manager path before certificates are distributed. | A generic persistence callback must preserve ordering and failure propagation without importing Lockd. |
| Vectis already restricts dynamic exports; its export list includes vectis_request_kore, not a public Kore runtime. | Preserve explicit native request access without accidentally publishing all of Kore as another stable ABI. |
| Lua upload/DSV dispatch is registered through the pthread-backed upload_reader and directly accesses the registered Lua state there. | The cutover must marshal Lua work to the owner worker loop; this thread placement is not a contract to copy. |
| The current reader adapter's write/finish paths can block in a condition wait or thread join in the HTTP loop. | Preserve its reader feature with partial consumption, pause/resume and wake delivery, not event-loop blocking. |

## 2. Ownership and scope

| librotmos owns | Vectis owns |
| --- | --- |
| Accepted sockets, HTTP parsing, transport framing and keepalive; generic router, parameter/regex matching and route ordering | Route declarations, application handlers, auth decisions and application composition |
| Worker processes and backend event loops; shipped Lua facade and public-API C binding | Outer supervisor, service graph, host Lua runtime ownership and application readiness policy |
| Request/body transport, explicit materialization, uploads, response delivery and generic JSON/XML route codecs | Application schemas/business logic and unrelated client/format workflows such as DSV |
| TLS termination, client-certificate enforcement and ACME mechanics | Durable ACME storage selection, Lockd/Pouch integration and hydration policy |
| WebSocket/frame transport, raw duplex channels and generic HTTP/SSE/WebSocket reverse-proxy engine | WebSocket application behavior and configured proxy destinations/rewrite/auth decisions |
| Admission/event hooks, generic trusted-proxy/autoblock implementation and transport observations | Selection of rules, application-specific admission events and application metrics/persistence |
| Structured/access-log transport through libpslog | Application logger selection and correlation/effective-IP policy |
| File transport, static mounts, conditional/range handling, WebDAV protocol and filesystem storage provider | Configured roots/quotas/auth, packed-asset provider and Lockd-backed workflow adapters |

librotmos supplies the reusable router and server conveniences described in
section 19. Vectis translates its existing public route/configuration APIs into
that router and removes the corresponding duplicate mechanisms at cutover.
Its application facade remains Vectis-owned. Custom low-level consumers may use
the headers callback and explicitly dispatch a public router themselves; there
must not be two implicit competing route selectors.

The separately packaged Lua module in section 20 has a Lua facade over a C binding
which calls the installed public shared librotmos API. It runs in the host's
existing Lua VM, including an embedded VM; the host creates and closes that VM.

Initial backend scope is the Vectis-selected HTTP/1.0 and HTTP/1.1 runtime,
OpenSSL TLS, ACME, curl integration, LoneJSON and pslog enhancements. HTTP/2 and
HTTP/3 listeners, h2c, native Kore Python/Lua/PGSQL/tasks/JSON-RPC modules, dynamic
Kore modules and the standalone Kore CLI are not initial products. Vectis's
HTTP/2 **upstream client** support moves with the generic curl-backed proxy engine;
it is not an HTTP/2 listener claim. Windows is outside this fork/event backend's
initial target set.

## 3. Public surface and C89 rules

Use the `rotmos_` symbol prefix and these header families:

| Header | Responsibility |
| --- | --- |
| `<rotmos/server.h>` | Errors, server/configuration, lifecycle, observations |
| `<rotmos/http.h>` | Request views, body plans, materialized bodies, responses |
| `<rotmos/io.h>` | Loop watches, timers, deferred work, wakers and channels |
| `<rotmos/tls.h>` | Listener identities, material sources and ACME callbacks |
| `<rotmos/websocket.h>` | Bounded application-frame connections |
| `<rotmos/router.h>` | Route registration, captures, groups/prefixes and dispatch |
| `<rotmos/proxy.h>` | Generic reverse-proxy configuration and public hook views |
| `<rotmos/webdav.h>` | WebDAV mounts, filesystem provider and storage/auth interfaces |
| `<rotmos/xml.h>` | libxml2-backed mapped routes, DOM/SAX and response encoders |
| `<rotmos/rotmos.h>` | Ordinary server/HTTP/router/I/O/TLS/WebSocket/proxy/WebDAV interfaces; dependency-specific codec/interop headers remain explicit |
| `<rotmos/pslog.h>` | Explicit libpslog logger/access-log integration |
| `<rotmos/lonejson.h>` | Explicit LoneJSON helpers over materialized bodies |
| `<rotmos/backend/kore.h>` | Opt-in backend identity and native request access |
| `<rotmos/lua.h>` | Binding-owned embedding/interop declarations; excluded from ordinary C umbrella headers |

Core headers expose opaque handles and small value/configuration structs. They
must stand alone in C89 and C++ consumers. Do not expose Kore, OpenSSL, curl,
Lockd, pthread, epoll/kqueue or Lua layouts in core declarations. The dependency
integration headers may include the corresponding upstream public header;
they must use its types rather than locally mirroring them.

No flexible arrays, anonymous unions, variably modified types, designated
initializers, C99 inline, long long, or required `<stdint.h>` in owned public
declarations. Use `size_t` for chunk sizes and in-memory capacities, `unsigned
short` for ports, and `long` for bounded millisecond durations. Reject negative
durations. Configuration initialization functions are authoritative; arbitrary
memset of pointer-containing structs is not the portable initialization contract.

For cumulative byte counts, content lengths and file offsets use a C89 value:

```c
typedef struct rotmos_u64 {
    unsigned long high;
    unsigned long low;
} rotmos_u64;
```

Each member represents a 32-bit word, including on LP64. Values above
`0xffffffffUL` in either word are invalid. Supply checked construction from
`size_t`, comparison, addition and conversion back to `size_t`; overflow returns
an error. This is a numeric API value, not a serialized or packed representation.
It prevents a 32-bit consumer's chunk size from becoming a 4 GiB stream limit.
Private OS/dependency integer types remain implementation details.

Do not add ABI-version fields, reserved method-pointer slots, parallel legacy
APIs or compatibility shims merely to speculate about pre-1.0 changes. A future
released shared-library ABI break follows the project's independent ABI policy.

### Operation index

These names define the initial families; signatures shown elsewhere constrain
the asynchronous operations. Getters return immutable borrowed views unless
explicitly named copy/take. Config, plan and error types each have an initializer.

| Operations | Contract |
| --- | --- |
| `rotmos_error_clear`, `rotmos_status_string` | Bounded diagnostic reset and static status description |
| `rotmos_u64_from_size`, `rotmos_u64_from_words`, `rotmos_u64_compare`, `rotmos_u64_add`, `rotmos_u64_to_size` | Checked C89 wide counters; no truncating casts |
| `rotmos_listener_config_init`, `rotmos_tls_identity_init`, `rotmos_request_plan_init` | Explicit initial states, including no implicit TLS identity/body |
| `rotmos_request_method`, `rotmos_request_method_token`, `rotmos_request_http_version` | Parsed method/version and original method spelling |
| `rotmos_request_target`, `rotmos_request_path`, `rotmos_request_query`, `rotmos_request_authority` | Raw, un-decoded length-delimited views |
| `rotmos_request_header_count`, `rotmos_request_header_at`, `rotmos_request_header_first` | Ordered duplicate-preserving enumeration and case-insensitive lookup; missing is distinct from empty |
| `rotmos_request_peer`, `rotmos_request_local`, `rotmos_request_tls_info`, `rotmos_request_framing` | Immutable transport/framing snapshots |
| `rotmos_request_loop`, `rotmos_request_response` | Borrowed owning-loop/response handles |
| `rotmos_request_effective_address`, `rotmos_request_correlation_id` | Resolver results copied into request lifetime storage |
| `rotmos_request_body_info`, `rotmos_request_body_read`, `rotmos_request_body_rewind` | Materialized-kind/length, bounded reads and explicit rewind; live streams reject these operations |
| `rotmos_response_state`, `rotmos_response_is_committed`, `rotmos_response_queued_bytes` | Drain state, irreversible commitment latch and bounded queue occupancy |
| `rotmos_response_bytes` | Bounded copied complete reply (status/type/body), automatically begins and ends on success; cannot replace an existing reply |
| `rotmos_response_file_take`, `rotmos_response_producer`, `rotmos_response_producer_resume` | Explicit descriptor/producer ownership, pause/resume and close callback as defined in section 9 |
| `rotmos_channel_body_complete`, `rotmos_channel_upgrade`, `rotmos_channel_queued_bytes`, `rotmos_channel_shutdown_write`, `rotmos_channel_close` | Complete the takeover family in section 10; close schedules terminal cleanup |
| `rotmos_loop_now`, `rotmos_waker_new`, `rotmos_waker_signal`, `rotmos_waker_destroy` | Monotonic clock and bounded coalescing wake family |
| `rotmos_websocket_accept`, `rotmos_websocket_send`, `rotmos_websocket_resume`, `rotmos_websocket_close` | Application-frame family with separate connection lifetime |
| `rotmos_server_supervision_take`, `rotmos_server_control_interest` | Replace a consumed supervision declaration before a run; update control events on its running parent loop |
| `rotmos_server_set_pslog` | Inert logger/access policy registration from the optional header, allowed only outside a run |
| `rotmos_request_json_validate_materialized`, `rotmos_request_json_parse_materialized` | Optional LoneJSON validation/map decode; no live-body streaming implication |
| `rotmos_server_process_context` | Public parent/HTTP/key-manager/ACME role and process/worker identity snapshot for real binding/lifecycle consumers |

Factories, writes, state changes and indexed lookups use status plus output
parameters when failure/absence is meaningful. No new allocation-returning
getter hides a cleanup obligation. A direct immutable snapshot getter can return
a borrowed pointer; it must not require a callback to retain private backend data.

`rotmos_bytes` is a borrowed `(const void *data, size_t length)` view; a NULL data
pointer is valid only at zero length. `rotmos_header` contains name and value
views with the same lifetime rule. Text fields validate their own grammar and
reject embedded NUL; body views remain binary. An empty view is not an absence
indicator: lookup operations return a separate found flag. HTTP methods and
statuses use enums/integers, never an untyped pointer convention.

## 4. Common status, ownership and execution contract

`rotmos_status` has `ROTMOS_OK == 0`; distinct nonzero results are `ROTMOS_AGAIN`,
`ROTMOS_EOF`, `ROTMOS_INVALID`, `ROTMOS_NOMEM`, `ROTMOS_STATE`, `ROTMOS_CONFLICT`,
`ROTMOS_LIMIT`, `ROTMOS_OVERFLOW`, `ROTMOS_TIMEOUT`, `ROTMOS_IO`,
`ROTMOS_NOT_SUPPORTED`, `ROTMOS_CANCELLED` and `ROTMOS_BACKEND`.
`AGAIN` and `EOF` are I/O outcomes, not generic callback failures.

`rotmos_error` is a caller-owned bounded value containing status, source
category, native/dependency code, operation, message and detail. It requires no
cleanup. Optional error outputs may be NULL. Success clears a supplied error;
failed factories leave output handles NULL. Errors must not disclose keys,
credentials or body contents. HTTP status is selected explicitly by the adapter,
not inferred from every library error.
AGAIN/EOF clear diagnostic error fields; neither should generate an error log by
itself. I/O count outputs are zero on those outcomes unless an operation explicitly
documents a successfully accepted prefix.

| Input/resource | Ownership |
| --- | --- |
| Configuration structs, strings, domain lists, TLS memory and rule arrays | Copied when accepted; later caller edits have no effect |
| Callback functions, callback userdata, configured pslog logger | Borrowed for the documented server/process lifetime |
| Request handle, metadata, materialized body and response handle | Runtime-owned until the request's terminal callback |
| Body/frame callback bytes | Borrowed only during that callback |
| Bytes accepted by a response/channel write | Copied into a bounded owned queue; caller may immediately reuse the accepted prefix |
| File-response descriptor passed to a take operation | Transferred only on success; closed once on completion/cancellation |
| Channel returned by takeover | Runtime-owned exclusive I/O capability, not an independently closed socket |
| Application request state | Application-owned; exactly one terminal callback releases it |

Every request belongs to one HTTP worker and one loop. Except explicitly marked
wake/stop functions, operations require that owning process/thread. Handles are
not portable across fork boundaries, threads or server runs. Borrowed handles may
be kept in owner-loop request state until their documented terminal callback;
they cannot outlive it. There is no general request refcount or cross-thread
response mutation API.

Server pointers deliberately supplied to post-fork hooks identify the newly
initialized process-local runtime context. That exception does not make captured
parent loop/watch/waker handles usable in a child.

Public writes, cancellation and close operations must not invoke terminal user
callbacks recursively before returning. Retire objects after the active event
batch; a flush must not unexpectedly free a consumer's active callback state.
Ordinary callbacks must return promptly. A blocking source, parser or application
operation must not be disguised as nonblocking transport work.

Once-only cleanup applies to orderly success, cancellation and recoverable
failure. SIGKILL or a crashed child cannot execute its destructors; the parent
reports that abandonment and reaps the process. Do not claim child callbacks ran
when only a parent death observation was possible.

## 5. Server construction and lifecycle

The principal operations have these shapes; referenced config/value types are
specified in the following sections:

```c
void rotmos_server_config_init(rotmos_server_config *config);
rotmos_status rotmos_server_new(const rotmos_server_config *config,
    rotmos_server **out, rotmos_error *error);
rotmos_status rotmos_server_validate(rotmos_server *server,
    rotmos_error *error);
rotmos_status rotmos_server_effective_config(const rotmos_server *server,
    rotmos_server_effective *out, rotmos_error *error);
rotmos_status rotmos_server_run(rotmos_server *server,
    rotmos_run_result *out, rotmos_error *error);
rotmos_status rotmos_server_request_stop(rotmos_server *server,
    rotmos_error *error);
void rotmos_server_notify_signal(rotmos_server *server, int signal_number);
rotmos_status rotmos_server_set_serving(rotmos_server *server, int serving,
    rotmos_error *error);
rotmos_status rotmos_server_destroy(rotmos_server *server,
    rotmos_error *error);
```

`new` copies declarations and allocates inert state. It does not bind, fork,
start a thread, contact an ACME service, install signals or change process state.
`validate` checks values, backend support, TLS material, referenced directories
and file access, with bounded preparation and complete cleanup. It is a preflight,
not proof that a later bind or file open will succeed. It does not issue network
requests or modify durable ACME state.

`run` executes the backend parent on the calling thread and forks its HTTP and,
when selected, auxiliary workers. It creates no additional outer supervisor and
no hidden server thread. Bind actual sockets during startup and retain them;
do not treat a temporary bind probe as a race-free reservation. Startup errors
after validation still return a structured error. Port zero is allowed for test
and embedding listeners; the bound port is reported at readiness. Redirects
require an explicit usable presented HTTPS port if their destination is ephemeral.

The first Kore backend supports one running server per process. Multiple inert
objects are allowed; a second concurrent run returns `CONFLICT` before changing
the first. Do not simulate independent instances over process-global Kore state.

| State | Allowed behavior |
| --- | --- |
| DECLARED | Configure integration hooks, validate, inspect effective settings, run or destroy |
| STARTING | Acquire resources and run ordered process hooks; application requests are not served |
| READY_PAUSED | Listeners and required workers are initialized; application requests receive the readiness 503 policy |
| SERVING | Dispatch requests normally |
| STOPPING | Stop intake, cancel/drain owned work within the configured deadline, reap owned children |
| STOPPED / FAILED | No live runtime resources; inspect run result, validate/run again, or destroy |

`start_paused == 0` enters SERVING when initialization completes. Vectis uses
`start_paused = 1`: its existing outer supervisor can start services after runtime
readiness, then release the application gate. The parent propagates gate changes
to HTTP workers; an observation acknowledges the effective gate after all live
workers apply it. A replacement worker inherits the current gate, never a stale
initial value. Request bodies and proxy admission honor the gate too.

Repeated stop requests are idempotent. `request_stop` is thread-safe within the
running process; it wakes the parent loop and never waits for its own callback.
It is not async-signal-safe. `notify_signal` is the separate async-signal-safe
notification path during a run, with no allocation, locks or user callbacks.
The caller must keep the server alive while a signal handler can reference it.

Default signal mode is HOST: the caller forwards relevant child/termination
signals. Explicit LIBRARY mode installs/restores its own run-scoped handlers.
In both modes preserve the first termination reason across later SIGCHLD, never
reap unrelated children with an unqualified waitpid(-1), and restore borrowed
process state. `rotmos_run_result` distinguishes requested stop, terminating
signal, startup failure and backend/worker failure and records whether escalation
was required. A normal stop is not reported as a backend error.

Destroying a running object returns `STATE`; it does not perform a hidden blocking
shutdown. Destroying NULL is successful. Restart the same object only after `run`
returns; a new run has no stale timers, callbacks, domains, keys, loggers or queues.
The immutable copied declaration survives a run; consumed control descriptors do
not. `rotmos_server_supervision_take` installs a fresh supervision declaration in
DECLARED/STOPPED/FAILED, transferring its FD only on success. A supervised rerun
without a fresh control FD fails STATE. No stale descriptor number is reused.

### Process-domain hooks and supervision

Provide typed hook slots, not string-to-function or void-pointer symbol tables:

| Hook | Context and order |
| --- | --- |
| `on_process_start` | Public role/context for the runtime parent and each owned child, after its relevant fork and before role-specific/user callbacks; initialize process-local binding state here |
| `on_parent_prepare` | Runtime parent, before any backend fork; inert application declarations only |
| `on_parent_ready` | Parent, after bind and initialization acknowledgements from required workers; includes actual listener addresses |
| `on_worker_start` | Each HTTP worker after its final fork and before accepting requests; create worker clients/resources here |
| `on_worker_stopping` | Worker before teardown; request cancellation without blocking, then retire post-fork application work during bounded drain |
| `on_worker_exit` | Worker after backend teardown; no request, channel, timer, backend allocator or TLS access; returns child exit status |
| `on_worker_event` | Parent observation of owned child start, exit, restart and escalation; distinguishes HTTP/key-manager/ACME roles |
| `on_parent_stopping` / `on_parent_stopped` | Parent; stop intake first, then release parent resources after child reaping |

Startup hook errors propagate to the parent and prevent readiness. Cleanup runs
for every hook whose initialization started, including partial failure. A worker
restarted under RESTART receives a fresh start/stop lifecycle. The parent remains
thread-quiescent while future worker replacement forks are possible. Worker
threads may be created only after that worker's last fork. The library cannot
prove arbitrary application quiescence; document it as a precondition and do not
add thread/process scanners.

Worker stopping keeps wake/completion delivery available while accepted work
drains or acknowledges cancellation. Join only threads already known to have
finished; never join a producer waiting for this same loop. Application-owned
watch/waker registrations must retire before orderly loop teardown. At the
shutdown deadline the parent escalates the child and reports abandonment rather
than promising that unfinished native jobs or their destructors completed.

Thread-bearing parent maintenance belongs in Vectis's outer supervisor or in an
appropriate post-fork worker, not inside a parent hook which will later fork a
replacement worker. Parent-loop hooks may use the existing supervisor channel
to request that work without importing Vectis's service machinery.

Optional `rotmos_supervision_config` has `enabled = 0`, `control_fd`, a parent-loop
event callback and userdata. When enabled, successful `new` transfers the supplied
descriptor; failed construction leaves it with the caller. The runtime parent
owns its nonblocking I/O; every auxiliary/HTTP child closes its inherited copy
before application hooks. EOF/error causes controlled stop. Readable bytes are
passed to the host callback without inventing a librotmos application protocol;
Vectis keeps its READY/STOP/service-failure framing. Read/write-interest updates
and partial messages are explicit; the backend does not assume one read is one
message. Detect EOF after draining readable bytes. No periodic control-channel
polling or parent-PID-only lifetime supervision replaces this owned channel.

Startup has a finite `startup_timeout_ms` (default 30000), covering required
worker acknowledgements and transition to ready. Failure stops intake, tears down
partial initialization and reaps owned children within the shutdown bound.
Graceful shutdown stops new requests, asks active work to finish/cancel, then
escalates at the configured bound; forced child death is reported as abandonment.
No deadline can preempt arbitrary synchronous host C code safely: hooks must
honor their documented execution budget. A host which blocks a parent hook
violates the lifecycle precondition rather than receiving a promised hard timeout.

## 6. Configuration and defaults

Use focused `workers`, `limits`, `timeouts`, `http`, `tls`, `callbacks`,
`supervision` and `io` value structs within the server declaration. Listeners and
TLS identities are arrays of copied records. There is no implicit network
listener: at least one must be declared before run. The required initial shapes
are a primary HTTP/HTTPS listener and an optional cleartext redirect listener;
all listeners share this server's worker pool and process-scoped backend limits.

The following proposed defaults preserve Vectis's effective transport baseline
unless explicitly noted. The Vectis adapter passes effective settings rather
than relying on future librotmos defaults.

| Vectis setting | librotmos requirement / default |
| --- | --- |
| `max_connections` | Per-worker connection limit; 1024 |
| `request_limit` | Active request objects per worker; zero resolves to max_connections |
| `worker_count` | Zero resolves automatically; explicit HTTP workers 1..253, excluding auxiliary slots |
| `worker_accept_threshold` | Per-loop intake budget; 16 |
| `worker_rlimit_nofiles` | Per-worker descriptor rlimit; 768; report OS refusal, do not silently clamp |
| `worker_set_affinity_disabled` | False; platform affinity policy remains enabled where supported |
| `worker_shutdown_timeout_ms` | Bounded child shutdown; 5000 ms |
| `worker_death_policy` | RESTART = 0; TERMINATE stops the runtime on unexpected child death |
| `max_request_header_bytes` | 65536 bytes, including terminator; independently expose the effective field-count bound |
| `max_request_body_bytes` | Separate default managed-body limit (10 MiB) and optional hard server ceiling; hard ceiling zero means route plans decide |
| `request_body_spool_dir` | Explicit path required when a materialized-body plan permits disk spill; no hidden choice of a Vectis/XDG directory |
| `request_header_timeout_ms` | 5000 ms |
| `request_body_idle_timeout_ms` | 15000 ms of peer receive inactivity |
| `response_write_idle_timeout_ms` | 15000 ms without actual downstream progress |
| `request_body_min_rate_bytes_per_sec` | 1024 bytes/s for managed incoming bodies |
| `request_body_min_rate_grace_ms` | 10000 ms |
| `idle_timeout_ms` | 30000 ms connection idle policy |
| `keepalive_disabled` | False |
| `keepalive_timeout_ms` | 5000 ms |
| `keepalive_max_requests` | 100 |
| `kore_curl_timeout_seconds` | ACME/backend-workflow request timeout; zero retains the compiled backend default |
| `kore_curl_recv_max_bytes` | ACME/backend-workflow response bound; zero retains the compiled backend default |
| `kore_quiet` | Backend lifecycle chatter suppression, false; separate from app and access-log levels |
| `socket_backlog` | 5000 |
| `request_process_budget_ms` | 100 ms per request-processing loop budget, not a wall-clock timeout on a handler |
| `hsts_max_age_seconds` | Zero disables HSTS; apply only to appropriate TLS responses |
| `websocket_max_frame_bytes` | 16384 bytes for application frames; not a proxy-tunnel message limit |
| `websocket_timeout_ms` | 120000 ms |
| `server_header` | Nonempty copied value; librotmos default `rotmos`, Vectis explicitly passes `vectis` |
| `access_log_path` | NULL disables the independent file sink; preflight append access |
| `access_log` | pslog sink enabled by default, trace/warn/error categories and up to 32 exact-status overrides |
| `pretty_error_pages` | False |
| `client_ip` | Generic trusted-proxy resolution in rotmos; Vectis passes selected trust settings and can supply a resolver |
| `autoblock` | Generic process-shared rule engine in rotmos; Vectis selects rules and emits application events through public APIs |

`io.chunk_bytes` defaults to 8192 and queue/memory budgets to 262144 bytes per
request direction. Those are heap allocations, not large automatic arrays.
Bounds must include headers, held input remainder, queued writes and deferred
callbacks. Disk materialization additionally requires a finite body/disk budget.
The imported parser uses HTTP_REQ_HEADER_MAX = 25 split slots and rejects 25
nonempty lines including the request line: its current effective maximum is
23 header fields, including Host. Report that independent limit explicitly and
test both sides of it; a 64 KiB byte allowance does not promise unlimited fields.

Configuration zero normally requests the initialized default; zero explicitly
disables HSTS and optional hard ceilings, selects automatic workers, and represents
false booleans. A disabled deadline uses its named disable field, not an
ambiguous negative number. The effective view must contain resolved values,
units, supported capabilities and backend limits. Reject unsupported values,
overflow, unknown enums and unsafe text rather than silently accepting or
truncating them.

The Kore backend currently stores header, body-idle and keepalive timeouts in
whole seconds. For this backend, round positive requested values up to whole
seconds and expose the actual milliseconds in the effective view, preserving the
existing bridge's behavior. Reject a rounded value over 65535 seconds rather
than silently saturating it. Disabled means bypassing the check, not converting
it into a one-second deadline. Other millisecond settings retain their declared
units; scheduler lateness does not change the configured duration.

Vectis currently derives a native body ceiling from the largest declared route
when its server-wide body limit is zero; an explicit positive server limit is a
ceiling. Its live-upload route default is 3 GiB and its route memory threshold
256 KiB. Preserve these decisions in its adapter. Do not turn the generic
10 MiB default into an unconditional ceiling on every upload or proxy route.

Every configurable deadline is represented by an initialized duration plus a
named disabled flag. The worker shutdown bound cannot be disabled. A zero
minimum-rate value requests the default; a separate rate-disabled flag is the
explicit opt-out. The effective view must distinguish inherited, explicitly
disabled and active settings without forcing callers to reverse-engineer
sentinel arithmetic.

An unanswered managed request has a finite `deferred_response_timeout_ms`
(default 30000) after successful body completion. This covers the otherwise
unbounded wait before response begin; response write-idle timing covers started
responses, including a paused producer. Application receive pauses do not freeze
these deadlines. Startup and deferred-response bounds cannot be disabled in this
initial contract. Proxy-owned progress/total deadlines are separately specified
in section 19, with the response-start bound still applying.

## 7. Request dispatch and metadata

The server defaults to ROUTER dispatch through its attached public router.
Explicit CALLBACK mode instead requires the following callback and never
silently invokes a second router afterward. A custom dispatcher can call
`rotmos_router_dispatch_headers` itself. The callback is:

```c
typedef rotmos_status (*rotmos_headers_fn)(rotmos_request *request,
    rotmos_request_plan *plan, void *userdata, rotmos_error *error);
```

Call it once after validating and selecting the listener/host, before consuming
body bytes, emitting 100 Continue, storing a body, allocating large exchange
buffers, or forwarding to an upstream. It must see already coalesced input without
losing or pre-consuming it. The backend retains surplus unless a successful
takeover transfers its logical ownership.

Initialize the output plan before entry. It contains body mode/limits, typed
body/completion/write-ready/terminal callbacks and per-request userdata. The
callback chooses exactly one disposition: managed body delivery, an immediate
finished response, application WebSocket upgrade, or connection takeover. An
unanswered NONE-body plan invokes its completion callback for an empty body;
it does not silently invent an application response. A missing required callback
or conflicting disposition is `INVALID`.

`plan.disposition` is MANAGED = 0, REPLY, WEBSOCKET or TAKEOVER. REPLY requires
a successfully ended final response; WEBSOCKET/TAKEOVER require successful
installation of that capability during dispatch. MANAGED requires an on_body_end
callback unless a producer/response already owns completion. Body mode NONE
still delivers an empty completion event; it never means "free the request now".
The plan is copied on successful dispatch; pointers to the plan itself do not
escape the callback. Its `on_body_end` slot has type `rotmos_complete_fn` below.

After dispatch has started, the plan's terminal callback runs exactly once even
if dispatch returns an error after assigning userdata. Callback failures before
commit produce a bounded 500 response unless the adapter already selected a
valid error reply; after commit they abort the exchange. Parser rejects which
precede dispatch use connection-level observations instead.

Request accessors provide:

- method enum and raw method token, HTTP version, listener identity;
- immutable raw origin-form target, raw path, raw query and Host/authority;
- header count and indexed `(name,value)` byte views in arrival order, preserving
  duplicates and Host even if Kore stores Host separately;
- case-insensitive first-value lookup with a defined missing result;
- TCP peer/local address and port, TLS state and verified client-certificate
  presence; optional copied certificate views via the TLS interface;
- declared Content-Length as `rotmos_u64` with a separate presence flag,
  transfer-coding and Expect/Upgrade metadata without conflating absent and zero;
- application/effective client address and correlation identifier set through
  configured resolvers, separately from the unmodified TCP peer;
- owner loop, response handle and caller request-state association.

Use pointer-plus-length views; do not require body bytes to be NUL terminated.
Metadata views last until the request terminal callback, not merely headers
dispatch. The generic router owns validated path decoding, proxy fallback,
repeated query pairs, and static-route/parameter/regex precedence. Vectis passes
its declarations through that public contract instead of keeping a second copy.
Never rebuild a proxy target from decoded parameters or normalize percent
escapes in the transport accessor.

The plan callback types are `rotmos_body_fn` (section 8), `rotmos_complete_fn`
(request, userdata, error -> status), `rotmos_writable_fn` (request, userdata ->
void), and `rotmos_request_close_fn` (request, const outcome, userdata -> void).
Completion is body completion, not proof that a response drained. Write-ready
callbacks may write/end/abort but must not block. Close observes still-valid
metadata and cannot restart I/O on the terminated request. If a close callback
is omitted, the library still performs its own cleanup; dynamically allocated
application state requires an explicit owner/destructor.

A managed request remains live after body completion while its final response
is pending or draining. Returning from a handler does not implicitly end that
response. Normal terminal cleanup waits for successful body completion, final
response drain and release of attached producers/files/channels. Disconnect,
timeout, abort or shutdown cancels remaining work and yields one terminal
outcome. An early reply with an unread body closes after draining the reply;
it cannot authorize keepalive by pretending the input completed. The deferred
response deadline bounds a handler which never starts its reply.
For that deliberate early reply the outcome reports the selected response and
unconsumed/cancelled input separately; a failed peer body is never relabelled EOF.

Terminal order is: stop further delivery/readiness callbacks, retire attached
I/O and run their close callbacks, invoke the request close callback with valid
metadata/outcome, then invalidate borrowed request/response views. No later
callback can access that request. Delayed application work must cancel its own
timer/watch registration during close; merely saving a request pointer does not
extend its lifetime. A successful WebSocket upgrade instead transfers ownership
to the separate connection lifetime in section 13.

Before dispatch, reject ambiguous Host or message framing rather than allowing
two layers to disagree: duplicate Host, conflicting/repeated Content-Length,
or Transfer-Encoding combined with Content-Length. Valid chunked transfer coding
must still reach the takeover decision. These are proposed boundary validation
requirements, not a claim that the imported native parser already performs
every check; compare ordinary and proxy wire tests during the port. Do not expand
supported transfer codings or normalize raw routing data as a side effect.

## 8. Body modes, limits and streaming

| Plan mode | Observable behavior |
| --- | --- |
| NONE = 0 | No body is accepted for the selected application route |
| BUFFERED | Materialize in memory under an explicit finite memory/body bound |
| MATERIALIZED | Materialize with a finite memory threshold and explicitly allowed disk spill; expose memory/file result and cleanup |
| STREAM | Deliver incremental chunks with bounded retention and pause/resume; never collect or spool the whole body |

A zero plan body limit inherits the configured default. Nonzero plan limits may
exceed that default but not a positive hard server ceiling. Validate declared
length before starting application body work and enforce actual received length
as well. Reject oversize/unwanted managed bodies with 413 before final commitment;
after commitment abort the exchange instead of emitting a second response.
Close when unread bytes prevent safe connection reuse. Empty bodies still receive
completion and cleanup exactly once. Timeout, truncation and disconnect are not
successful body EOF.

The streaming callback shape is:

```c
typedef rotmos_status (*rotmos_body_fn)(rotmos_request *request,
    const void *data, size_t length, size_t *consumed,
    void *userdata, rotmos_error *error);
rotmos_status rotmos_request_body_resume(rotmos_request *request,
    rotmos_error *error);
```

On `OK`, consume only `*consumed <= length`. A short consumption pauses reads;
the backend retains at most its bounded remainder and redelivers it before new
bytes. `AGAIN` requires zero consumption and pauses without spinning. Resume is
an owner-loop operation; a producer on another thread uses a waker. Other
statuses terminate delivery. There is no automatic retry of application side
effects. `on_body_end` occurs once after actual successful completion; it never
substitutes for `on_close` on failure.

Peer body-idle/minimum-rate accounting excludes time in an application-requested
receive pause. It must not disable independent total/deadline policy supplied by
the application. Slow consumers cannot create an unbounded queue.

Materialized bodies expose kind, length, borrowed memory or borrowed file/path,
and bounded read/rewind operations. Memory-only access to a spilled body reports
its actual kind; no hidden read-all or second spool. Spool files are private,
created only in the declared directory and removed once after request cleanup.
Materialized/rewind operations on a live STREAM request return `STATE`.

The initial managed Kore path retains its Content-Length framing contract.
Chunked bodies and trailers required by proxy routes use takeover and the
incremental framer migrated into rotmos. Do not send such a request through ordinary
buffering as a fallback. Preserve Vectis's observed missing-length behavior in
the adapter: POST/PUT/PATCH require a length on the managed path; DELETE,
PROPFIND and MKCOL may complete with no body when length is absent. GET/HEAD/
OPTIONS/COPY/MOVE's current managed handling must be covered by explicit wire
tests before any change to body acceptance. All eleven existing methods remain
available; unknown methods must not alias an ANY application route.

The adapter must retain Vectis's live blocking-reader feature without exposing
`lc_source` in librotmos. Vectis owns its bounded producer/reader bridge and
post-fork reader thread. That thread may access a copied/materialized adapter
view or its bounded reader, not owner-loop rotmos handles; it wakes the worker to
resume and publish results. Lua is never entered from the foreign thread.

An explicit finite BUFFERED limit must fit the materialization memory budget;
otherwise reject the plan rather than allocate beyond that budget. MATERIALIZED
can cross its memory threshold only when a spool directory and finite disk/body
budget were supplied. STREAM never materializes the transport body. A typed
codec may explicitly materialize a record under its separate declared budget;
an item-stream codec must not collect the complete result. The per-worker
resource model must account for concurrent header storage, body materialization
where selected, both direction queues and TLS
buffers. Bounded owned queues alone do not prove Vectis's existing conditional
256 MiB proxy-worker planning target; repeat its actual slow-peer measurements.

Specifically, `vectis_lua_app_upload` and `vectis_lua_app_dsv` register their
dispatch functions through `app->upload_reader`; `vectis_upload_reader_open`
starts `vectis_upload_reader_thread_main`, which directly invokes that handler.
The dispatch functions use `route->lua` and Lua stack/callback operations rather
than posting back to the HTTP loop. Separately, `vectis_upload_reader_write`
waits when its queue is full, and finish waits for the handler. These are
source-observed migration gaps, not a reproduced race/crash report. The new
adapter must keep Lua work on its owner loop, move only suitable native parsing
to reader threads, and return a pause to librotmos when capacity is exhausted.
Record callback thread identity and concurrent-request progress in its tests.

## 9. Response construction, framing and commitment

Each request has a borrowed `rotmos_response`. The principal operations are
`rotmos_response_status`, `rotmos_response_header_set`,
`rotmos_response_header_add`, `rotmos_response_begin`, `rotmos_response_write`,
`rotmos_response_trailers_enable`, `rotmos_response_end`,
`rotmos_response_replace_uncommitted`, and
`rotmos_response_abort`.

Header operations copy validated text. `set` replaces matching fields;
`add` preserves order and duplicates, including multiple Set-Cookie fields.
Reject CR/LF injection and invalid names. The response layer owns framing and
connection fields. An application length is set through a dedicated known-length
option, not by conflicting Transfer-Encoding/Content-Length strings.

```c
rotmos_status rotmos_response_write(rotmos_response *response,
    const void *data, size_t length, size_t *accepted, rotmos_error *error);
rotmos_status rotmos_response_end(rotmos_response *response,
    const rotmos_header *trailers, size_t trailer_count, rotmos_error *error);
```

Write accepts a bounded prefix and reports it explicitly. On `AGAIN`, accepted
is zero; await `on_writable` instead of looping. Readiness is rearmed only when
capacity returns. Producers stop generating when downstream capacity is exhausted.
Body-bearing known-length responses require exactly that many accepted body
bytes before end; underflow/overflow fails and prevents unsafe keepalive.
Unknown-length HTTP/1.1
uses chunk framing; HTTP/1.0 uses close delimitation. Trailers require supported
framing and validation; they cannot become arbitrary framing/header overrides.

| Response drain state | Meaning |
| --- | --- |
| BUILDING | Status and headers can change |
| ACTIVE | Begin accepted; bounded output may be queued or in flight |
| FINISHING | End requested; queued data/trailers still draining |
| COMPLETE / ABORTED | One terminal outcome; no further writes |

Commitment is an independent monotonic latch, queried with
`rotmos_response_is_committed`: final-response bytes reached the transport, or a
backend/TLS retry makes replacement unsafe. ACTIVE and FINISHING may each be
uncommitted or committed; starting drain must not erase commitment information.
The latch never clears on an accepted response; replacement is possible only
before it was set. Queue occupancy is independently observable.

`begin` queues validated headers; it does not claim delivery. The backend reports
commitment truthfully even for SSL_write which returns a retry without a public
positive byte count. `replace_uncommitted` is all-or-nothing: retract only an
entirely untouched response with no active retry/stream buffer; otherwise return
`STATE` and leave it intact. This preserves Vectis's queued-200-to-502 recovery.
After commitment an upstream/producer failure aborts; do not append a second
status line or a successful final chunk.

Provide bounded `rotmos_response_bytes` convenience and explicit file/source
producers. A file take operation owns the descriptor on success, supports checked
offset/length and zero-length files, and uses a pinned open file rather than a
later path lookup. Generic static/WebDAV helpers own cache, path, range and
conditional mechanisms; Vectis supplies roots, asset providers and app policy.
Preserve explicit content type across file-cache hits and TLS/non-TLS paths.
Checked offsets must also fit the target's actual file/seek representation.
Prove sparse-file offsets beyond 4 GiB on 32-bit targets; wide public counters
alone are insufficient. An unsupported file range fails before taking the FD.

A source producer is a nonblocking pull callback into a supplied bounded buffer
returning bytes, EOF, AGAIN or error, plus one close callback. Do not reset or
rewind a live source implicitly. Its close callback runs once on success,
replacement, abort, HEAD/bodyless response or startup failure after transfer of
ownership. Blocking file/source adapters and materializing helpers must be named
and documented accordingly; they cannot hide a full-value staging operation
behind a streaming name. Vectis's existing materializing `response_source` and
live `response_stream_source` remain distinct application operations.

The producer callback receives `(buffer, capacity, produced, userdata, error)`.
OK requires `0 < produced <= capacity`; zero-progress OK is invalid. AGAIN and
EOF produce zero bytes; EOF is the sole successful end indication. Adapters for
providers reporting data and EOF together first return the data, then EOF on the
next pull. Errors do not expose a successful prefix. All counters are checked
before queuing; overproduction/underproduction against a known length fails.

AGAIN pauses pulls until `rotmos_response_producer_resume(response, error)` on
the owning loop. Resume schedules a later pull, never recursively invokes it;
repeated resumes coalesce and no pull runs without output capacity. A thread
signals an owner-loop waker to request resume. Downstream writable events alone
do not poll an application-paused producer. Once the library accepts the producer,
it owns exactly one close notification even if begin subsequently fails. Failed
registration leaves ownership with the caller. Manual writes/end and an active
producer are exclusive; EOF ends the producer response using its copied trailers.

Ordinary final status is 200..599. Status 101 requires a validated upgrade;
100/103 use explicit interim operations, never the final-response setter. Reject
other informational responses as unsupported in the initial API.
For HEAD and 204/205/304, suppress forbidden payload generation. Preserve
HEAD/304 representation length where supplied, omit an invented zero length for
HEAD/304/204, and preserve the existing 205 zero-length keepalive behavior.
Application-generated 101 is reserved for a validated upgrade. Bounded explicit
100/103 interim responses do not commit a later final response; accounting and
replacement must distinguish the two. Enforce an interim-response budget.
For a bodyless reply, a supplied representation length does not require matching
accepted payload bytes. Explicit body writes return STATE with zero accepted;
bytes/file/producer conveniences suppress payload copies/pulls and still release
transferred owners once. Wire/body counts report actual transmission, not the
representation length. The 205 response uses zero length regardless of a source.

Response completion and observations report the final selected/committed status,
wire/body counts, success/abort cause and timing. Replaced queued statuses must
not produce duplicate request counts or a false successful completion.

`response_end` accepting end-of-stream is idempotent while draining; it must not
enqueue the same trailers twice. If trailer capacity is unavailable, AGAIN
accepts nothing and the caller retries with the same trailer set after writable
notification. Trailer support must be enabled before begin; HTTP/1.0, known-length or
bodyless responses reject incompatible declarations before committing headers.
Once end is accepted, later end calls must use the identical trailer set or
return STATE. `rotmos_response_trailers_enable` optionally copies known names
for the generated Trailer field. Valid bounded trailers with names not known
before begin remain supported for proxy relays; enabling support does not
permit forbidden framing/authentication overrides. Reject any header/trailer
set which can never fit its applicable
finite bound with LIMIT, rather than returning an AGAIN that cannot become
writable. Producer EOF retains pending end/trailers until accepted without
pulling the producer again. A failed complete-reply convenience operation leaves
no partially installed reply. Known-length writes reject an overflowing offered
chunk before accepting any of that chunk; a short end is a terminal error.

When adapting an already materialized Vectis response larger than the queue
budget, retain/move that payload into a producer whose close callback releases
it. Do not free its source Vectis response while asynchronous writes still borrow
the payload, and do not make a second whole-value copy merely to enqueue it.
Vectis can explicitly preserve its existing source-reset behavior before handing
over a producer; the core producer starts at the supplied position.

## 10. Exclusive connection handoff and duplex I/O

This interface is required for the Vectis replacement, not an optional future
escape hatch:

```c
rotmos_status rotmos_request_takeover(rotmos_request *request,
    const rotmos_channel_config *config, rotmos_channel **out,
    rotmos_error *error);
rotmos_status rotmos_channel_read(rotmos_channel *channel, void *buffer,
    size_t capacity, size_t *received, rotmos_error *error);
rotmos_status rotmos_channel_write(rotmos_channel *channel, const void *data,
    size_t length, size_t *accepted, rotmos_error *error);
rotmos_status rotmos_channel_interest(rotmos_channel *channel,
    unsigned int interest, rotmos_error *error);
```

Successful takeover at headers time atomically suspends managed body reads,
transfers every already-read surplus byte into the channel's bounded receive
view, and transfers event/disconnect ownership to the channel. Failure transfers
nothing. There is one owner: neither the application nor a second watcher reads
the accepted descriptor or calls SSL_read/SSL_write alongside the channel.

Initially the channel is HTTP_INPUT: read returns raw post-header bytes, including
chunk framing/trailers or early WebSocket bytes. Final HTTP/SSE/non-101 rejection
output continues through the request's managed response interface. Raw channel
write is allowed only after the explicit tunnel upgrade below; this keeps HTTP
response framing, commitment and accounting in one place.

`rotmos_channel_body_complete(channel, tail, tail_length, error)` is the host
framer's explicit declaration that request body/trailers ended. Any bytes beyond
that message boundary are returned as a bounded copied pipeline tail, exactly
once. The library does not reparse or silently discard the host's tail. HTTP
parsing resumes only after successful body completion, response drain, and a
connection policy allowing reuse. Reject a tail over the configured bound. An
incomplete body, early local rejection or invalid framing closes rather than
guessing where the next request starts.

The framer stops reading at the declared boundary and passes exactly its unread
suffix. Successful body_complete copies that suffix and consumes the declaration;
it disables further HTTP_INPUT reads. Failure leaves input/tail ownership intact
so the caller can close; no partial tail transfer is allowed. Repeating a completed
declaration returns STATE. Prefetched backend surplus and caller-supplied tail
must not both enqueue the same bytes. The receive/tail bounds apply to coalesced
input too, before admitting another read.

For proxy upgrades, `rotmos_channel_upgrade(channel, headers, count, error)`
accepts the host's already validated successful upstream WebSocket handshake,
queues exactly one 101 response and transitions to TUNNEL. Validate the
downstream Upgrade/Connection/accept fields and preserve allowed negotiated
extension/subprotocol fields. The proxy must not forward early client frames
upstream before that upstream handshake succeeds. Raw output is ordered after
the downstream 101; a non-101 upstream reply stays an ordinary response with
bounded body/trailers. Do not use the application WebSocket parser for tunnels.

Read/write are nonblocking, and plaintext and TLS have the same logical API.
AGAIN is not EOF. TLS read-needs-write and write-needs-read are handled inside
the channel's event calculation; a prior read retry cannot suppress the first
write. Accepted write prefixes are copied into a bounded queue and retain retry
storage until the TLS operation is resolved. `rotmos_channel_queued_bytes` and
drain notifications expose capacity without exposing netbufs.

Half-close drains already readable bytes before reporting EOF, preserves pending
output, and does not continuously rearm a closed read half. A separate
`rotmos_channel_shutdown_write` requests a bounded orderly output shutdown;
read-side EOF alone is not permission to discard output. Channel close and worker
shutdown cancel watches, timers, producers and transfers exactly once.

Handoff does not disable server resource limits. Header validation/limits and
admission apply before takeover; channel queue, connection and write-idle limits
remain active. The host owns its raw-body framing/size and proxy progress/total
deadlines. Activity records distinguish actual I/O progress from a wake event.

`rotmos_channel_config` declares finite receive/send/tail capacities, initial
logical interests, an event callback, a terminal callback and userdata. Its
event callback receives `(channel, ready_flags, userdata)` and runs only on the
owning worker loop. An initial pending surplus schedules READABLE even if no
new socket event occurs. Channel close notification occurs before the enclosing
request's terminal callback; metadata still exists, but I/O is closed. Keep a
single explicit destructor owner when both callbacks reference shared state.

Queued output from an earlier pipelined response, including an active producer,
must remain ordered and drain under its existing ownership during takeover.
Per-response commitment/replacement cannot discard that earlier output. The
queued-stream and prior-response cases in Vectis's direct-I/O regression are
required successful handoffs, not permission to reject every nonempty queue.

## 11. Loop integration and wake paths

Expose owner-loop FD watches, one-shot/repeating timers and deferred callbacks:

```c
typedef void (*rotmos_retire_fn)(void *userdata);
rotmos_status rotmos_loop_watch(rotmos_loop *loop, int fd,
    unsigned int interest, rotmos_watch_fn callback, rotmos_retire_fn retired,
    void *userdata,
    rotmos_watch **out, rotmos_error *error);
rotmos_status rotmos_watch_update(rotmos_watch *watch,
    unsigned int interest, rotmos_error *error);
void rotmos_watch_cancel(rotmos_watch *watch);
rotmos_status rotmos_loop_timer(rotmos_loop *loop, long delay_ms,
    long repeat_ms, rotmos_timer_fn callback, rotmos_retire_fn retired,
    void *userdata,
    rotmos_timer **out, rotmos_error *error);
void rotmos_timer_cancel(rotmos_timer *timer);
rotmos_status rotmos_loop_defer(rotmos_loop *loop, rotmos_defer_fn callback,
    rotmos_retire_fn retired, void *userdata, rotmos_error *error);
```

Events describe READABLE, WRITABLE, READ_CLOSED and ERROR independently.
Zero interest suspends a watch; it does not close its borrowed descriptor.
Unchanged interest must not spuriously rearm or generate work. Watch cancellation
prevents future callbacks; internal event memory remains safe through the current
batch. The caller retains userdata until its active callback returns and a
non-reentrant `retired(userdata)` confirms removal. Each accepted watch/timer/
deferred item invokes its optional retirement callback exactly once, including
shutdown or cancellation before first delivery; rejected registration invokes
neither callback. The callback is the point at which userdata may be released.
Do not expose epoll/kqueue flags or require an upstream library's private structs.

Timers and `rotmos_loop_now` use a monotonic time base; absolute timestamps use
`rotmos_u64` milliseconds, while bounded relative durations use `long`. Neither
uptime nor subtraction may wrap at a 32-bit signed-long boundary.
Zero repeat means one-shot; zero delay means deferred work on a later turn,
not immediate recursion or a
busy timer. Bound pending deferred work and per-turn work. Parent and worker
loops are distinct; watches/timers are created in the process which owns them
after the relevant fork boundary. This supports parent auth/control work and
worker curl-multi socket/timer integration without a Kore dependency in Vectis.

A one-shot timer retires after delivery. A cancelled repeating timer cannot rearm
when its callback returns. Repeats coalesce missed ticks, with no catch-up burst.
Accepted deferred work runs once on a later turn or retires without delivery
during shutdown. A full pending-work budget returns LIMIT with no ownership
transfer; it is not a retry notification that may be silently lost.

Provide `rotmos_waker_new`, `rotmos_waker_signal` and `rotmos_waker_destroy`.
A waker has bounded coalescing state and an owning-loop callback; signal is the
only thread-safe operation on it. It schedules loop work through a real wake
descriptor, not a periodically checked flag. Signals may coalesce: the callback
drains the application's own queue to its declared budget and reschedules if
needed. Signal is not async-signal-safe and not a cross-process pointer API.
Stop producers and join their threads before destroying the waker. Creation and
destruction require its loop and cannot straddle a fork or server run.
Waker creation also declares an optional retirement callback. Destruction
disables delivery and retires after pending batch references. Signalling which
races a running callback must leave either a pending callback or work observed
by that callback; clearing the coalescing bit must not lose the wake. A signal
after owner-loop destruction has begun returns STATE; producers must already
have stopped before retirement, so no freed pointer can be signalled.

These facilities do not become a generic service scheduler or message broker.
Vectis keeps its supervisor dispatcher, outbox/mailbox and application clients.
Generic proxy curl pools move into rotmos. Where libcurl connect-only hides TLS
retry direction, the migrated proxy retains its existing
documented bounded retry policy; do not fabricate a dependency capability or
bypass its public API. This is separate from the downstream TLS channel, whose
retry direction the owned backend actually knows.

Watch callbacks receive `(watch, ready_flags, userdata)`; timer callbacks receive
`(timer, userdata)`; deferred/waker callbacks receive `(loop, userdata)`. All
return void and handle operational errors by an explicit owner-loop action.
Retirement notifications are non-reentrant and occur after the last possible
event-batch reference. Delay/count/queue arithmetic is checked; zero-interest
watches and cancelled timers cannot become retry spin loops.

## 12. TLS, listeners and ACME

Listener records specify bind address, local port, HTTP versus HTTPS, identities
and optional redirect policy. TLS modes are DISABLED = 0, MANUAL and ACME. Manual
identities accept either combined certificate/key PEM or separate chain/key,
each from a path or copied memory, with conflicting sources rejected. A separate
extra server-chain source is not a client-auth trust bundle. Require nonempty
valid material, matching keys and a finite `max_material_bytes`; the proposed
default is 1 MiB per input, explicitly overridable. Vectis can adapt `lc_source`
material to this bounded input without making Lockd a library dependency.

TLS 1.2+1.3 is the initial default; explicit 1.2-only/1.3-only/both selections and
the current OpenSSL cipher-list behavior must remain available. Do not claim a
separate TLS 1.3 cipher-suite setting on the strength of the existing cipher-list
setter. Client CA and required-client-certificate controls preserve mTLS and its
failure cases. Unknown SNI is rejected; Host/SNI mismatch is handled consistently
with the current 421 path. Manual wildcard host acceptance remains an explicit
identity choice, never an implicit ACME wildcard certificate claim.

Memory PEM may be materialized to private temporary files only where the backend
requires paths, with this behavior documented, bounded and cleaned after failure
or stop. It is not an HTTP streaming operation. Do not require external DH
parameter files for cleartext or direct/manual TLS mode. Test certificate/key
reference release and restart cleanup. Live certificate replacement via full
server restart is required; hot reload is not an initial promise.

Redirect policy preserves status 308, allowed-host checks, raw path/query, IPv6
authority handling, and the distinction between local and presented ports.
Omit the presented HTTPS port when it is 443; do not redirect to an unchecked Host
or reconstruct a URI from decoded routing data. The Vectis adapter preserves its
explicit wildcard behavior and presented_http_port metadata.

ACME requires explicit domains, account email, directory URL and a prepared
private runtime directory. Merely creating a server performs no ACME traffic.
Its runtime directory is working state; Vectis's durable storage remains external.
Support the existing account-key recovery and HTTP challenge/order/renewal path.
ACME transport timeout/response bounds and CA trust affect backend workflows,
not Vectis's application clients. The imported CA-environment patch uses
`SSL_CERT_FILE` and `SSL_CERT_DIR`, not `CURL_CA_BUNDLE`. Resolve explicit backend
trust first, then those environment sources, then the selected SDK's defaults.
Snapshot environment configuration before forks; a later environment edit must
not silently change a running client's trust. Report invalid explicit sources.

Define `rotmos_acme_state_fn(server, event, domain, runtime_dir, userdata,
error)` with explicit events and process context:

1. RESTORE runs in the thread-quiescent parent before backend forks. The host
   hydrates its private runtime directory before workers read account/key state.
2. PERSIST runs in the key-manager process after candidate certificate files are
   closed and before distribution/activation. The host persists the complete
   state required by its current Vectis workflow, not just the named certificate.
3. ERROR is observable with domain and cause. Persistence failure prevents
   distribution, reaches the parent's child-failure observation and applies
   the selected worker death policy; it is not logged as a successful renewal.
   RESTART can keep the runtime alive with existing usable state. The run result
   records a bounded last-failure snapshot and recovered-failure count without
   turning an eventual normal requested stop into a false fatal exit. Existing
   state is not presented as newly persisted state.

The callback receives borrowed directory/domain views and a known role, not an
`lc_client_config`. Any thread-bearing storage client must be opened in that
process after its fork. A callback may perform an explicitly bounded persistence
operation in this dedicated auxiliary process; it must not block an HTTP worker
or parent event loop. Supply a configured persistence deadline and failure tests.
Do not promise a storage transaction/rollback that the existing callback cannot
provide. Storage ownership, encryption, multi-writer policy and durable format
remain Vectis concerns.

RESTORE must close/join any temporary thread-bearing storage resources before
returning to the parent fork boundary. It cannot leave a supervisor client alive
in the runtime parent for reuse in the key manager. PERSIST receives its own
post-fork client lifetime; it must clean partial initialization on failure.
Validation never calls RESTORE/PERSIST. RESTORE receives a cooperative startup
budget and must return within it; a synchronous C callback cannot be forcibly
unwound by the parent. An auxiliary PERSIST deadline is enforced by the parent
through the owned child lifecycle, with a timed-out child reaped and no candidate
activation. Timeout does not prove that an external storage write did not occur.

Initial ACME configuration requires the RESTORE/PERSIST callback contract;
standalone applications can implement it over their own private durable
directory without a Lockd dependency. Runtime readiness reports each TLS
identity as pending or usable; it does not imply that a remote CA already issued
a certificate. Supply an identity-change observation for activation/failure.
Paused application serving must not disable the backend's configured ACME
challenge handling, or initial issuance could never complete. That exception
does not bypass application auth on arbitrary routes.

PERSIST sees a complete, bounded candidate state directory. The backend must
keep that candidate separate from active material until the callback succeeds;
an auxiliary restart must not activate a candidate whose persistence failed.
Publish a complete certificate/key identity as one selected generation; separate
renames of live key and certificate files do not provide atomic pair activation.
Private staging/publication must retain the last acknowledged usable generation
after a crash or restart. This is an implementation requirement within the
backend's private runtime directory, not a new public storage format or
transaction service. The callback acknowledges the host's
durability policy; it can be retried with the same snapshot, so the host must
make repeated persistence safe. The backend cannot roll back an external write
which completed before a callback returned an ambiguous failure. Test failure
before acknowledgement and interruption between acknowledgement and activation.

## 13. Application WebSockets

`rotmos_websocket_accept` validates a downstream handshake and installs typed
open/frame/close callbacks. Allocate state transactionally; a rejected handshake
must not retain state awaiting a disconnect callback that will never be installed.
Frames are borrowed for the invocation, with explicit opcode and byte length.
`rotmos_websocket_send` accepts only a bounded payload into the configured output
budget and reports backpressure. Close is idempotent; open and terminal callbacks
occur at most once in their documented order.

Accept also registers `on_writable`; an output frame is accepted atomically or
returns AGAIN with nothing queued. Resume after capacity notification, never by
polling. Oversize frames return LIMIT. Frame callbacks accept a complete bounded
frame with OK or pause with AGAIN and no consumption; explicit
`rotmos_websocket_resume` redelivers a held frame before any later frame. This
does not turn frames into unbounded message assembly. Validate control/close and
UTF-8 text according to the advertised WebSocket protocol. A finite close
handshake deadline (default 5000 ms) prevents a peer from holding shutdown
indefinitely and is capped by an earlier worker shutdown deadline.

Failed accept leaves callback userdata with the caller. Successful accept owns
one on_close notification even if the handshake fails before on_open. No frame
or writable callback occurs before on_open or after terminal close. The request
metadata must be copied if application WebSocket state needs it after upgrade.

The initial application path provides bounded text/binary frames and the current
ping/pong/close behavior. Kore's receiver does not support continuation frames;
advertise that limitation and return NOT_SUPPORTED for unsupported native
operations. A continuation enum alone is not evidence of fragmentation support.
Do not assemble an arbitrary full message and call it streaming. Native
fragmentation support would require a separately implemented frame/continuation
contract and tests. Transparent proxy tunnels in section 10 retain opaque,
bounded relay of fragmentation, control frames and negotiated extensions.

The handshake request and accepted WebSocket have separate lifetimes. The
request's terminal outcome records `upgraded`; its native request then expires.
WebSocket callbacks use separately registered state, with on_open dispatched
after the handshake request's terminal notification and on_close owning the
WebSocket state's release. Sharing a userdata pointer requires an explicit
ownership transfer in application state; the library must not implicitly free
that pointer at both request close and WebSocket close.

## 14. Admission, address policy, logging and observations

Admission callbacks run at two stages: accepted TCP connection before expensive
handshake/work allocation, and validated request headers before body/proxy
allocation. Default is allow. An explicit decision can close a connection or
send a bounded local HTTP response. The shared 16-exchange proxy default is
configurable in librotmos; Vectis can add stricter application admission.

Expose transport events for TCP stall, TLS failure, malformed request/status,
request rejection, application-named events, response commitment and terminal
outcome. Include a copied/borrowed peer snapshot as appropriate even when there
is no valid HTTP request. Do not invoke a callback with a freed request after a
final flush. The generic process-shared autoblock table and worker-death lock
recovery move into rotmos; hooks must not become no-ops behind disabled guards.

Transport events always identify the real TCP peer. Request-level effective-IP
resolution is separate and defaults to that peer. The generic resolver preserves
trusted-proxy chain resolution and X-Real-IP fallback; untrusted headers must
never affect pre-request transport admission. Use the same resolved request
identity for handlers, request-level autoblock and access logging.

The pslog integration accepts a borrowed native logger and copied access-log
rules, with trace/warn/error categories and exact status overrides including
DISABLED. Structured and file access sinks are independent. Preserve repeated
Set-Cookie and response-status classification without putting sensitive headers
or bodies in logs. Expose effective-IP and correlation-ID resolvers; do not invent
a competing logging formatter or logger hierarchy.

Worker-to-parent log records remain bounded and length-validated, with initialized
IPC padding. Parent sink emission must not enter arbitrary Lua states; require a
native-capable sink where the process/thread context needs it. Verify one selected
libpslog implementation/provider rather than accidentally compiling the imported
single-header implementation and shipping a second independent logger runtime.

Observers receive stable snapshots with monotonic timings, role/worker identity,
status, commitment flag, accepted/transmitted body counts, abort reason and
disconnect side where known. Unknown values are explicit, not fabricated.
Vectis owns metrics aggregation/persistence. Exactly-once final accounting must
cover replacement before commitment and abort after commitment; queuing a chunk
is not proof it reached the client.

Counts use checked wide values. Body counts exclude framing; HTTP wire counts
include plaintext HTTP headers/framing at the transport boundary, not TLS record
overhead or TCP acknowledgements. A queued/accepted count and a transmitted
count are distinct. If a backend cannot measure a quantity, report it as unknown
rather than substituting queue length or a producer's representation size.

## 15. LoneJSON and backend escape hatch

The LoneJSON helper header exposes validation and map-based parsing over an
explicitly MATERIALIZED/BUFFERED request body, preserving rewind/read error
behavior and upstream `lonejson_map`/error/cleanup contracts. Parsing may read a
spilled file incrementally, but decoding a complete object is still materializing
that object. Do not label it live request streaming. Generic typed JSON routes,
selected-array flow and generated JSON responses are selected toolkit features
in section 19. XML routes use the libxml2-backed public codec. Application schemas
and unrelated DSV/data workflows remain Vectis integration concerns.
LoneJSON owns JSON parsing/serialization, including ACME JSON; retain no second
ad hoc parser as an extraction shortcut.

The opt-in Kore header exposes:

```c
struct http_request;
const char *rotmos_kore_revision(const rotmos_server *server);
struct http_request *rotmos_kore_request(rotmos_request *request);
```

The native pointer is borrowed for the same request lifetime and owner loop. It
is NULL for a non-Kore backend or after its native request has ended. Vectis's
`vectis_request_kore` delegates to this accessor. Document that native layout is
coupled to the packaged backend revision, not a backend-independent ABI promise.

A separately opted-in backend-header component supplies matching patched C89
Kore declarations and required backend compile definitions to users who inspect
those structures. Core headers/imported targets do not inherit them. This does
not publish arbitrary private `kore_*` dynamic symbols or permit concurrent native
I/O while managed transport owns the connection. Actual transport intervention
uses takeover; any additional native operation needs an explicit supported
backend extension and verification, not an unresolved symbol or a local extern
declaration. Vectis's own adapter must use ordinary librotmos operations for the
workflows specified here; the escape hatch is not its missing integration layer.

## 16. Backend and package requirements

Kore remains a pristine pinned snapshot with an external ordered patch stack,
applied exclusively to owned generated build copies. Add extraction/C89 patches
after the preserved Vectis series; do not edit vendor/kore to implement this API.
Upgrades must rerun semantic API/behavior checks, not only patch replay.

Implement a private backend boundary with explicit configure, start, loop,
stop and teardown operations. Do not expose or invoke a renamed standalone main
as the whole server API, rewrite argv/environ/process titles, daemonize, chdir,
drop the embedding parent's privileges or enable seccomp implicitly. Preserve the
existing embedded/direct-TLS/no-seccomp behavior while making any future process
security options explicit.

Expected configuration, bind, TLS, allocation and callback failures must not call
exit/abort in the embedding parent. This requires backend error-path work;
preflight around a fatal-only backend is not sufficient. Fatal failures in owned
worker children produce lifecycle events and apply the selected death policy.
This does not promise recovery from arbitrary memory corruption.

Install static/shared targets as `rotmos::static` and `rotmos::shared` from
`find_package(rotmos CONFIG REQUIRED COMPONENTS ...)`, with a distinct opt-in
Kore-header component. Hide private adapter/Kore symbols with native export
controls; ensure static archives do not impose unresolved `vectis_*` hooks or
colliding standalone main/global callbacks. Do not leak build/cache paths.

Initial dependency candidates remain cpkt 0.6.1, LoneJSON 0.45.0 and libpslog
0.12.0, the releases checked during the extraction audit. Acquire exact target
SDKs/digests from c89systems, retain independent prefixes, and use complete
upstream imported-target link closure. Core server headers must not acquire
Lockd or cpktdb/cpktmisc merely because Vectis uses them. Lua 5.5.1 is a selected
dependency of the separate binding product, not the core C header/link interface.
The selected XML facilities use cpkt's libxml2; no extra XML SDK or parser is
introduced. See the complete dependency table in section 20.

Use the selected lifecycle toolchains and strict `-std=c89` plus warning/pedantic
gates on all owned C and the patched selected backend, with no GNU99 exception.
Compile standalone core, optional integration and backend headers. Preserve the
Vectis-consumer target set: GNU/musl x86_64, aarch64 and armhf Linux, plus arm64
Darwin. Local osxcross supplies Darwin build evidence; native runtime evidence
must be distinguished from cross compile/link inspection. No release is implied
by implementing this spec.

## 17. Concrete Vectis cutover

1. Keep Vectis's public C/Lua declarations and application policies/supervisor.
   Translate its request/response builders and registration APIs into public
   rotmos operations; reusable server mechanisms have one owner in rotmos.
2. Replace private `vectis_kore_runtime` object compilation with the installed
   rotmos target. Move runtime-global configuration and worker setup behind the
   library boundary. Remove Kore feature definitions from ordinary Vectis TUs.
3. Construct a rotmos server in the existing runtime child. Pass explicit
   effective settings, identities, paused-start gate, LIBRARY signal ownership
   in that isolated runtime child, and owned control FD. Map
   child readiness to Vectis's current control frame, then release serving only
   when its supervisor services are ready.
4. Translate headers into Vectis request metadata once. Move the generic selector
   and ordinary/proxy/static/WebSocket ordering into the public rotmos router,
   registering Vectis application handlers through that router before body delivery.
   Select per-route body plans, preserving the 3 GiB upload default and current
   explicit server ceilings.
5. Map upload callbacks and materialized-body views without a second full body
   copy. Keep blocking reader/format adapters in Vectis with bounded worker wake
   and cleanup. Translate response variants into bytes/file/producer operations,
   preserving materialized versus live-source names and ownership.
   Keep Lua dispatch on the worker loop and retain callback/producer registry
   roots until their actual terminal close, not just until the route returns.
6. Lift generic proxy transport, curl pools, framing and hook execution into
   rotmos, using its channel/response/loop APIs. Vectis supplies destinations and
   application callbacks. Lift generic WebDAV/storage, static and JSON/XML route
   machinery likewise; remove duplicate engines and all direct Kore/SSL/event
   access from the Vectis adapter.
7. Connect autoblock/admission, effective IP, observations, parent auth timers,
   worker client initialization and teardown, and ACME persistence with typed
   callbacks. Preserve process domains; no dummy Vectis hook definitions remain.
8. Delegate vectis_request_kore through the explicit extension. Remove Vectis's
   vendored Kore source/patch/build machinery in the same cutover and update its
   docs, Lua coverage matrix, examples and package/source-rock closure.

This task does not authorize edits in the Vectis repository. The above sequence
defines the later consumer change, with a clear cutover rather than a permanent
parallel runtime or refactor feature flag.

### Illustrative ordinary consumer

This is proposed consumer syntax, not a compiled example. Once the API exists,
turn this shape into a real installed static/shared consumer regression. It uses
an explicit listener and explicit standalone signal ownership; it needs no Kore
types or application router.

```c
#include <rotmos/rotmos.h>
#include <stdio.h>

static rotmos_status hello(rotmos_request *request,
    rotmos_request_plan *plan, void *userdata, rotmos_error *error)
{
    (void)userdata;
    plan->disposition = ROTMOS_REQUEST_REPLY;
    return rotmos_response_bytes(rotmos_request_response(request),
        200, "text/plain", "ok\n", 3u, error);
}

int main(void)
{
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_server *server;
    rotmos_run_result result;
    rotmos_error error;
    rotmos_status status;
    int failed;

    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    rotmos_error_clear(&error);
    listener.bind = "127.0.0.1";
    listener.port = 8080u;
    config.listeners = &listener;
    config.listener_count = 1u;
    config.dispatch_mode = ROTMOS_DISPATCH_CALLBACK;
    config.workers.count = 1u;
    config.callbacks.on_headers = hello;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    server = NULL;
    status = rotmos_server_new(&config, &server, &error);
    if (status != ROTMOS_OK) {
        (void)fprintf(stderr, "%s\n", error.message);
        return 1;
    }
    status = rotmos_server_run(server, &result, &error);
    failed = status != ROTMOS_OK;
    if (failed)
        (void)fprintf(stderr, "%s\n", error.message);
    if (rotmos_server_destroy(server, &error) != ROTMOS_OK)
        return 1;
    return failed ? 1 : 0;
}
```

## 18. Acceptance and implementation sequence

Each row requires observable tests, including failed execution and cleanup.
Do not count an API-presence probe as behavior evidence.

| Gate | Required evidence |
| --- | --- |
| C89/API | Real compile commands for core/backend/header/example TUs; strict installed C89 and C++ consumers; no private includes/defines |
| Build boundary | Original 42 patches plus extraction patches replay; pristine snapshot unchanged; static/shared installed links, exports and no vectis imports |
| Configuration | Defaults/zero normalization, every mapping above, invalid enums/negative times, width overflow, range limits and effective timeout rounding |
| Lifecycle | Bind conflict after validation, startup-hook failure, real ready barrier, paused admission, gate release/worker replacement, restart and second-active-server rejection |
| Process ownership | Hook role/order, post-fork resources, unrelated child not reaped, first signal preserved, control EOF/parent death and bounded escalation |
| Requests/parser | All methods, raw/decoded separation, ordered duplicate headers/query data, exact Host lookup, malformed NUL/bare-CR/field-count blocks, near-64-KiB headers, Host/SNI mismatch |
| Body modes | Empty/fixed/truncated/oversize bodies, early rejection, no forbidden spool, explicit spill cleanup, pause/resume partial consumption and no redelivery of accepted bytes |
| Streaming | First output before producer EOF; chunk-size-independent memory bounds, slow producer/consumer, cancel/failure and cross-thread wake; counters beyond 4 GiB without full-value buffers |
| Async progress | Producer AGAIN requires explicit resume; no writable-event polling, lost wake or zero-progress spin; body EOF with delayed reply retains the request until drain/deadline |
| Cancellation | Cancel watch/timer/defer within its callback and earlier in the same event batch; retire once on success/shutdown; no rearm, callback after retirement or userdata use after free |
| Reader/Lua dispatch | Slow native reader does not stall an unrelated request; upload/DSV Lua callback thread equals the owner worker thread; callback/producer roots survive GC until terminal cleanup |
| Responses | Repeated Set-Cookie, known-length under/overflow, HTTP/1.0 close and HTTP/1.1 chunk framing/trailers, HEAD/1xx/204/205/304, interim/final distinction |
| Commitment | Untouched queued 200 replaced by complete 502 once; partial write/TLS retry refuses replacement; committed failure has no success terminator |
| Response state | Exercise ACTIVE/FINISHING both before/after commitment; declared trailers rejected before an incompatible begin, atomic end retry, terminal producer close and strict byte/EOF results |
| Handoff | Coalesced header/body/trailer/pipeline bytes, bounded tail return, preserved queued output, exclusive ownership, read-half-close drain and HTTP restore |
| TLS channels | Plain/TLS same observable contract; read WANT_WRITE and write WANT_READ; no busy-spin or dropped queued writes; mTLS, slow peers and disconnect during retry |
| Proxy integration | HTTP/SSE, chunked upload/trailers, HTTP/2 upstream duplex, shared admission, raw WebSocket early frames/fragmentation/extensions and non-101 streamed rejection |
| Native WebSocket | Successful/rejected handshake state cleanup, bounded frames/backpressure, native unsupported continuation behavior, disconnect once |
| TLS/ACME | Path/memory/bundle/chain, incorrect key/trust/SNI/client cert, no required DH files, redirect authority, account recovery, restore/persist context and persist-before-activate failure |
| Events/logging | TCP versus effective IP, admission before allocation, once-only accounting, status override/disabled/file sinks, malformed/oversize worker logs and stable post-flush snapshots |
| Resources | Heap allocation failure, zero-length/cached file content type, no key/pool/IPC leaks, shutdown with active channels/producers; focused musl default-worker-stack proof where applicable |
| Consumer cutover | Vectis C/Lua routes, uploads, static/packed assets, WebDAV/auth, services, metrics, normal/packed execution and installed/source-rock consumers |
| Toolkit | Public C router ordering/captures/groups, profiles/trusted-IP/autoblock, typed JSON/multiple output schemas, libxml2 mapped/DOM/SAX/resumable output, static and filesystem/read-only/custom DAV workflows |
| Storage | Concurrent conditional mutations, cancellation before/after commit, provider AGAIN/completion races, finite lock wait and unrelated request progress during slow native jobs; confined paths and symlink replacement races |
| Codec limits | Preserve Vectis writer isolation under original-table/option mutation, GC/reentry and spooled-field cleanup; preserve rejection of external mutable records in callback writers; parser depth/text/node budgets, disabled external entities, scoped XML callbacks and generator output without hidden staging |
| Lua public boundary | Build only from installed public headers against shared librotmos; inspect imports/loaded identity, reject private-symbol consumers, prove no static runtime copy and no private Vectis/Kore/codec access |
| Lua behavior/delivery | C/Lua workflow parity, binary payloads, owner retention/GC/mutation, protected callback errors, fork roles, backpressure, proxy/DAV, reconstructed source archive, rockspec/source rock and matching Lua runtime |
| Lua embedding | Host-created VM preload/require/run/stop/host-close, existing globals preserved, loader conflict/idempotence, one shared core/provider identity, exact wide counters and allocation-failure cleanup |

Recommended implementation order:

1. Add private backend lifecycle/error hooks and the C89 port; verify explicit
   start/stop independently of a standalone main and Vectis symbols.
2. Implement inert configuration/validation, lifecycle callbacks, supervision,
   effective settings and ordinary empty/bytes request-response behavior.
3. Add body modes, producers, files, commitment and cancellation state machines.
4. Add loop/watch/waker and channel handoff, then native WebSocket and TLS/ACME
   callbacks; prove the hard proxy/duplex scenarios before claiming replaceability.
5. Complete the reusable router, profiles, static serving, JSON/XML codecs/routes,
   proxy and WebDAV through public C APIs. Add the Lua facade/C binding against
   the installed shared API and prove its source-archive/LuaRocks delivery.
6. Finish exported packages/examples, then perform the separate Vectis cutover
   and all affected consumer gates. No stage is a release merely because it links.

The largest risks are expected-failure handling in Kore's fatal paths, ownership
across callbacks/flushes and event batches, readiness/fork domains, raw surplus
and TLS retry state, and the 32-bit C89 counter/file boundary. Resolve these
through focused observable tests before adding convenience APIs.

## 19. Required server toolkit

These are selected deliverables, with public C operations and Lua counterparts;
they are not left as Vectis-only implementations. Keep transport, routing,
codecs, proxy and WebDAV in focused modules rather than one server god object.
All high-level helpers build on the request/response/loop contracts above.

### Routing, profiles and ordinary serving

Provide `rotmos_router_new`, `rotmos_server_router`, `rotmos_router_route`,
`rotmos_router_group`, `rotmos_router_dispatch_headers` and
`rotmos_request_path_param`. `rotmos_route_config` has a copied path, method/mask,
literal/parameter/regex kind, body plan, typed handler and borrowed userdata.
Registration returns a router-owned route handle. Each server creates an empty
owned router; `rotmos_server_router` borrows it. Optional
`rotmos_server_router_take` replaces an empty router with an independently
created one outside a run, transferring ownership only on success. An attached
router cannot be destroyed/attached elsewhere; unattached routers use
`rotmos_router_destroy`. Route/group handles never own a second copy. Registries
are immutable from STARTING until run returns; registration errors leave the
registry unchanged and caller userdata unconsumed.

Reject invalid regexes and exact conflicts: overlapping normalized method masks,
the same path kind and identical path text, matching Vectis's registration
predicate. General overlap across regexes, parameters and literal/proxy routes
is allowed and resolved by the preserved ordering; rejecting every possible
overlap would break valid Vectis configurations. Group prefixes/options are
copied at registration, not mutable aliases that can change a running route.

Preserve registration ordering across route kinds, static trailing-slash and
method-denial exceptions, captured parameter lifetimes, validated decoded paths,
ordered query pairs and the existing safe raw proxy fallback. An ordinary
route's rejected path must not silently become an unrelated proxy route. Preserve
the source/overlap regression cases when moving the selector out of Vectis.
Groups apply one prefix and shared middleware/auth hooks without copying route
matching into Lua. Unmatched routes return 404; matched static/WebDAV method
denials return the appropriate 405/Allow contract. ANY never accepts a method
which the backend rejected as unknown.

Supply helpers for verb routes, streaming uploads, fixed replies, static files,
static directories and read-only asset providers. `rotmos_static_config` covers
root/prefix, index behavior, explicit content type/cache policy and public
authorization hooks. Own path confinement, ETag/If-Match/If-None-Match and bounded
range behavior in the generic helpers; do not make each consumer reconstruct
those mechanisms. Packed asset lookup is a provider supplied by Vectis through
public stat/list/open/read operations, not a Vectis structure inside rotmos.
Path confinement includes decoded traversal, embedded NUL, repeated decoding
and symlink escapes. The filesystem provider defaults to rejecting symlinks and
resolves relative to pinned roots without an unchecked stat/open gap; lexical
prefix checks alone do not establish confinement. Conditional/range policy is
evaluated against the same opened object used for delivery.

Provide `rotmos_server_config_init_production_webserver` and convenience
constructors, retaining explicit listener/TLS selection and validated zero/default
semantics. Move the reusable trusted-proxy and process-shared autoblock mechanism
into rotmos, including worker-death lock recovery and peer-versus-effective-IP
separation. Production rule defaults can preserve Vectis's existing transport/
status thresholds; applications choose whether to enable that profile and add
named events. Auth hooks and middleware are generic; Vectis's auth providers,
credential issuance, sessions, login flows and Lockd storage remain outside.

### Fast typed JSON and XML/libxml2 routes

Public registrations include `rotmos_router_json_route`,
`rotmos_router_json_typed_route` and `rotmos_router_xml_route`, with schema/body
limits and typed handlers. Fixed JSON response registration is a separately named
materialized convenience, not a replacement for a typed request route.

The JSON fast path decodes through LoneJSON directly into typed record storage,
invokes the C handler or Lua callback with the corresponding public record view,
and serializes via LoneJSON. No intermediate JSON text round trip, duplicate
parser, mandatory Lua table/DOM conversion, or per-field Lua reflection loop is
inserted. Supply public schema-view/record-storage entry points alongside C-owned
map registration so the binding does not need a private route or record layout.
Lua uses LoneJSON's public schema/record interop, retaining owners until cleanup.
Preserve Vectis's established writer input/ownership contract:

- Lua callback writers accept value tables, allocate private records through
  `lonejson_lua_new_record`, and populate them through public `schema:assign`.
  They retain schema/record owners and copied options through output and cleanup.
  Changing the original table/options, collecting Lua objects, or reentering an
  independent writer cannot alter the private input already being emitted.
- Spooled input fields follow the existing public schema-assignment/spill policy.
  Disclose that input preparation; it does not materialize or spool the output.
- External mutable record userdata remains supported by the existing explicitly
  buffered serializers. Callback writers reject it with the existing actionable
  error. Do not add support for borrowing that userdata across output callbacks
  or asynchronous drain as a prerequisite of the extraction.
- The C API permits borrowed record storage with a terminal release callback;
  callers keep it valid and unchanged until release. Owned route/generator
  storage is retained until producer close when output becomes asynchronous.

This path already uses released public LoneJSON operations. No new mutation
lease, general record-copy API, private userdata cast or upstream dependency
change is required to extract it. The binding delegates ownership to public
rotmos entry points; it does not reimplement native storage or codecs.
Schemas, input maps and output maps may differ; typed replies may choose an
output schema per response. Preserve the current multi-output/generator use cases.

The source baseline is `vectis_lua_writer_record`/`vectis_lua_writer_clear_record`
in [vectis_cli.c](../../vectis/src/vectis_cli.c), documented in the
[Lua XML contract](../../vectis/manual/lua-xml.md) and exercised by
[upgrade_seams.lua](../../vectis/tests/lua/upgrade_seams.lua). Port those cases,
including mutation/GC/reentry, private spooled input and buffered external-record
acceptance, to the installed rotmos consumers. Add producer pause/cancellation
coverage for the new transport boundary while preserving the same input policy.

Typed object parsing materializes the declared record, under its body/field and
spill policy. Selected-array/item APIs use actual incremental producer/consumer
flow where requested. Generated JSON response output uses the public pull-style
LoneJSON generator and the response producer's backpressure. An operation which
returns a complete string/table is explicitly materialized. "Fast" denotes this
direct typed path; comparative throughput/allocation claims require measurements
against the current Vectis route examples and equivalent installed consumers.

XML routes use **libxml2** from cpkt and mapped LoneJSON records. Preserve
root_element, text_key, attribute_prefix, trimming, unknown-field handling,
depth/text limits, namespaces and repeated-element/array semantics. Provide
explicit typed/mapped parsing, materialized serialization, owned DOM/XPath views
and incremental SAX operations through public rotmos XML APIs. DOM operations
remain explicitly materialized, with mutation invalidating old views and owner
retention/process-thread confinement. SAX input is bounded and incremental.
Disable DTD/external resource loading per parser context; do not change global
libxml2 handlers or expose xmlDoc/xmlNode internals through the binding.

Initialize materialized XML document input/output limits to 16 MiB, nesting to
128, individual text values to 1 MiB, DOM nodes to 100000 and returned XPath nodes
to 4096; explicit larger finite values are allowed. HTTP routes also enforce
their selected body/record limits. Streaming SAX/generator operations enforce
depth/text and configured cumulative input/output limits without retaining the
whole document. These are logical limits, not a claim that libxml2's allocator
or XPath intermediate evaluation has the same byte bound. Expressions are
application declarations, never evaluated from untrusted request text by default.
Do not promise cancellable/preemptible XPath or strict process RSS based only on
an output-node count. Parser/record budget failures retire partial owners once.

Provide public `rotmos_xml_generator_new/read/destroy` for HTTP output. Its state
is resumable between bounded reads; each step drives public libxml2 output APIs
for bounded units and yields when response capacity is exhausted. The existing
synchronous sink writer cannot be converted into that contract by collecting its
whole output or hiding a staging file. Keep a separate synchronous XML write-to-
sink operation and a separately named materialized serializer. If a necessary
upstream public operation cannot support resumability, report the exact blocker
before implementing a substitute. Lua callbacks never longjmp through libxml2.

The generic XML codecs and route machinery move to rotmos; Vectis keeps its
application schemas and may preserve its old XML facade as an application
adapter. DSV and unrelated data/client workflows are not prerequisites of this
server toolkit and can consume the generic body/route interfaces from Vectis.

### Reverse proxy

Provide `rotmos_router_proxy` with copied `rotmos_proxy_config`, opaque inbound/
outbound/response/local-reply views and public accessors/edit operations. Lift the
generic selector/director hooks, headers/framing validation, curl pools, upload
framer, HTTP/SSE transport, WebSocket/WSS relay and response-rejection handling.
No public proxy declaration leaks curl/Kore/OpenSSL layouts.

Preserve configured HTTP/HTTPS targets and allowed alternates; raw path/query
rewrites; explicit Host and forwarding behavior; request preflight; rewrite;
uncommitted final-response modification; gateway-error local replies; sticky
failed edits; duplicate end-to-end headers; and the 64 KiB bounded local-body
contract. Application callbacks choose policy; rotmos owns execution ordering,
validation, transport and cleanup. Successful 101 handshake metadata is not
passed through an arbitrary final-response rewriter.

Defaults preserve the existing 10 s connect and 60 s idle deadlines, disabled
total timeout, 16 KiB direction chunks with the existing 8 KiB..1 MiB accepted
range, a shared 16-active-exchange worker admission default and four retained
idle connections per curl pool. Expose resource policy explicitly; raising a
limit does not inherit an unmeasured RSS guarantee. Admission precedes exchange
buffer/upstream allocation and is shared across HTTP and WebSocket routes.

Preserve HTTPS HTTP/2 preference/fallback, HTTP/1.1 cleartext and WebSocket,
same-exchange duplex upload/download, SSE progress, upload/response trailers,
100/103 handling, queued-response replacement, and abort after commitment.
Preserve in-memory CA/client identity bundles and peer/hostname verification.
Bound each direction under a slow peer, with early WebSocket bytes held until
the upstream approves the handshake and opaque fragmentation/extension relay.
Vectis retains configured destinations, business rewrites/auth decisions and
supervisor integrations; it no longer ships another generic proxy engine.

### WebDAV and storage providers

Provide `rotmos_router_webdav` with a filesystem-backed provider usable without
Vectis, plus public read-only/custom provider interfaces for packed/remote
storage. The initial filesystem provider lifts the existing direct-root and
managed-cache semantics: normalized confined paths, quotas, ETags, atomic
conditional mutations, collections, copy/move depth behavior, mutation locking,
tombstones and explicit partial-failure/recovery outcomes. Do not turn extracting
the code into a storage-format redesign or silently change existing roots.

Preserve OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, MKCOL, COPY and MOVE, including
405/Allow for read-only mounts. PROPFIND supports finite depth 0/1; omitted or
infinite depth retains the existing 403 finite-depth error, invalid depth 400.
Preserve Destination decoding/confinement, Overwrite, source/destination
authorization, conditional ETags under the same mutation lock, quota failures
and the current explicit incomplete MOVE cleanup behavior. Native filesystem
locks are not WebDAV LOCK/UNLOCK; do not advertise those protocol methods as
implemented. The existing client-side `vectis.webdav` curl helper is a separate
outbound-client product and need not move for server WebDAV support.

Define language-agnostic provider operations for stat, bounded listing, opening
read content, bounded writes, conditional create/delete/copy/move and terminal
cleanup. Provider metadata and body flow use public rotmos values/streams, not
`vectis_embedded_fs`, `lc_source`, Lua userdata or native file structs. A read-only
provider declares that capability; unsupported mutations are rejected before any
write. Do not emulate atomic conditional mutation as an unlocked stat-then-write
sequence over an incapable remote provider. Listing/mutation allocation and
temporary files are explicitly bounded. Any retained legacy materialized PUT
path is named and capped; a streaming upload path never collects the whole file.

Each provider call receives an opaque `rotmos_storage_operation` and immutable
arguments. OK completes synchronously; AGAIN transfers completion responsibility
to the provider, pins its owners and pauses the relevant HTTP flow. Complete
later through `rotmos_storage_operation_complete` on the owner loop, using a
waker for foreign-thread results. Failure before accepting an operation transfers
nothing. Completion occurs once; metadata/results are copied or explicitly taken,
and byte writes report accepted prefixes using the transport count rules.
Read/list iteration exposes bounded items and has explicit EOF, not read-all.
For a pending operation, the engine pins its bounded argument/chunk storage until
completion/cancellation retirement. Providers do not retain a transient public
body callback pointer after that callback returns. An accepted prefix becomes
consumed input once and is never retried as part of the next provider write.

Provider cancellation suppresses further HTTP work and invokes its cancel hook;
the provider acknowledges cancellation before its terminal retirement callback
releases operation userdata. A result racing cancellation is retired without
touching a closed request. Cleanup cannot free resources still used by a job.
Shutdown has the same bounded escalation/abandonment rules as other worker work.
Lua providers complete on their own loop and may not yield across the binding.

Declare conditional-mutation and cross-resource atomicity capabilities up front.
An incapable provider returns NOT_SUPPORTED before mutating, never an unlocked
stat-then-write approximation. Success acknowledges the provider's documented
commit/durability point. Cancellation after a committed mutation does not undo
it; outcome reporting distinguishes a completed mutation from abandoned delivery.
Cross-filesystem MOVE/COPY and partial recovery retain their explicit existing
semantics rather than acquiring a fictitious universal transaction guarantee.

The default HTTP PUT path is incremental: provider begin/write/finish/abort
operations receive bounded body chunks, with pause/resume when storage cannot
keep up. For filesystem storage the temporary destination is an explicitly
declared storage transaction, committed by replacement only after successful
end-of-body; it is not an undisclosed transport spool or a complete in-memory
body. Abort removes owned partial output. PROPFIND output advances a bounded
listing iterator/XML generator rather than collecting a whole directory tree.

The existing filesystem mutation lock uses blocking flock; importing it unchanged
would violate worker-loop progress. Acquire locks with nonblocking attempts and
owner-loop timer/deadline resumption, or in a declared bounded native job; never
wait on a lock, condition variable or thread join in an HTTP callback. Initialize
storage-operation timeout to 30000 ms, including lock wait, with finite overrides.
Regular filesystem syscalls may still block in the OS; the API promises bounded
queues and cooperative progress, not real-time disk latency. Potentially long
filesystem jobs run in a bounded post-fork native executor and complete via the
same operation contract. No executor thread calls Lua or owns a request handle.

Protected mount registration requires an auth callback. Its public request view
includes method and normalized source/destination resource. Its reply can allow
with a principal, conceal/deny, challenge or redirect with bounded copied fields.
Both resources are authorized before COPY/MOVE mutates storage. Vectis supplies
its existing auth/Lockd/packed adapters through those public interfaces. Rotmos
does not acquire mandatory Lockd/Pouch/database dependencies to serve WebDAV.

## 20. Shipped Lua facade, binding and dependencies

### Product and API boundary

Ship a separately packaged `require("rotmos")` module for **Lua 5.5.1**. Its Lua facade
normalizes ergonomic configuration and receiver calls over `rotmos.native`, a C
binding of the installed public **shared** librotmos library. Do not compile a
second static librotmos/Kore runtime into the module. Runtime ownership belongs
to the host, whether it is the Lua executable or an application embedding Lua.
The binding never creates a second VM, opens standard libraries implicitly or
closes the host VM. Supporting another Lua ABI requires an explicit product
decision; no interpreter executable or redistributed Lua SDK is part of this
module delivery.

Expected layout is `lua/rotmos.lua`, focused `lua/rotmos/` facades, and
`src/lua/rotmos_native.c` plus small binding-owned support files. These are planned
sources, not existing files. `luaopen_rotmos_native` is the native module entry.
Core C headers stay Lua-agnostic; `<rotmos/lua.h>` owns any public embedding or
conversion declarations. Vectis may retain its application Lua facade while
delegating the underlying server operations to public rotmos C APIs.
That Lua interop header belongs to the separate binding source/install product,
not the per-target C SDK payload.

Provide `rotmos_lua_preload(lua_State *state, rotmos_error *error)` from the
binding product. It registers the native loader and packaged Lua facade loaders
in an existing initialized VM, without requiring source-tree paths or creating
a runtime. Return CONFLICT for unrelated existing module registrations; repeated
registration by the same binding is idempotent. The host then uses ordinary
`require("rotmos")`. Declare required host libraries (`package`, plus those the
facade actually uses) and return an actionable error if unavailable. The binding
product exports `rotmos::lua` through `find_package(rotmos_lua CONFIG REQUIRED)`
for this embedding path. Preload and
dynamic loading resolve the same public shared core and provider instances.

The binding compiles with only installed public rotmos and Lua headers plus
explicit public dependency interop headers and binding-owned support headers.
No core/backend private source include path, implementation struct cast,
internal extern, direct Kore/socket/TLS/proxy engine,
or private libxml2 operation belongs there. The only direct provider conversions
are documented public LoneJSON schema/record views and pslog logger interop.
Reuse those upstream facades unchanged. If public interop is insufficient,
identify the missing upstream contract instead of copying private userdata.
The binding does not include the opt-in Kore backend header or expose its pointer
accessors to Lua. Record/logger interop uses one compatible shared LoneJSON/pslog
provider instance, not independent static copies in the binding and provider
modules; select and verify that shared closure explicitly.

All useful Lua behavior has a public C implementation/entry point: routing,
matching, JSON/XML codecs, proxy hooks, WebDAV, lifecycle, streaming and errors.
Do not implement a competing router/HTTP state machine in Lua or hide missing
native functionality behind a C binding-only helper. Facade aliases normalize
arguments into the same public C configuration and validation path.

### Lua surface and lifetime

Expose server creation/configuration, generic and verb routes, groups, uploads,
typed JSON routes, XML routes/codecs, static files/directories/providers,
WebSockets, reverse proxy, WebDAV, logging, run/stop/close and observations.
Names include `rotmos.server`, `server:route/get/post/put/patch/delete/head/options`,
`server:group`, `server:upload`, `server:json_route/json_typed_route/xml_route`,
`server:static_file/static_directory`, `server:websocket/proxy/webdav`,
`server:run/stop/close`, and `rotmos.json`, `rotmos.xml`, `rotmos.proxy`,
`rotmos.webdav` facades. `server:fixed_json` is explicitly a fixed materialized
reply. Method-specific DAV registration remains available through generic route
method sets; a mount normally registers its own method set.

Requests expose immutable metadata, ordered headers/query data, captures, body
mode/materialized access and streaming callbacks. Responses support text/bytes,
typed JSON, XML, files, producers, headers, status, end and abort. Binary strings
preserve NUL. Normal validation/runtime failures return `nil, error`, with stable
status/source/operation/native-code fields and copied diagnostics. Callback
failures are protected and converted into the native error/abort contract after
cleanup; they must not longjmp through active backend/codec frames.

Byte counts, offsets and lengths are exact Lua integers when representable;
larger `rotmos_u64` values use a public immutable count value with high/low words,
comparison and decimal formatting. Never convert them through a Lua floating
number, truncate to signed range or use numeric zero to mean absence. Reject
fractional/negative/out-of-range sizes before calling C. Missing headers return
nil; empty values return an empty string. I/O results distinguish accepted bytes,
the stable `rotmos.AGAIN` sentinel and true EOF; empty strings cannot stand for
EOF or a zero-progress producer success. Resume is an explicit owner-loop action.

Concretely, `response:write(chunk)` returns an accepted byte count, or
`nil, rotmos.AGAIN`, or `nil, error`. Reads return a binary chunk, or
`nil, rotmos.EOF/AGAIN`, or `nil, error`. A producer callback receives its bounded
capacity and uses that same chunk/EOF/AGAIN/error convention. Body callbacks
return a consumed count, or `nil, rotmos.AGAIN`, or `nil, error`; only the consumed
prefix is released. Frame callbacks return true for a whole accepted frame, or
`nil, rotmos.AGAIN`, or `nil, error`. The sentinels have stable identity and are
distinct from error tables and empty strings. Callback exceptions take the same
protected error path as an explicit error result; they never mean successful EOF.

Userdata retains its server/request/schema/record/producer owners until actual
terminal cleanup, including after a route returns. Scope-bound request views
fail cleanly when retained past close; do not expose dangling lightuserdata.
Explicit close is idempotent after orderly completion. GC never starts a server,
waits indefinitely, forks, or substitutes for an explicit stop/run join. Reject
close from an active callback when it would invalidate that callback's owner.

Acquire registry references and install finalizers with unwind-safe ownership:
an allocation failure during userdata creation must not strand a newly acquired
native resource. Protect all Lua callback entry points and retain the original
bounded error through cleanup. `__gc`/close methods release only resources they
own; they never close the host VM. Test allocation failure during registration,
callback result conversion and producer setup, in addition to explicit Lua errors.
The running server and callbacks are rooted until run returns; request/proxy/
provider wrappers become inert after their individual terminal notification.

All Lua callbacks execute on their owning runtime thread in the correct process,
never on a foreign producer/parser/logging thread. Bindings receive a public
process-context/role snapshot and a post-fork `on_process_start` hook for all
HTTP/key-manager/ACME roles before user callbacks. They may initialize the
quiescent forked Lua context and invalidate inherited live handles there, without
reading backend state. Do not share Lua states across OS processes/threads.
Blocking native readers communicate through bounded data and public wakers;
Lua body/proxy/schema callbacks remain owner-loop work. Yield across a native
callback is not supported by this initial binding.

Streaming uses bounded chunks plus explicit accepted/consumed or producer return
results and owner-loop writable/resume notifications. A Lua callback returning
AGAIN cannot cause the facade to concatenate chunks or create a temporary body
file. Raw backend access remains the explicit C extension; the Lua facade does
not publish Kore pointers, SSL handles or unscoped native addresses.

Illustrative proposed syntax, to become executable consumer examples:

```lua
local rotmos = require("rotmos")
local server = assert(rotmos.server({
  listen = { bind = "127.0.0.1", port = 8080 },
  workers = { count = 1 },
  signals = { mode = "library" },
}))
assert(server:get("/health", function(request, response)
  return response:text(200, "ok\n")
end))
-- Generic registrations also include json_route, xml_route, proxy and webdav.
assert(server:run())
assert(server:close())
```

`listen` is the single-listener convenience normalized to `listeners`; specifying
both is invalid. TLS and network actions remain explicit. Rich examples must
exercise schema records, producer backpressure and authorized DAV mounts, not
just show API presence.

### Dependency inventory and delivery

| Input | Selected baseline / role |
| --- | --- |
| Vendored Kore | Exact source revision and external patches already recorded; private HTTP backend |
| cpkt | 0.6.1; selected public packages for OpenSSL, curl, nghttp2, libxml2, Lua and complete native link closure |
| LoneJSON | 0.45.0; typed JSON/maps/generators, XML record mapping and ACME JSON |
| libpslog | 0.12.0; structured runtime/access logging and public Lua logger interop |
| Lua | 5.5.1 from cpkt's Lua runtime selection for binding builds/tests; hosts supply one compatible runtime, executable or embedded |
| LoneJSON/pslog Lua providers | Their matching released public facade/binding/interop products, only where schemas/loggers cross into the rotmos binding |
| OS runtime | Selected target libc, threads, sockets/files and platform event facilities |

The recorded cpkt v0.6.1 source pins include OpenSSL 3.6.5, curl 8.22.0, nghttp2
1.70.0, libxml2 2.15.4 and Lua 5.5.1. These are the SDK baseline, not permission
to mix unrelated host versions. Their transitive compression/crypto/SSH/system
closure comes from imported package metadata. All product SDK/facade acquisition
uses exact published c89systems assets/digests and independent prefixes. No
cpktdb/cpktmisc, CAI, LQL, MDF, Lockd, Pouch, Vectis, Python or XML Lua package is
a mandatory rotmos server dependency. The Lua C binding itself remains C89.

These versions are compatibility baselines verified against the recorded source
tags, not a continuing claim of latest availability. Before implementation locks
SDK inputs, check current published c89systems releases and prefer the newest
compatible complete C/Lua closure, pinning exact asset identities and digests.
A newer version still has to satisfy the public interop and behavioral gates;
a version number alone does not discharge those requirements.

Ship `rotmos-lua-<version>.tar.gz`, `rotmos-<version>-1.rockspec` and
`rotmos-<version>-1.src.rock`. Include all facade/binding sources, needed public
interop declarations/generators, licenses, version and manifest; use public
logical release URLs. The rock builds against the selected installed shared
rotmos SDK through CMake/imported targets, with explicit Lua/dependency/compiler
selection and state under build/. No sibling source or host-library fallback.
Keep this source/package-manager product out of per-target C SDKs; do not bundle
a second Lua interpreter or dependency implementation implicitly. Include all
Lua artifacts and nested payload checks in release checksums/privacy evidence.

Test the module against an installed SDK with the matching Bootlin-built Lua
runtime on Linux and selected native/cross Darwin evidence. Inspect actual module
imports, include closure, loaded shared-library identity and once-only runtime
state; a symbol scan alone is not the whole gate. Include an installed C host
which creates one Lua VM, preloads/requires rotmos, serves and stops, verifies
its own globals remain intact and closes that same VM itself. Prove route/config parity,
binary data, schema ownership/GC, callback errors, cancellation, fork roles,
backpressure, JSON/XML flow, proxy/DAV behavior and finalizers. No required test
may pass by using a source-tree module or privately linked runtime copy.

An embedded Lua VM is supported by the shared binding; embedding is independent
of whether a product is packaged as a source rock or a native application.
Vectis's existing fully static/packed CLI may keep its application Lua facade over
its public C adapter and one selected static rotmos core. Loading this shared
module beside that static core would create a second runtime and is unsupported.
Direct `require("rotmos")` inside that fully static product is a separate
composition decision, requiring a proved single shared closure or a deliberately
specified single-runtime static embedding product. This spec does not claim
that packed composition already works. The Vectis cutover must preserve its
existing Lua facade/packed behavior without a second VM or private runtime copy.

## 21. Patch preservation ledger

Numbers refer to exact filenames in [series](../patches/kore/series). Build-only
patches are preserved as provenance even where CMake replaces the upstream
Makefile path. Their presence is not proof that a runtime feature guard is enabled.

| Patch | Required disposition / verification |
| --- | --- |
| 0001 | pslog logging and independent bundle build knobs; single logger provider and parent/worker logging |
| 0002 | LoneJSON request helpers and ACME parsing; preserve upstream parser ownership and error/rewind behavior |
| 0003 | curl toolchain types; build all selected targets with correct widths |
| 0004 | Clean version-object generation; CMake owns equivalent generated provenance |
| 0005 | HTTP-only operation without DH parameters |
| 0006 | Direct TLS path; configure through the backend, not Vectis globals |
| 0007 | HTTP/worker guardrails; defaults, limits, min-rate and timing propagation |
| 0008 | Lifecycle portability; Linux and Darwin lifecycle evidence |
| 0009 | 64 KiB HTTP header limit; size and boundary regressions |
| 0010 | Static runtime callbacks; replace Vectis symbol table with typed registration |
| 0011 | Direct TLS without mandatory DH parameters |
| 0012 | Embedded no-seccomp selection; no implicit embedding-parent policy change |
| 0013 | Autoblock hooks; replace Vectis calls with live admission/observation hooks |
| 0014 | Embedded shutdown; return to caller and release owned runtime state |
| 0015 | Bounded child shutdown and escalation |
| 0016 | Structured pslog access sink independent of file logging |
| 0017 | Unknown HTTP host 404 behavior |
| 0018 | Embedded restart cleanup; no residual domains/timers/loggers |
| 0019 | Preserve termination signal against later events |
| 0020 | Backend curl CA environment/trust handling |
| 0021 | ACME existing-key/account recovery |
| 0022 | Parent timers; replace Vectis hook with parent-loop integration |
| 0023 | Unknown TLS SNI rejection |
| 0024 | PROPFIND/MKCOL/COPY/MOVE support and body/method behavior |
| 0025 | Preserve explicit content type across file references/cache |
| 0026 | ACME persistence before certificate distribution; generic callback with propagated failure |
| 0027 | Access-log categories and exact status-level overrides |
| 0028 | Bound worker pslog messages and validate received lengths |
| 0029 | Preserve pipelined request bytes across fixed-body boundaries |
| 0030 | Prebody handoff before storage/consumption; surplus ownership and local rejection |
| 0031 | Drain readable bytes before read-half-close |
| 0032 | Exact Host header-name match |
| 0033 | Reject malformed header blocks and avoid split-array truncation |
| 0034 | Release TLS domain key references on cleanup |
| 0035 | Future write-start timestamp does not underflow timeout arithmetic |
| 0036 | Wake for connection timeout sweep even without application I/O |
| 0037 | Validate header length before pipeline subtraction/split |
| 0038 | Initialize IPC header padding |
| 0039 | Worker exit ownership; separate pre-backend teardown and final exit callbacks |
| 0040 | Release worker accept lock on clean exit |
| 0041 | Static pool-name ownership; cleanup without freeing static names |
| 0042 | Independent cpkt/LoneJSON/pslog prefixes and relocatable closure |

## 22. Investigation references

Vectis paths below are relative to this spec in the inspected sibling checkout;
the baseline commit above fixes their meaning. They are evidence sources, not
build inputs or permission to modify the sibling repository.

| Evidence | Relevant sources |
| --- | --- |
| Public controls/body/TLS/request/response contract | [vectis.h](../../vectis/include/vectis/vectis.h): vectis_server_config, vectis_tls_config, vectis_body_policy, request/response declarations |
| Normalization, body-limit derivation, readers and outer fork | [vectis.c](../../vectis/src/vectis.c): vectis_effective_server_config, vectis_app_max_request_body_bytes, vectis_app_make_kore_runtime_config, vectis_app_start_impl, upload_reader implementation |
| Lua reader-thread and producer lifetime seams | [vectis_cli.c](../../vectis/src/vectis_cli.c): vectis_lua_app_upload_dispatch, vectis_lua_app_dsv_dispatch, vectis_lua_app_callback_pcall, vectis_lua_response_callback_source |
| Generic proxy/WebDAV/XML extraction | [proxy.h](../../vectis/include/vectis/proxy.h), [webdav.h](../../vectis/include/vectis/webdav.h), [xml.h](../../vectis/include/vectis/xml.h), [vectis_webdav.c](../../vectis/src/vectis_webdav.c), [vectis_xml.c](../../vectis/src/vectis_xml.c), [Lua XML contract](../../vectis/manual/lua-xml.md) |
| Native runtime configuration, body delivery, streaming and hooks | [vectis_kore_bridge.c](../../vectis/src/vectis_kore_bridge.c): apply_server_config, parent_configure, body_chunk, send_response, response_stream_next, internal_kore_run/validate/stop |
| Internal configuration and hooks | [vectis_internal.h](../../vectis/src/vectis_internal.h), [vectis_kore_hooks.h](../../vectis/src/vectis_kore_hooks.h) |
| HTTP/SSE admission and full duplex | [vectis_kore_proxy.c](../../vectis/src/vectis_kore_proxy.c), [vectis_proxy_upload.c](../../vectis/src/vectis_proxy_upload.c) |
| TLS/raw WebSocket relay and streamed rejection | [vectis_kore_proxy_ws.c](../../vectis/src/vectis_kore_proxy_ws.c), [vectis_proxy_ws_rejection.c](../../vectis/src/vectis_proxy_ws_rejection.c) |
| Commitment and local response | [vectis_kore_proxy_local.c](../../vectis/src/vectis_kore_proxy_local.c), [test_kore_proxy_unwritten.c](../../vectis/tests/unit/test_kore_proxy_unwritten.c), [test_kore_proxy_unwritten_live.c](../../vectis/tests/unit/test_kore_proxy_unwritten_live.c) |
| Worker loop/curl integration and event retirement | [vectis_proxy_events.c](../../vectis/src/vectis_proxy_events.c), [vectis_proxy_curl.c](../../vectis/src/vectis_proxy_curl.c) |
| ACME storage contract | [vectis_acme_state.h](../../vectis/src/vectis_acme_state.h), [vectis_acme_state.c](../../vectis/src/vectis_acme_state.c), [test_vectis_acme_state.c](../../vectis/tests/unit/test_vectis_acme_state.c) |
| Runtime/feature guards and shared-symbol boundary | [CMakeLists.txt](../../vectis/CMakeLists.txt), [vectis.exports](../../vectis/cmake/vectis.exports), [runtime surface audit](../../vectis/docs/kore-runtime-surface-audit.md) |
| Product proxy and process contracts | [reverse-proxy-spec.md](../../vectis/docs/reverse-proxy-spec.md), [service-runtime-lifecycle-spec.md](../../vectis/docs/service-runtime-lifecycle-spec.md), [production-webserver-hardening.md](../../vectis/docs/production-webserver-hardening.md) |
| HTTP pipeline/prebody/parser regressions | [test_kore_pipelining.c](../../vectis/tests/unit/test_kore_pipelining.c), [test_kore_prebody_handoff.c](../../vectis/tests/unit/test_kore_prebody_handoff.c) |
| TLS direction, half-close, restoration and queued streams | [test_kore_proxy_direct_io.c](../../vectis/tests/unit/test_kore_proxy_direct_io.c), [test_kore_response_write_timeout.c](../../vectis/tests/unit/test_kore_response_write_timeout.c) |
| Real runtime and response framing | [test_vectis_runtime.c](../../vectis/tests/unit/test_vectis_runtime.c), [test_vectis_https_runtime.c](../../vectis/tests/unit/test_vectis_https_runtime.c), [test_vectis_https_mtls_runtime.c](../../vectis/tests/unit/test_vectis_https_mtls_runtime.c) |
| Proxy memory/admission/ordering | [test_kore_proxy_h2_memory_live.c](../../vectis/tests/unit/test_kore_proxy_h2_memory_live.c), [test_kore_proxy_admission_live.c](../../vectis/tests/unit/test_kore_proxy_admission_live.c), [test_kore_proxy_overlap_live.c](../../vectis/tests/unit/test_kore_proxy_overlap_live.c) |
| Proxy WebSocket contract | [test_kore_proxy_ws_live.c](../../vectis/tests/unit/test_kore_proxy_ws_live.c), [test_kore_proxy_ws_rejection_live.c](../../vectis/tests/unit/test_kore_proxy_ws_rejection_live.c), [test_kore_proxy_wss_live.c](../../vectis/tests/unit/test_kore_proxy_wss_live.c) |
| Lua and packed/server lifecycle seams | [lua-kore.md](../../vectis/manual/lua-kore.md), [server_websocket.lua](../../vectis/tests/lua/server_websocket.lua), [http_uploads.lua](../../vectis/tests/lua/http_uploads.lua), [server_restart.cmake](../../vectis/tests/lua/server_restart.cmake), [server_signal_shutdown.sh](../../vectis/tests/lua/server_signal_shutdown.sh) |

The patched Kore http.c, websocket.c, worker.c, kore.c, runtime.c and
keymgr_openssl.c were also inspected in the generated tree to trace the active
patch composition. The authoritative tracked inputs remain the pristine source
and external series; generated paths are intentionally not documentation links.
