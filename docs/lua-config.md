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

`knurl.server` uses the C production webserver profile with one listener
(default `127.0.0.1:8080`) and one HTTP worker. Supply `tls = {host, certificate,
private_key}` with named fields to serve HTTPS using PEM files. Set `workers`,
`max_connections` or `backend_quiet` as needed. `application_max_tasks`
limits admitted Lua requests per worker, including queued work. Its default
is 16 and a full pool returns 503. `application_timeout_ms` defaults to
30000 including queue time. Expiry returns 504. Cancellation is cooperative,
so a running handler retains its slot until it returns. Kore continues
serving native services while the application thread runs.

Set `autoblock_disabled = true` to turn off the production profile's
automatic abuse blocking.

`validate()` checks declarations and `preflight()` also probes startup
resources. Run from a host that can safely fork. `close()` releases the
inactive server and its routes. An API error returns `nil, error` with a
numeric status, message and optional detail.

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
| `route` | An inline `handler` function and optional body and buffer limits. Defaults to GET. |
| `proxy`, `application` | A fixed HTTP(S) `target` and optional [proxy limits and upstream TLS fields](proxy-http.md). `application` uses the same native proxy with a 30 s default total deadline. Both preserve the original path and query. |

Each declaration takes a `path`, optional ACL and optional `method` or array
`methods` of HTTP method names. Fixed responses default to GET, static routes
to GET and HEAD, uploads to POST and proxies to all supported methods.
Fixed responses, uploads, inline handlers and proxies accept `path_kind`
with `literal`, `params` or `regex`. Omit it for path inference. Register
in the same order you would use in the C router.

`server:group{prefix = ..., acl = ...}` scopes fixed responses, static files,
file uploads and inline handlers to a shared prefix and ACL. Declare upstream
proxies directly on the server. Supply an ACL table or an object returned by
`knurl.acl{...}`. Successful route registrations return a route handle.
Use `route:set_acl(value)` or `group:set_acl(value)` before serving to replace
an attachment. Pass nil to clear it. Core retains each attachment.

Native routes serve entirely in C. Empty `hooks = {}` and server
`callbacks = {}` tables are accepted. The inline handler and worker hook
methods are the Lua callback entry points. Use `acl` for network policy and
`auth` for identity policy.

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
country and asset providers. It also exposes request hooks, proxy target
selection hooks, ACME, multiple listeners, WebDAV, WebSocket handlers, and
mapped REST, JSON and XML routes. Lua covers the route declarations above,
native auth, file-backed ACLs, worker hooks, logging, and inline request handlers.
