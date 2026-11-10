# Lua server facade

Editor types and per-operation hover comments ship in
[`knurl.meta.lua`](../lua/knurl.meta.lua); the [facade source](../lua/knurl.lua)
attaches them to the exported module and dynamically registered methods. Add the
installed `share/lua/5.5` directory to your Lua language server library paths
when working outside this source tree. This metadata contains no runtime code.

`require("knurl")` uses the host's Lua 5.5.1 state. The host creates, configures
and closes that state. The separately built C binding links the installed public
shared libknurl API and the same shared Lua runtime as the host. It includes no
Kore declarations, core-private headers, alternate router or transport executor.
`<knurl/lua.h>` belongs to this binding product; it is absent from the C SDK.

The binding covers server declaration/run/stop/close, configuration and serving
state, manual TLS/HTTPS/redirect listeners, lifecycle/policy callbacks, route counts,
ordinary routes, groups, callback auth, request lookups/events, buffered bodies,
body readers/materialization, live and file uploads, static files/directories,
filesystem WebDAV, reverse-proxy configuration/callbacks, WebSockets, SSE,
loop registrations, supervision, observations, staged bytes/files and live response sources. Its
behavior comes from Vectis's [callback routes](https://github.com/sa6mwa/vectis/blob/6b1e263e0436687f68a7768ca55f4361a6e59e19/manual/lua-app.md)
and [existing bridge](https://github.com/sa6mwa/vectis/blob/6b1e263e0436687f68a7768ca55f4361a6e59e19/src/vectis_cli.c), rather than a new server
framework. The [integration contract](vectis-integration-contract.md) remains
the complete replacement requirement.

Integer-valued fields use Lua's checked conversion, matching Vectis: exact whole
floats and numeric strings are accepted within the field's sign/range limits.
Fractions, nonfinite values and overflow are rejected. Exact unsigned counts
above Lua's signed range use `knurl.count`. Method names keep their string
meaning; SSE retry retains its explicit integer/count requirement.

The [buffered REST facade](rest.md) also lifts Vectis's server-side
`knurl.rest.route/group/json/error_response` workflow. [OpenAPI](openapi.md)
binds `server:openapi_doc/openapi` and route annotations to the same public C
router, with retained public LoneJSON schema owners and bounded JSON/YAML output.

```lua
local knurl = require("knurl")
local server = assert(knurl.server {
  port = 8080,
  workers = 1,
})
assert(server:route {
  path = "/hello/:name",
  methods = {"GET", "POST"},
  body = {mode = "buffered", max_bytes = 4096},
  handler = function(request)
    return {status = 200, body = request.param("name") .. "\n"}
  end,
})
assert(server:run())
assert(server:close())
```

The facade accepts the existing `route{...}` and `group{prefix/path_prefix,
routes={...}}` forms. Group declarations copy shared defaults; each route can
override them. Method strings and arrays map to the public C method mask;
ordinary routes default to GET. Path parameters and leading-`^` regular
expressions retain Vectis's inference. Verb helpers forward to the same registry.
The separate `server:group(prefix, hooks)` form returns a public C group wrapper.
Registration returns true, or `nil, error`; it does not start the server.

`server:text{...}`, `:json{...}`, `:redirect{...}` and `:auth_json{...}`
(also on groups) register ordinary routes with copied fixed response values.
Text defaults to `ok\n`, JSON to `{"ok":true}\n`, and redirects to an empty
302 with required `location`. Fields are `path`, `method`/`methods`,
`status_code`/`status`, `body`, `content_type`, `cache_control` and `openapi`;
authenticated JSON requires `auth` and ignores `cache_control`, as in Vectis.
These fixed helpers preserve the baseline C-string boundary at the first NUL.
JSON bodies are already encoded bytes and are forwarded unchanged. Use typed
routes or `knurl.rest.json` for JSON encoding.

Before hooks may short-circuit. Before/after hooks return nil/true to continue
or a response table to select/replace; false and strings are invalid, as in
Vectis. After hooks receive the original handler value
and may retain, mutate or replace it. Only the final selected declaration is
converted, under protection that includes response/source getters and allocation.
Nil means 204; strings and numbers produce text. Response declarations accept
`status_code`/`status` in 100..599, headers, content type and body/file/stream
source. Repeated
headers can use an ordered array of `{name=..., value=...}` pairs.

Native route helpers supply their already staged result to after hooks as a
borrowed reply view. `reply:snapshot()` copies status, payload kind, content type,
ordered headers and a bytes payload when present. File/source payloads remain
unread. `header_set(name, value)` / `header_add(name, value)` use public builders;
`replace(declaration)` uses the ordinary response converter. Returning nil, true
or that view preserves the native result; returning an ordinary Lua declaration
replaces it and passes that declaration to subsequent group hooks. The view
expires at the end of its individual callback, including on exception or OOM.
Saved views return STATE; a subsequent after hook receives its own view.

`spooled_source` is Vectis's explicit file-backed alternative to `stream_source`.
The binding calls public `knurl_reply_source_spooled`, closes the input once
after synchronous consumption/failure, and leaves the staged file to ordinary
response cleanup. It uses `limits.body_spool_directory` when configured,
otherwise the public helper's `/tmp` default. Optional reset runs once; legacy
EOF/numeric chunks and oversized-chunk remainders retain their meaning. The
operation can block; AGAIN fails rather than collecting a paused producer.
Spooling still occurs for HEAD before output suppression, matching Vectis's
builder timing. Unselected declarations acquire no source or file ownership.

Legacy `stream_source` tables retain `read(max_bytes)`, optional `reset()` and
`close()`. Optional reset runs once immediately before the first live pull,
after upload completion/close and after final reply selection. Reads accept
strings/numbers and nil/false/empty-string EOF, retain only an oversized chunk's
remaining suffix, and drain it before another callback. Delivery uses the public
source executor with bounded transport chunks; it creates no full-body buffer or
temporary response file. `knurl.source{...}` directly exposes the explicit
producer convention: bytes or `nil, knurl.EOF/AGAIN`, and `close(cause)`.

Request tables copy scalar metadata and retain scoped lookup helpers. A saved
lookup returns `nil, error` after request close. Binary bodies preserve NUL.
Ordered headers/query pairs and first-value lookups use public request APIs.
Absent values return nil. The backend retains Vectis's rejection of empty wire
header fields; decoded empty query values remain distinct from absence.

Routes accept `body="materialized"` or a policy table with `mode`, `max_bytes`,
`memory_buffer_limit_bytes` and `disk_spool_disabled`. Counts use the same exact
unsigned conversion as the public C API. File-backed request bodies expose
`body_spooled`, `body_path` and `body_size`; `body` remains nil. Their transport
files are removed on request close. `request.body_reader()` returns a bounded,
resettable public source wrapper which expires with the request, including its
independently opened descriptor. Saving the Lua wrapper does not extend that
scope. `source:read(capacity)` returns bytes or `nil, knurl.EOF/AGAIN`;
`source:reset()` and `source:close()` follow public ownership checks.

Explicit `request.body_materialize(options)`, `body_spill(options)` and
`body_copy(capacity)` reuse the public C materializers. Options are `capacity`
(a Lua-owned caller buffer), `memory_limit_bytes`, `directory` and `prefix`.
Materialize/spill use the configured server spool directory when omitted;
otherwise the public helper's `/tmp` default applies. These operations explicitly
copy a completed body and can block. They do not apply to live upload input.
The independent result has `snapshot()`, `reader()` and `close()`. Its snapshot
contains kind, exact length and memory bytes or file path. Readers pin the result;
closing a result while readers remain open fails with STATE. Closing frees
memory/path metadata but leaves a returned file as a caller artifact: save its
path and explicitly remove it when finished.

`server:upload{path=..., ...}` and `group:upload{...}` lift Vectis's live Lua
upload callbacks through public `knurl_router_upload_stream` / group equivalent.
The default method is POST. `open(request)` (alias `on_open`) returns state;
omission supplies a fresh table. Required `on_chunk(request, bytes, state)`
(aliases `chunk`, `write`) accepts nil/true for the entire offered chunk. It also
accepts a positive consumed prefix; the binding schedules remaining delivery.
`nil, knurl.AGAIN` pauses until `request.body_resume()` schedules another turn.
`buffer_bytes` bounds each Lua chunk, and `max_body_bytes` sets the admitted
public body limit. Input flows on the owning Lua thread without a whole-body
buffer, spool, native Lua thread or second queue.

Optional `on_complete(request, state)` (aliases `complete`, `finish`) returns an
ordinary response, defaulting to 204. Normal auth and before/after hooks apply;
the same request table and state persist through all callbacks. `body` is nil
and `body_streaming_upload` is true. `close(request, state)` (alias `on_close`)
runs exactly once after successful open, on success or failure. Open failure
owns its own partial cleanup. Successful close follows final response conversion
and precedes network output/live source reset. A close exception turns an
otherwise successful upload into a server error; cleanup cannot replace an
earlier failure. Admission/auth rejection opens no upload state.

`server:upload_file{path=..., file_path=..., max_body_bytes=...}` (also on a group)
uses the public native file upload executor. `file` aliases `file_path`. POST is
the default; method(s), path kind, upload body policy and auth/before/after hooks
are supported. The explicit file opens/truncates after admission, is written
incrementally, and remains a caller artifact, including partial output on error.
Success is Vectis's 200 text response containing the uploaded byte count. No Lua
callback runs on the native file worker.

`static_file{path=..., file_path=...}` and
`static_directory{path_prefix=..., root_dir=..., index=true}` work on a server or
group. File accepts `file`; directory accepts `prefix`, `root` or `dir`. The
canonical low-level keys are `path`, `root_dir` and `index_enabled`. Directory
paths default to `/`; indexes default disabled. GET/HEAD masks, content/cache
type policy, index file and optional not-found body/type map to public static
configuration. File opens, confinement, range/conditional handling and slash
redirects remain in the existing C asset engine. Registration copies declarations
and performs no file lookup. Ordinary auth and route hooks apply.

`http.mime_types={mp3='audio/mpeg',custom='application/vnd.example.resource'}`
sets instance-wide static extension overrides; each static declaration accepts
its own `mime_types` dictionary. Explicit route `content_type` wins, followed by
route mappings, instance mappings, provider metadata and built-in inference.
Keys are ASCII case-insensitive with an optional leading dot; duplicate normalized
keys fail. Mappings are copied, so later Lua mutations do not affect serving.
`server:content_type(filename)` uses the public Core instance resolver without
opening a file. See [MIME resolution and examples](assets.md).

`webdav{path_prefix=..., cache_dir=..., site_id=..., root_dir=..., auth=...}` uses
the public filesystem WebDAV mount, also on a group. `prefix` aliases the mount
path, default `/`. File/total/resource quotas, body limit, content/cache policy
and operation timeout/disable fields bind C. Omitted body limits forward the file
quota; explicit body limits retain their override/zero-inheritance meaning.
The extra operation deadline is disabled by default. Explicit timeout values
enable it unless disabled explicitly. Deferred-response deadlines are also
disabled by default; explicit server timeout declarations enable the C deadline.
Vectis's
`auth_required=true` and `conceal_unauthorized=true` defaults map to the public
disabled flags; there is no implicit XDG/cache location. The callback provider
receives each relative resource path, including child visibility and mutation
checks. Its decoded decision stays rooted until the native engine copies it.
WebDAV owns authorization, conceal/challenge/redirect behavior and principal scope;
the binding adds no second mount auth gate. Request principal metadata refreshes
after mount authorization and retains the WebDAV public 254-byte bound. Native
storage work never accesses Lua. Route hooks may inspect/replace staged results.

`server:websocket{path=..., connect=..., message=..., disconnect=...}` retains
Vectis's callback shape; `handler` aliases `message`. Message is required.
The binding registers a GET headers handler and calls public
`knurl_websocket_accept`, rooting connection userdata before acceptance. Failed
acceptance releases that root; successful acceptance keeps it until terminal
close. Path kind, protocol, copied handshake headers, maximum frame/output queue
sizes and close timeout map to the public WebSocket configuration. The path kind
defaults to literal. This raw headers route does not implicitly run ordinary
auth/before/after middleware, matching the public C route contract.

The connection exposes `send(opcode, payload)`, `send_text`, `send_binary`,
`close()`, explicit `close(code, reason)`, `resume`, `state` and `queued_bytes`.
No-argument close queues the baseline empty frame; explicit code/reason selects
the bounded close-handshake helper. Constants are under
`knurl.websocket`. Sends accept an entire frame or return `nil, knurl.AGAIN`
without queuing it. Optional `writable(ws)` retries after native capacity becomes
available. `message(ws, opcode, payload)` consumes on nil/true, or holds the
complete frame on `nil, knurl.AGAIN`; `resume()` schedules redelivery.
`disconnect(ws, outcome)` retains the legacy first argument and adds the copied
public terminal outcome, including any first callback error. The same userdata
can be retained during its connection lifetime and becomes inert after terminal
close. Closed sends/resumes fail with STATE; orderly close is idempotent.
Callback exceptions/OOM follow native connection failure and cleanup; no framing,
message assembly or output queue is implemented in Lua.

`server:sse{path=..., open=..., read=..., close=...}` is the existing ordinary-route
facade over a live source. `open(request)` supplies state (nil when omitted),
`read(state, capacity)` returns a string/event or nil/false EOF, and `close(state)`
runs when the selected source closes. Normal methods/auth/before/after hooks also
apply, including `group:sse`. The default type is
`text/event-stream; charset=utf-8`; absent cache-control/accel-buffering headers
receive `no-cache`/`no`, including with ordered header arrays. Status/type/header
overrides retain the ordinary response contract.

`knurl.sse_encode(event, capacity)` calls public `knurl_sse_encode` with one
bounded output buffer. Strings/numbers become data; event tables accept comment,
id, event, exact unsigned retry, data or a preformatted frame. Frame takes
precedence without reading other getters. An oversized event fails instead of
splitting it across producer reads. This buffers one bounded event, never the
stream. Optional `nil, knurl.AGAIN` from read pauses; a loop callback can use the
still-live request's `producer_resume()` to schedule another pull.

`server:defer`, `timer`, `watch` and `waker` bind public loop registrations. They
accept `{callback=..., retired=...}`; timer adds `delay_ms`/`repeat_ms`, and watch
requires `fd` with optional `interest`. Defer/waker also accept a callback
function directly. Registrations return an owned token with `close()`/`cancel()`;
watches add `update(interest)`, wakers add `signal()`. Flags are under
`knurl.loop`, and `server:now()` returns the exact public monotonic count.
Descriptors remain caller-owned and must already suit nonblocking delivery.
Callback arguments are token (plus ready flags for a watch). Exceptions cancel
that registration and preserve the first error. `retired(token, error_or_nil)`
runs at actual native retirement, under protection. A later token close returns
the preserved error when present. Deferred cancellation suppresses its Lua
delivery while retaining ownership through the scheduled C retirement.

Loop tokens stay rooted through retirement and fail cleanly beyond their owner
process/run. The process-start hook invalidates inherited parent registrations
and releases their copied roots after the C engine discards the parent loop;
it does not replay parent retirement callbacks in the worker. Process/run checks
precede native handle access. There is no Lua scheduler or separate event queue.

`server:proxy{path=..., target=...}` binds public `knurl_router_proxy` and
Vectis's [proxy callback contract](https://github.com/sa6mwa/vectis/blob/6b1e263e0436687f68a7768ca55f4361a6e59e19/src/vectis_cli_proxy_lua.c).
It accepts method(s), explicit `path_kind` (default literal), up to 15
`alternate_targets`, `upstream_http_version="auto"/"http1"`, connect/idle/total
timeouts and buffer limit. Target strings and TLS PEM inputs (`tls_ca_pem`,
paired `tls_client_cert_pem` / `tls_client_key_pem`) are validated/copied by the
public registration path. Declaration performs no connection. The binding adds
no body reader or queue to the C proxy's HTTP/SSE/WebSocket transports.

`preflight(inbound)` may return nil/true to continue, or a local response table
with integer `status`, optional binary `body` and ordered `headers` pairs. The
existing limits are 64 KiB of copied local body and 100 headers.
`rewrite(inbound, outbound)` receives copied method/raw path/raw query/host,
WebSocket flag and ordered headers. `inbound:param(name)` performs a scoped
capture lookup. Outbound provides `select_target(index)` (one based),
`set_method(name)`, `set_path(raw_path)`, `set_query(raw_query_or_nil)`,
`set_host(host_or_nil)`, `add_header`, `set_header` and `remove_header`.
Both dot and colon calls retain Vectis's behavior. Changing copied metadata has
no native effect. Setter arguments are validated before use; a failed setter
rejects the callback even if its nil/error result is ignored. No private proxy
failure field is accessed.

`modify_response(response)` receives a copied status and ordered headers, with
`set_status`, `add_header`, `set_header` and `remove_header` methods. It has no
body access. Rewrite and response callbacks return nil/true; other values fail.
`on_error(cause)` receives a table with `status`, `code` and `message` containing
the gateway default and copied cause,
and can return the same local response declaration. The C engine retains its
400/500 setup mappings, gateway-hook dispatch for 502/504, default gateway
fallback and abort behavior after commitment.

All proxy callbacks run synchronously on the owner loop and cannot yield.
Helpers expire immediately when their callback returns, before local response
getters run; saved helpers return STATE. A temporary registry root keeps the
scope alive through exceptions/OOM so native cleanup can always invalidate it.
The entire metadata construction, callback, getter and result conversion runs
inside the existing protected boundary. Request/response body flow, TLS, SSE
and WebSocket framing/relay remain in the public C proxy implementation.

`knurl.count(decimal_text)` or `knurl.count(high, low)` represents unsigned
64-bit quantities exactly. Values fitting Lua's integer range are integers;
larger values are immutable userdata with high/low words, comparison and decimal
formatting. Sizes reject fractions, negatives and overflow before reaching C.
Ordinary failures return `nil, error` with native status/source/operation and
copied bounded diagnostics. Source cleanup preserves the first callback failure;
close exceptions do not escape the native cleanup boundary.

Core configuration groups are copied, with currently bound fields checked by
name/type/range. Unknown or unbound fields fail rather than silently changing
behavior. The facade's default signal mode is `library`; embedding hosts owning
signal delivery select `signals={mode="host"}`. Lifecycle callbacks receive the
facade server and a copied public process-role snapshot.

`workers`, `io`, `limits`, `http`, `timeouts`, `proxy`, `client_ip.trusted_proxies`
and `autoblock` map directly to their public C declarations. Deadlines accept a
nonnegative millisecond integer or `{milliseconds=..., disabled=...}`. Zero
inherits the C default. `workers.death_policy` is `restart` or `terminate`.
`profile="production_webserver"` applies the public production initializer before
overrides. `server:effective_config()` copies normalized values and requested
deadlines; editing the snapshot cannot change the server. It includes configured
TLS memory bytes, so callers control how that configuration is handled.

Set `http={server_header="vectis"}` when constructing the server to configure
the emitted `Server` header (C: `config.http.server_header`). The value defaults
to `"knurl"`, is copied, and accepts 1..53 bytes valid in an HTTP header value.
The top-level `name` identifies the instance independently.

Listeners accept `kind="http"`, `"https"` or `"redirect"`, separate bind/presented
ports, `redirect_https_port` and a one-based `identity_index`. `tls.identities`
contains explicit domains and manual bundle or certificate/private-key inputs,
optional chain/client CA, and `require_client_certificate`. A material is a file
path string, `{kind="file", path=...}`, `{kind="memory", bytes=...}` or
`{kind="callback", open=function() ... end}`. Omitted kind follows the supplied
path/bytes/open field. An opener returns an unowned `knurl.source` or the existing
legacy source table, or nil/error. Identity mode defaults to manual.
TLS versions are `default`, `both`, `1.2` and `1.3`. Validation and run use the C
TLS material path; declaration itself opens nothing. `start_paused=true`,
`server:state()` and `server:set_serving(boolean)` expose the public serving gate.
The [readiness contract](processes.md#readiness-and-serving) also applies to Lua
callbacks: explicitly open serving before starting clients that must complete
application requests inside `on_parent_ready`.

ACME identities use `mode="acme"` and an explicit `tls.acme` declaration with
email, directory_url, runtime_directory, optional CA inputs/persistence deadline
and `state(server, context)`. The copied context identifies restore/persist/error,
parent/key-manager process and configured Kore runtime directory. Durable storage stays with
the host. The [TLS callback contract](tls.md) describes source ownership,
reentry, failure handling and publication order.

Callback auth retains Vectis's `{kind="callback", callback=function(request)...}`
declaration, including optional nested `provider`, purpose/realm and
`allowed_modes`/`modes`. The callback receives authorization, purpose, resource and
mode flags. Its decision table selects `allow`, `deny`, `required` or `redirect`,
with optional principal/status/body/content type/location/challenge fields.
Allow stages `cache-control: no-store` and supplies the existing ordinary-route
principal view (at most 127 bytes). The other actions short-circuit before hooks
or handlers, retaining 403/401/302 defaults and binary response bytes. Invalid
decisions follow Vectis's 400 mapping; exceptions/getter/allocation errors produce
500. The whole conversion is protected. Native credential stores and browser
sessions stay in Vectis: it registers its existing decisions through the public
C auth hook or a callback provider. libknurl implements no credential/session store.

## Building and embedding

```sh
make lua                    # Selected x86_64-linux-gnu toolchain, build/debug/lua.
make lua-all                # All seven explicit lifecycle targets, serially.
ctest --test-dir build/debug/lua --output-on-failure
```

`make lua` first builds Core and installs its SDK under the selected build tree.
The independent Lua CMake project discovers that installed shared SDK plus the
selected dependency SDKs. Its compilation database is `build/debug/lua`, matching
the Lua paths in `.clangd`. Neither the build nor the shipped binding owns an
interpreter. Binding install layout is `lib/lua/5.5/knurl/native.so`,
`share/lua/5.5/knurl.lua`, `share/lua/5.5/knurl.meta.lua`, its embedding header
and `knurl_lua` CMake package. LuaRocks installs the same metadata at
`share/lua/5.5/knurl/meta.lua`.

Embedding applications may link `knurl::lua` and call
`knurl_lua_preload(existing_state, &error)` before `require("knurl")`. The host
must open the base, package and string libraries. Registration is idempotent and
rejects conflicting loaders/loaded modules. It neither opens host libraries nor
changes unrelated globals. A standalone Lua host instead uses the installed Lua
and C module search paths.

`knurl.json` and `knurl.xml` expose `serialize(options)`, `source(options)`
and `write(options, sink)`. They use the installed C codecs and LoneJSON's
released public Lua interop. Options select `schema` and `value`/`table` input;
the explicitly materialized `serialize` also accepts external `record` userdata
and optional `max_bytes` (zero adds no output ceiling). XML mapping options
match the public C config, including root/text/attribute names and limits.

The callback and source forms assign tables into private records before output,
preserving Vectis's writer contract. Mutating the caller's table/options or an
unrelated record, collecting objects, or running an independent writer cannot
change an active producer. Spooled fields use public schema assignment to prepare
private input storage. Output is generated in bounded chunks, without collecting
or staging the whole output. Mutable external records are rejected with the
existing direction to use buffered serialization.

`write` synchronously delivers chunks of at most 4096 bytes to a function or
`{write=function(bytes) ... end}` and returns `true, byte_count`. It borrows the
sink without closing it. Exceptions, false and `nil, error` stop generation,
return a structured error and release the private input. `source` returns the
ordinary public Source wrapper; pass it as `stream_source` in a response, read
it manually, or close it to cancel. Its schema/record owners survive until native
producer cleanup. It is not resettable. The
[writer fixture](../tests/lua/codecs.lua) ports Vectis's mutation/GC/reentry,
spooled-input and mutable-record cases, adding small pulls, cancellation, wire
GET/HEAD delivery, sink failures and allocation unwinding.
Like Vectis, option getters and a table sink's `write` function are snapshotted
before output; malformed assignment or throwing preparation getters raise Lua
errors. Sink invocation failures return the structured result described above.

Both codecs also expose `parse(options)` and `parse_record(options)`. Supply
`schema` and a `data` string (or `json`/`xml` for the selected codec), or `path`.
File input uses the public pinned-file Source and bounded reads. Parsing owns a
complete typed record under its field/spill limits; `parse` additionally converts
that record to a Lua table. `parse_record` returns independent LoneJSON userdata,
including spooled fields and captured JSON values, with no intermediate table or
JSON text round trip. XML mapping options retain Vectis's semantics and defaults.
`strict_unknown`, when present, takes precedence over `skip_unknown`.

Active requests expose `json_record(schema)` and `xml_record(options)`, with
`schema` included in XML options. These operate on completed BUFFERED/MATERIALIZED
bodies and rewind before and after parsing, including ordinary parse failure.
The returned record remains valid after request close; saved request methods
return STATE. JSON uses the provider's public Lua-owned record parser over the
public body reader, retaining its captured-value and spool policy. XML fills
Lua-owned record storage through the public request codec.

Servers and groups expose `json_route`, `json_typed_route` and `xml_route`.
Declarations use ordinary path/method/body/hooks, optional `input_schema`, and
`handler(request, input_record, ...)`. Their default method is POST. JSON uses
JSON input with the normal memory-threshold/spool policy; XML requires
an input schema and live STREAMING_UPLOAD/STREAM input. XML `buffer_bytes` bounds
the reader queue (zero selects 256 KiB), without collecting a transport body.
Schema arguments are checked during registration. Auth and before hooks finish
before decoding. These bind the public C typed registrations: their after hooks
receive a scoped native reply view (`snapshot`, `replace`, header edits), after
the handler's selected response has been prepared. Ordinary JSON bytes are
already serialized then. Ordinary non-typed routes retain declaration hooks.

`json_route` additionally accepts `output_schema` and `max_output_bytes`. Its
handler receives `handler(request, input_record, output_record)` and populates
the output with LoneJSON's public operations. A successful handler selects a 200
materialized JSON reply, or 204 without an output schema. Zero/omitted output
bound adds no limit. The input/output records can use different schemas. The
explicit materialized reply finishes serialization before asynchronous delivery,
so retaining or later mutating a Lua record cannot change the selected bytes.
The public typed-record provider supplies Lua-owned records through released
LoneJSON interop. Its owner-loop acquire/release callbacks pin records until C
finishes using them, including XML native acknowledgement after cancellation.
Release drops the binding's reference; records retained by user code remain
valid. C performs parsing, handler dispatch, ordinary output serialization and
hook ordering. The Lua facade only adapts the request argument.

`json_typed_route` and `xml_route` return ordinary response declarations, choosing
schemas per response. XML declarations accept an `xml` mapping-options table.
Any ordinary or typed handler may select `{json={schema=...,value=...}}` or
`{xml={schema=...,value=...}}` for bounded live output from privately assigned table
input. Explicit `{json_materialized={schema=...,record=...}}` and
`{xml_materialized={schema=...,record=...}}` accept mutable records or value tables,
plus `max_bytes`; they serialize synchronously; zero/omitted `max_bytes` adds no output ceiling.
Status, content type and headers remain ordinary response fields. Nil from a
general typed handler selects 204. Typed handlers prepare their selected reply
before after-hooks, matching the C operations. After-hook replacement retires
that reply normally. Standalone ordinary Lua routes still delay declaration
conversion until their after-hooks finish.

```lua
server:json_route{
  path='/echo', input_schema=request_schema, output_schema=response_schema,
  handler=function(request, input, output)
    response_schema:assign(output, {message=input.message})
  end,
}
```

The [typed fixture](../tests/lua/typed.lua) covers records, captured JSON,
file/spooled input, repeated body decoding, owner lifetime, copied declarations,
group/hook order, auth short circuit, multiple output schemas, mapped XML,
bounded live output, malformed input, allocation failures and staged replacement.

`make lua` acquires matching checksum-pinned LoneJSON and pslog Lua source
products into the checkout's dependency cache. The LoneJSON provider is compiled
with the selected lifecycle toolchain into its own dependency SDK prefix,
`.cache/sdk/<target>/lonejson-lua`, against the installed shared C SDK and cpkt's
shared Lua. Its normal `lonejson.core` loader and the knurl C binding use one
shared provider instance. No dependency Lua source or interop header is added to
the C-only knurl SDK. Standalone hosts add the provider's installed facade and
module search paths; preloading knurl does not change host package paths.

The pslog Lua provider is built similarly into `.cache/sdk/<target>/pslog-lua`,
using its checksum-pinned source product and the installed public C SDK. Its
single C99 `static inline` hint is removed by a dependency-only C89 preprocessor
definition; the source archive is unchanged. `pslog.core` and knurl's interop
link resolve to the same shared provider.

`knurl.server{logger=log,access_log=policy}` and the inactive-server method
`server:set_pslog(log,policy)` bind `knurl_server_set_pslog`. The root and optional
`policy.logger` must be live pslog loggers with native sinks. Public pslog
check/ref/unref operations retain them through server teardown or replacement,
including coroutine collection; no userdata layout is imported. Failure keeps
the prior configuration. Lua output callbacks are rejected before retention.
Keep borrowed output files open through server use. Explicit logger close keeps
the retained native allocation safe but can stop output, under pslog's contract.

Access policy accepts Vectis's boolean opt-out or a table with `enabled`,
`success_level`, `client_error_level`, `server_error_level` and `status_levels`.
Levels and exact HTTP status overrides keep the baseline trace/warn/error
defaults and 32-rule bound. With no access logger override, the C runtime derives
`sub=http` from the root. `access_log=false` or `{enabled=false}` disables
structured access logs independently of `http.access_log_path`. Application
seed selection, service fields and category enablement remain host policy.
The [logging fixture](../tests/lua/logging.lua) covers retention, replacement,
failed conversion, GC/coroutines, native-sink validation, severity overrides,
derived and explicit loggers, and independent legacy file output.

File access-log rotation uses the public C configuration through
`http={access_log_path="access.log", access_log_max_bytes=10485760,
access_log_file_count=3}`. The count includes the active file; backups are `.1`
and `.2`. Zero size/count inherit those defaults. Set
`access_log_rotation_disabled=true` for unbounded append. Whole records stay
together, so one oversized record may exceed the threshold. File errors,
including ENOSPC, are reported through the root logger while HTTP keeps serving;
the failed record is dropped and later records retry. See the
[file logging contract](logging.md) for startup failures, ownership and recovery.

`knurl.xml.document(bytes, options)` lifts Vectis's owned document facade.
Options are `max_bytes` (16 MiB) and `max_xpath_nodes` (4096), with explicit
positive limits no greater than 2147483647. Documents expose `root()`,
`xpath(expression, namespaces)`, `serialize_materialized(formatted)` and `close()`.
XPath namespaces are prefix/URI maps of at most 256 entries; results are scalar
numbers/booleans/strings or arrays of element views. Expressions are bounded to
1 MiB. Non-element node sets are rejected by the C API.

Element views expose `name()` (local name, URI, prefix), `text()`,
`attribute(name, namespace_uri)`, `set_text(bytes)`,
`set_attribute(name, bytes, {namespace=...,prefix=...})`,
`append_child(name, {namespace=...,prefix=...,text=...})`, `remove()` and `close()`.
Every mutation invalidates prior views, including its receiver; newly appended
children are fresh views. Views retain their document until close/collection;
explicit document close invalidates them. Absent attributes return nil, while
empty attributes return an empty string. Owned byte/XPath results are retired
on scope exit, including Lua allocation failure. Document closure is guarded
through result conversion; independent unused views can still be collected.

`knurl.xml.sax(callbacks, options)` exposes `feed(bytes)`, `finish()` and `close()`.
Callbacks are copied: `start_element(name, attributes, namespace)`,
`end_element(name, namespace)` and `text(bytes)`. Attributes contain
`name/value/namespace/prefix`; namespace records contain `uri/prefix`, plus a
`declarations` array for starts. Metadata is copied before calling Lua. Events
arrive synchronously before input completion. Feed/text chunks are at most
65536 bytes, with no cumulative input-size bound or hidden DOM. The public
parser's token/depth limits still apply. Start events allow at most 256 combined
attributes/declarations and `max_metadata_bytes` defaults to 16 MiB.

SAX exceptions and `nil, error` stop the parser and retain the same error object
for subsequent operations. Other returns, including false, are ignored.
Callback construction and execution are protected; Lua errors cannot cross
libxml2. Reentry/close fails the active parse even when the callback catches the
failure. Independent nested parsers are supported. Finish is idempotent after
success. Close calls no callback and releases retained callback owners.

These objects preserve Vectis's Lua argument-error contract for malformed
arguments and explicitly closed receivers. Native failures and invalidated views
use the public knurl error format. Process/thread confinement, mutation,
DTD/external-resource rejection and parsing belong to the public C API.
Documents, views and SAX receivers also support Lua 5.5 `<close>` scopes.
The [ported XML fixture](../tests/lua/xml.lua) covers those workflows, bounded
delivery beyond 16 MiB, callbacks/reentry, both collectors, owner release and
allocation failures.

## Host providers

Hosts wrap public asset/storage providers with `knurl_lua_push_asset_provider`
and `knurl_lua_push_storage_provider` from the installed Lua interop header.
Success pushes one opaque value, retaining the optional Lua owner; failure
preserves the stack. The provider stays borrowed, and the binding never closes
it. Keep it usable through every borrowing server's destruction. Host teardown
closes providers after servers; owner finalizers can handle the final release.

`server:static_assets{path='/assets',provider=assets,...}` and its group helper
bind the public static-asset registration. Indexes default enabled for these
provider mounts, matching Vectis's packed mounts; `index=false` or
`index_enabled=false` disables them. Filesystem directory indexes retain their
disabled default. `server:webdav{provider=storage,...}`
selects the host storage provider instead of constructing a filesystem provider.
The route declaration pins the opaque value and owner until server close. Path,
method, index, type/cache, auth and hook options retain the ordinary C behavior.
The host supplies packed lookup and durable-store adapters; C handles ranges,
conditions, source output, DAV operations and listing.

The [public host module](../tests/lua/providers.c) supplies generated and memory
assets plus the C read-only storage adapter, using only public headers. Its
[mount fixture](../tests/lua/providers.lua) checks scoped ownership, registration
failure, binary data, grouped mounts, ranges without prefix reads, conditions,
HEAD suppression, bounded reads, PROPFIND and read-only mutation rejection.
An [isolated consumer](../tests/lua/consumer/CMakeLists.txt) builds that same
module against installed C/Lua SDKs and selected toolchains.

`make lua-package` ships the facade/binding source archive,
release rockspec and source rock with a checksum inventory. The
[source-package instructions](../lua/README.md) cover installed public SDK inputs,
toolchain selection, LuaRocks installation and embedded hosts. The full Vectis
reference adapter and completed development compatibility/artifact checks are
recorded in [verification](verification.md). Actual Vectis cutover and release
verification retain their separate gates.

## Selected JSON arrays

`knurl.json.array_each{schema=..., selector=..., callback=..., source=...}`
accepts exactly one `source`, `data` or `path`. A source is borrowed at its current
position, never reset or closed; AGAIN is an error for this synchronous operation.
`request.json_array_each{schema=..., selector=..., callback=...}` uses completed
request input and rewinds before and after. An empty selector selects the root
array; a nonempty selector selects a direct object member.

The binding reuses one public Lua-owned mapped record as Core's per-item storage,
then uses LoneJSON's public conversion for `callback(value, zero_based_index)`.
Each callback receives an independently converted item table. No whole array or
document is collected. A non-nil second callback result stops with an error;
success returns `true, count`.
