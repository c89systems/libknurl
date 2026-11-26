# Run a Knurl application from Lua

The `knurl` executable runs a Lua 5.5.1 file that can declare native services
and inline application handlers. It embeds the same optional `knurl` module
available to a Lua host. The module calls libknurl's public C API.

## Build and run

```sh
make build TARGET=x86_64-linux-musl KNURL_BINARY=ON CONFIG=release
build/x86_64-linux-musl/release/knurl app.lua
```

Use `LUA_CONFIG=ON` to also build the loadable module. Install the C library
with a normal `cmake --install`, then use `--component binary` for the
executable and `--component lua` for the module. The release tarballs ship
the Linux runner as a static musl executable and the Apple Silicon runner
with macOS system libraries and frameworks. See [release artifacts](https://github.com/c89systems/libknurl/blob/trunk/docs/releasing.md).
With an external Lua 5.5.1 host, add `<prefix>/lib/lua/5.5/?.so` to `LUA_CPATH` and
`<prefix>/lib/lua/5.5/?/init.lua` and `<prefix>/lib/lua/5.5/?.lua` to
`LUA_PATH`. The module's platform loader needs the selected libknurl, cpkt,
lonejson and pslog runtime libraries. A static Linux runner loads pure Lua
modules and provides its built-in C modules through `require`.
See [runtime direction](runtime-direction.md).

Use `knurl.auth` to configure native browser or machine authentication for
both configuration-only and application routes. [Native auth and administration](auth.md)

## Logging

The runner and optional Lua install provide `require('pslog')` using the pinned
libpslog Lua source. Use `logger` to configure Kore's diagnostic and structured
HTTP access records. You can also set or replace the logger before `run()` with
`server:set_pslog(log, access_log)`. `access_log` accepts a boolean or a table
with `enabled`, `logger`, `success_level`, `client_error_level`,
`server_error_level`, and `status_levels` keyed by HTTP status.

```lua
local knurl = require('knurl')
local pslog = require('pslog')
local log = pslog.new_json{min_level = 'info'}
local server = assert(knurl.server{logger = log, access_log = {success_level = 'info'}})
log:info('configured the application')
```

Kore records reach the registered native sink in the process where they
originate. Ordinary `log:info()` calls use libpslog directly wherever `log`
is in scope, including in application handlers. The server retains registered
loggers until replacement or close. Choose a native sink suitable for writes
from multiple processes and keep its output available through the server run.

## Configure and serve

```lua
local knurl = require('knurl')

local function check(value, err)
  if value == nil then
    error(err.message .. (err.detail ~= '' and (' (' .. err.detail .. ')') or ''), 2)
  end
  return value
end

local server <close> = check(knurl.server{bind = '127.0.0.1', port = 8080})
check(server:text{path = '/health', body = 'ready\n'})
check(server:json{path = '/status', body = '{"ok":true}\n'})
check(server:redirect{path = '/', location = '/health'})
check(server:static_directory{path = '/static', root_dir = './public'})

local visits = 0
local log
check(server:on_worker_start(function()
  log = assert(io.open('visits.log', 'a'))
end))
check(server:route{
  path = '/app/:user/:section',
  handler = function(req)
    visits = visits + 1
    assert(log:write(req.params.user, '\n'))
    return {json = {user = req.params.user, section = req.params.section}}
  end,
})
check(server:on_worker_stop(function()
  assert(log:close())
end))

check(server:validate())
check(server:run())
assert(visits == 0) -- The parent continues with its original state.
```

The top-level file runs once. `run()` keeps the parent waiting in C until
the server stops and handles signals during that period. Each forked Kore
worker inherits the closures and their captured state and runs them on one
dedicated Lua execution thread. Lua mutations stay in that worker. Initialize
process- or thread-affine resources in `on_worker_start` before readiness.
`on_worker_stop` runs on the same thread after its work ends. A replacement
worker initializes from the parent's snapshot. The server reports errors from
worker initialization or cleanup through its lifecycle.

`knurl.server` defaults to one HTTP worker and one listener on
`127.0.0.1:8080`. Set `workers`, `max_connections` or `backend_quiet` as
needed. `application_max_tasks` limits admitted Lua work per worker, including
queued work, and defaults to 16. A full pool returns 503 for a new inline HTTP
handler. Detached Lua callbacks can wait in a separate queue of the same
size. A shared timer retries its head, and HTTP hooks rejected by this queue
return 503. `application_timeout_ms` defaults to 30000 including queue time.
Knurl returns 504 when an HTTP handler or route hook times out. Cancellation is
cooperative and a running callback retains its slot until it returns. Kore
continues serving native services while the application thread runs.

Set `autoblock_disabled = true` to turn off the production profile's
automatic abuse blocking.

`validate()` checks declarations and `preflight()` also probes startup
resources. Run from a host that can safely fork. `close()` releases the
inactive server and its routes. An API error returns `nil, error` with a
numeric status, message and optional detail.

## Listeners and TLS

For a single HTTPS listener use `tls = {host = 'example.com', certificate =
'cert.pem', private_key = 'key.pem'}`. Use `listeners` for multiple sockets.
Each entry accepts `name`, `bind`, `port`, and a `kind` of `http`, `https` or
`redirect`. On HTTPS and redirect listeners `identity` selects a one-based
index in `tls.identities` and defaults to 1. A redirect listener also needs
`redirect_https_port`. Set `presented_port` when clients use a different port
from the bind port.

```lua
local server = check(knurl.server{
  listeners = {
    {kind = 'redirect', port = 80, redirect_https_port = 443},
    {kind = 'https', port = 443},
  },
  tls = {
    identities = {{domains = {'example.com'}, mode = 'acme'}},
    acme = {
      email = 'ops@example.com',
      directory_url = 'https://acme-v02.api.letsencrypt.org/directory',
      runtime_directory = '/var/lib/knurl/acme',
    },
  },
})
```

ACME uses a private directory for account and certificate state. An ACME
directory can also specify `ca_file`, `ca_directory`, and `persist_timeout_ms`.
Manual identities use `bundle` or `certificate` and `private_key` PEM paths,
with optional `extra_chain`, `client_ca` and `require_client_certificate`.
`tls.version` accepts `default`, `both`, `1.2` or `1.3`. Configure TLS and
listeners at startup. [TLS and ACME](tls.md)

## Request and reply

The handler's request contains `method`, `path`, `raw_path`, `query`,
named `params`, and ordered `headers` entries with `name` and `value`.
Read live input with `req:read(bytes)`. Each call requests up to 64 KiB
and defaults to 16 KiB. It returns a chunk, nil at EOF, or `nil, error`.
Read until EOF to consume the whole body. A handler can return a response
early. Kore closes the connection after sending it if HTTP input is unfinished.
`req:cancelled()` reports cancellation. The reader expires when the handler
returns.
`max_body_bytes` may narrow the server's body limit, and `buffer_bytes`
sets the route's bounded input queue capacity.

Return a string for a 200 text response, nil for 204, or a table with optional
`status`, `content_type` and string-valued `headers`. Choose `body` for raw
bytes or `json` for a Lua string, number, boolean, object, or dense array.
JSON objects use string keys and an empty table makes an empty object. JSON
responses are buffered with an 8 MiB maximum and a nesting limit of 32.
An uncaught handler error returns 500 and logs a bounded message and Lua
traceback on the server.

### Streaming and server-sent events

Return `{stream = function(max_bytes) ... end}` for a live response. Return up
to `max_bytes` of nonempty data per call and nil at EOF. The producer runs on
the worker's Lua thread. Kore requests one bounded chunk as output capacity
becomes available. A HEAD response leaves the producer uncalled.

```lua
check(server:route{
  path = '/updates',
  handler = function()
    local sent = false
    return {sse = function()
      if sent then return end
      sent = true
      return {id = '1', event = 'update', data = 'ready'}
    end}
  end,
})
```

`sse` defaults to `text/event-stream`. A producer can return a data string or
a table with `id`, `event`, `data`, `comment` and `retry_ms`. Each encoded
event must fit the `max_bytes` argument. Returning nil ends the stream.

### Typed REST and OpenAPI

Set `request_json` to a LoneJSON schema or `request_xml` to a table with
`schema` and optional `root_element`, `text_key`, `attribute_prefix`,
`trim_text`, `skip_unknown_disabled`, `max_depth` and `max_text_bytes`.
Knurl decodes the request from the live body reader before invoking the
handler. The handler receives the mapped record as `req.input`. Malformed
or invalid input gets a 400 JSON error. Return a record as `{json = record}`
or `{xml = {record = record, root_element = 'message'}}`. Mapped replies
are materialized with an 8 MiB limit.

```lua
local json = require('lonejson').new()
local message = json.schema('Message', {json.field('text', json.string{required = true})})
check(server:route{
  method = 'POST', path = '/messages/:id', request_json = message,
  handler = function(req) return {json = req.input} end,
})
check(server:openapi_doc{
  method = 'POST', path = '/messages/:id', request_schema = message,
  responses = {[200] = {description = 'Echoed message', schema = message}},
})
local document = check(server:openapi{title = 'Messages', version = '1.0', format = 'json'})
```

`openapi_doc` also accepts `summary`, `operation_id` and `tags`.
`openapi` accepts `format = 'yaml'` and an optional `max_bytes`.

### WebSockets

```lua
check(server:websocket{
  path = '/chat/:room',
  on_open = function(ws, req)
    check(ws:send_text('Joined ' .. req.params.room))
  end,
  on_message = function(ws, bytes, kind)
    if kind == 'text' then
      check(ws:send_text(bytes))
    else
      check(ws:send_binary(bytes))
    end
  end,
})
```

ACL and native auth apply before the handshake. `on_message` receives a
complete text or binary message, including reassembled fragments.
Callbacks for a connection run in order on the same Lua thread as HTTP
handlers. Each idle connection leaves that thread available. Use
`ws:request()` to read the handshake metadata. Send messages and call
`ws:close(code, reason)` from a WebSocket callback. `on_close(ws, outcome)`
receives the peer and local close codes, peer reason, `opened`, `clean` and
terminal `cause`. Each accepted connection with `on_close` reserves space
for that callback until it retires. The callback waits for the Lua thread
without an application deadline while the worker is running. A worker holds
at most `max_connections` of these reservations and returns 503 on a new
handshake that needs one when all are in use.
Exceptions from WebSocket callbacks go to the server's diagnostic logger.
`protocol`, `max_frame_bytes`, `max_message_bytes`, `output_queue_bytes`,
and `close_timeout_ms` configure the connection. Use `on_writable(ws)` to
retry a send when the bounded output queue has room.

## Outbound requests with curl

The runner and optional Lua install provide `require('curl')`. The same call
works at the top level, in worker hooks, and in an application handler.
Use `require('lonejson')` for the bundled upstream Lua module, including
the schema constructors for streaming JSON.

```lua
local curl = require('curl')

check(server:route{
  path = '/upstream',
  handler = function()
    local reply = curl.json{
      url = 'https://example.com/status.json',
      connect_timeout_ms = 500,
      timeout_ms = 2000,
    }
    if not reply.ok or reply.status ~= 200 then
      return {status = 502, body = 'upstream request failed'}
    end
    return {json = reply.json}
  end,
})
```

`curl.perform` returns `ok`, `code`, `code_name`, HTTP `status`, `headers`,
`body`, `effective_url`, and `streamed`. Transfer failures include `error`.
HTTP errors such as 404 can be successful transfers. `curl.json` encodes
`body_json` and decodes a buffered response into `result.json`.

Transfers are synchronous and occupy the calling Lua thread. Kore continues
serving native routes, and other Lua requests on the same worker wait for the
thread. Set an outbound deadline for each request. `timeout_ms` defaults to
30000 and `connect_timeout_ms` to 10000. The route deadline can return a 504
while the curl call is still running. Transfers default to HTTP and HTTPS,
with TLS certificate verification enabled and redirects disabled.

The default response buffer holds up to 8 MiB. Supply `write(chunk)` or
`download_path` to consume the response incrementally. For uploads, use
`upload_path` or `read(max_bytes)`, which supplies a string per call and
returns nil at EOF. `curl.stream_json` uses a LoneJSON schema on either side
with `request = {schema = schema, value = value}` and
`response = {schema = schema}`. A streamed response leaves `body` empty.
Return `true` from a write callback to accept the chunk. An optional
`progress(info)` callback returns `true` to continue. Callbacks run on the
calling Lua thread, and nested transfers raise an error.
`download_path` must resolve to a different file from `upload_path` and from
every multipart file source. Multipart file sources must be readable regular
files.

## Send mail over SMTP

`require('knurl.smtp').send` uses the same synchronous curl binding. Supply
the SMTP envelope and a complete email message, including its headers.

```lua
local smtp = require('knurl.smtp')
local result = smtp.send{
  url = 'smtps://mail.example.com:465',
  username = os.getenv('SMTP_USER'),
  password = os.getenv('SMTP_PASSWORD'),
  from = 'notifications@example.com',
  to = {'ops@example.com'},
  body_path = 'message.eml',
  timeout_ms = 5000,
}
assert(result.ok, result.error_message)
```

`to` accepts one address or an array. Use `body` or `message` to supply a
string with CRLF line endings, `body_path` or `upload_path` for a file, or
`read(max_bytes)` to supply chunks. The default protocol allowlist is SMTP
and SMTPS. With an `smtp://` endpoint, set `smtp = {use_ssl = true}` to require
STARTTLS. Credentials, certificate options and deadlines are passed to curl.
Invalid arguments raise a Lua error. Transfer failures return `ok = false`
with a structured `error` and the SMTP reply code in `status` when available.

## Route declarations

| Registration | Fields and behavior |
| --- | --- |
| `text`, `json` | A fixed `body`, HTTP `status` (default 200), optional `content_type` and `cache_control`. Fixed JSON bodies contain already encoded JSON. Bodies may contain binary bytes. |
| `redirect` | Required `location`, optional 3xx `status` (default 302), `body`, `content_type` and `cache_control`. |
| `static_file`, `static_directory` | A `file_path` or `root_dir`, respectively. The [static service](assets.md) handles ranges, compression, indexes and MIME policy. |
| `upload_file` | A `file_path` destination and optional `max_body_bytes`. Core [file upload](uploads.md) performs native I/O. |
| `webdav` | A filesystem `root_dir` or managed storage using `cache_dir` and `site_id`. Set `auth` for a verified principal or `auth_disabled = true` for a public mount. Optional quotas include `max_file_bytes`, `max_total_bytes` and `max_resources`. |
| `route` | An inline `handler` function and optional body and buffer limits. Defaults to GET. |
| `proxy`, `application` | An HTTP(S) `target`, optional `alternate_targets` and [proxy limits and upstream TLS fields](proxy-http.md). `application` uses the same native proxy with a 30 s default total deadline. The default forwards the original path and query. |

Each declaration takes a `path`, optional ACL and optional `method` or array
`methods` of HTTP method names. Fixed responses default to GET, static routes
to GET and HEAD, uploads to POST and proxies to all supported methods.
Fixed responses, uploads, inline handlers and proxies accept `path_kind`
with `literal`, `params` or `regex`. Omit it for path inference. Register
in the same order you would use in the C router.

`server:group{prefix = ..., acl = ...}` applies a shared prefix and ACL to
route declarations, including proxy, WebDAV and WebSocket routes. Supply an
ACL table or an object returned by `knurl.acl{...}`. Successful registrations
return a route handle.
Use `route:set_acl(value)` or `group:set_acl(value)` before serving to replace
an attachment. Pass nil to clear it. Core retains each attachment.

Native routes declared without Lua callbacks stay in C at request time.
Omitting `hooks` and supplying `hooks = {}` have the same effect. Use `acl`
for network policy and `auth` for native identity policy.

### Request hooks

Register callbacks in `hooks` on a route or group. Native ACL and auth run
first. Then the group and route `auth(req)` hooks run, followed by group and
route `before(req)`. These hooks see request metadata before body admission,
including `req.auth` when native auth verified the request. An auth hook
returns true to continue, false or nil for 403, or a reply table. A before
hook returns nil to continue or a reply table to stop dispatch. A hook reply
table accepts `status`, `headers`, `content_type` and a `body` of up to 64 KiB.

The route and group `after(req, res)` hooks run in that order before final
response bytes are sent. `res` contains `status` and ordered `headers`.
Return nil to leave it as it is, or return `status` and/or a header edit
table. A string sets a header and false removes one. Header edits preserve
transport framing and auth cache policy.

### Proxy callbacks

- `preflight(req)` returns a bounded reply or nil to continue.
- `rewrite(req)` returns optional `path`, `query`, `host`, `method`, a
  one-based `target` index, and a `headers` edit table. Index 1 is the main
  target and subsequent indexes select from `alternate_targets`. Set
  `query = false` to strip the query or `host = false` to use the selected
  target's authority.
- `modify_response(req, res)` edits the status and headers of an upstream
  response before forwarding it. Its return table matches `hooks.after`.
- `on_error(req, err, status)` can return a bounded replacement reply
  for a gateway failure before commitment, including failure to obtain an
  OAuth2 token. Nil keeps the native error reply.
- `on_exchange(req, event)` receives terminal status, byte counts, duration,
  upstream and disconnect information for an admitted exchange. Delivery is
  best effort through the bounded Lua worker queue.

These callbacks run on the worker's Lua thread. The proxy transports HTTP,
SSE and WebSocket traffic in C with bounded buffers.
Knurl logs exceptions in `on_error` and `on_exchange` through the server's
configured logger. An exception in `on_error` keeps the native gateway reply.

## ACLs and country data

The [ACL policy](acl.md) supports the `allow` and `deny` sets, each with
`addresses`, `countries` and `unknown`. Set `policy` to choose the fallback.
Use `use_peer` to match the TCP peer, `country_database` for a file-backed
MMDB and `max_heap_bytes` for the reader budget. `dns_refresh_ms`,
`dns_max_addresses` and `dns_timeout_ms` control native hostname resolution.
To check an ACL outside the server, call `acl:refresh()` before
`acl:check(peer, effective)`.

Behind a reverse proxy you control, configure `client_ip` on `knurl.server`.
It accepts `trusted_proxies`, `forwarded_header`, `real_ip_header`,
`max_hops`, and `forwarded_format` (`comma` or `rfc7239`). The
[forwarding trust rules](acl.md#forwarding-trust) apply before country lookup.

```lua
local knurl = require('knurl')
local database, err = knurl.country_database()
assert(database, err and (err.message .. ' ' .. err.detail))

local server = assert(knurl.server{port = 8080})
assert(server:text{
  path = '/regional', body = 'Hello',
  acl = {policy = 'deny', allow = {countries = {'SE'}}, country_database = database},
})
assert(server:run())
```

Calling `knurl.country_database()` before `run()` opts in to the download.
It uses `$XDG_CACHE_HOME/knurl/dbip-country-lite.mmdb`, falling back to
`$HOME/.cache/knurl`. It returns a valid cached file immediately. On a miss,
it fetches the current UTC month's DB-IP Country Lite release and tries the
previous month if the publisher returns 404. The helper decompresses and
validates the file before publishing it atomically. It keeps the cached
database until the operator removes it or chooses a different cache path.
Optional `cache_dir`, `url` and `timeout_ms` fields select an absolute cache
directory, a mirror serving gzip-compressed MMDB, and the per-attempt timeout
(30000 ms by default). HTTPS uses the host CA bundle or the configured
`CURL_CA_BUNDLE`, `SSL_CERT_FILE` or `SSL_CERT_DIR`.

[DB-IP Country Lite](https://db-ip.com/db/download/ip-to-country-lite) is
licensed under CC BY 4.0. Add an [IP Geolocation by DB-IP](https://db-ip.com)
link to application pages that use or display the data.

## C host extension points

The C host API provides custom ACL resolvers, auth backends, and borrowed
country, asset and storage providers.
