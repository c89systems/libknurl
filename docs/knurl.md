# Knurl runner and Lua reference

Knurl runs a Lua 5.5.1 application in a standalone HTTP server. Native route
declarations configure C services. Application callbacks run on a dedicated
Lua thread in each Kore worker. This manual accompanies the runner and is
available on standard output through `knurl -a docs`.

## Command line

| Command | Behavior |
| --- | --- |
| `knurl FILE [ARG ...]` | Execute a Lua text file of any name. Paths are relative to the working directory unless absolute. |
| `knurl -a run FILE [ARG ...]` | Execute the file explicitly. `--action run` and `--action=run` are equivalent. |
| `knurl -a auth COMMAND ...` | Initialize or administer native authentication state. See the authentication section below. |
| `knurl -a docs` | Print this Markdown file to standard output. |
| `knurl -a source` | Print the Lua facade files, each preceded by `-- ===== path =====`. Save sections as individual files. The dump contains Knurl-owned Lua, the bundled pslog and LoneJSON facades, and no C implementation. |
| `knurl -h` or `knurl --help` | Print command usage. Use `knurl -a auth -h` for authentication usage. |
| `knurl --version` | Print the runner version. |

Use `knurl -- -filename` or `knurl -a run -- -filename` for a filename
beginning with a dash. A file named `auth` is a Lua script in the default
action. The remainder of the command line is passed to the script in `...`
and `arg[1]` onward. `arg[0]` is the filename and `arg[-1]` is the executable
as invoked. Exit status `0` means success, `1` means runtime or I/O failure,
and `2` means invalid CLI usage.

Save this as `hello` and run `knurl hello 8080`.

```lua
local knurl = require('knurl')
local server <close> = assert(knurl.server{port = tonumber(arg[1]) or 8080})
assert(server:text{path = '/', body = 'Hello from Knurl\n'})
assert(server:run())
```

The runner preloads `knurl`, `curl`, `curl.core`, `knurl.smtp`, `pslog`,
`pslog.core`, `lonejson`, and `lonejson.core`. `knurl.curl` and
`require('curl.core')` expose the same transfer core. `knurl.smtp` is also
available as `require('knurl').smtp`. Pure Lua modules can be loaded through Lua's normal
`package.path`. The static Linux runner cannot load arbitrary native plugins.

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

The `knurl` module exports `server`, `acl`, `auth`, `oauth2`,
`country_database`, `curl`, and `smtp`. Declarations normally return the
new handle or `nil, error`. Error tables provide `status`, `kind`, `source`,
`native_code`, `operation`, `message`, and `detail`. The curl facade raises
Lua errors for invalid options and returns a result table for transfers.

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

| `knurl.server{...}` option | Meaning |
| --- | --- |
| `bind`, `port` | Single-listener address and port. Defaults to `127.0.0.1:8080`. TLS makes this listener HTTPS. |
| `listeners`, `tls` | Explicit listeners and TLS settings below. `listeners` replaces `bind` and `port`. |
| `workers`, `max_connections` | Worker count and server connection ceiling. |
| `application_max_tasks`, `application_timeout_ms` | Lua worker admission bound and application deadline. The task bound may not exceed 256. |
| `websocket_max_frame_bytes` | Server ceiling for individual WebSocket frames. |
| `backend_quiet`, `autoblock_disabled` | Suppress backend startup diagnostics or disable Core automatic blocking, respectively. |
| `logger`, `access_log` | Borrow a pslog logger and select access logging as described under Logging. |
| `client_ip` | Trusted peer forwarding configuration under ACLs and country data. |
| `callbacks` | Reserved. Register worker callbacks with `server:on_worker_start(fn)` and `server:on_worker_stop(fn)`. Passing nil clears the respective callback. |

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
listeners at startup. [TLS and ACME](https://github.com/c89systems/libknurl/blob/trunk/docs/tls.md)

`tls.cipher_list` configures the cipher list. `tls.max_material_bytes` sets
the input bound for TLS material. Use `host` for a single identity or
`identities = {{domains = {...}, ...}}` for several SNI identities. Single
identity and list forms both accept `mode = 'manual'` or `'acme'` and the PEM
and client certificate fields above. Listener ports are integers from 0 to
65535, where 0 requests an ephemeral bind port. `listeners` accepts up to 255
entries.

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

Use the same schema to turn an XML upload into a typed record and send XML back.
This route accepts a body such as `<message><text>Hello</text></message>`.

```lua
check(server:route{
  method = 'POST', path = '/messages/xml',
  request_xml = {schema = message, root_element = 'message', text_key = ''},
  handler = function(req)
    local reply = message:assign(req.input, {text = req.input:get('text') .. '!'})
    return {xml = {record = reply, root_element = 'message', text_key = ''}}
  end,
})
```

`openapi_doc` also accepts `summary`, `operation_id` and `tags`.
`openapi` accepts `format = 'yaml'` and an optional `max_bytes`.
Call `server:openapi(options)` while inactive to obtain a JSON or YAML string.
Use `server:openapi_doc{path, method, methods, summary, operation_id, tags,
request_schema, responses}` to annotate matching operations. Supply `method`
or `methods` as for routes and map response status codes to tables with
`description` and optional LoneJSON `schema`. An XML response table can
specify the same XML mapping options as a request plus the required `record`.

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

### Curl API and option reference

`curl.version()` returns the linked libcurl version string. `curl.core` and
`knurl.curl` expose the same `version` and `perform` methods.
`curl.json(options)` accepts `body_json` or its alias `json`, supplies
default JSON `Content-Type` and `Accept` headers, and exposes decoded data
as `json` and `response_json`. A response decoding failure sets `ok = false`
and `json_error`. `curl.stream_json(options)` accepts the `request` and
`response` tables described above. The equivalent core options are
`request_schema` with `body_json`, and `response_schema`. A successful
incremental decode populates `json` and `response_json`. A schema validation
failure sets `json_error` and `error`.

| `curl.perform{...}` option | Meaning |
| --- | --- |
| `url`, `method`, `protocols` | Required nonempty URL, optional HTTP method token, and comma-separated protocol allowlist. The default allowlist is `http,https` and also applies to redirects. |
| `username`, `password` | Origin credentials where supported by the selected protocol. |
| `proxy`, `proxy_type`, `proxy_username`, `proxy_password` | Proxy URL, numeric libcurl proxy type (defaults to HTTP), and credentials. Set `proxy = ''` to disable environment proxy selection. |
| `interface`, `user_agent`, `accept_encoding` | Local interface selection, HTTP User-Agent, and accepted encodings. An empty `accept_encoding` asks libcurl for all compiled encodings. |
| `ca_file`, `ca_path`, `verify_peer`, `verify_host` | Override the CA file or directory. Certificate and hostname checks default to true. |
| `client_cert`, `client_key`, `client_cert_type`, `key_password` | Client TLS credentials, certificate type and key password. |
| `ssh_private_key`, `ssh_public_key`, `ssh_known_hosts` | SSH credential and host-key files for enabled SSH-based protocols. |
| `timeout_ms`, `connect_timeout_ms`, `low_speed_limit`, `low_speed_time` | Total and connection deadlines in milliseconds, plus a minimum transfer rate in bytes per second and a duration in seconds. Low-speed aborts are disabled by default. |
| `follow_redirects`, `tcp_keepalive`, `no_signal`, `http2` | Redirects default to false, TCP keepalive defaults to true, `no_signal` must be true, and `http2 = true` requests HTTP/2 negotiation over TLS. |
| `headers` | String-keyed field map and/or integer-keyed `Name: value` strings, up to 128 entries and 64 KiB of text. Field names must be valid HTTP tokens. |
| `upload`, `body` | `body` is a binary-safe string up to 8 MiB. It ordinarily uses a buffered POST. Set `upload = true` for the upload reader and supply `method` to override libcurl's default. |
| `upload_path`, `body_path`, `read` | File path (either spelling) or `read(max_bytes)` producer. The file must be readable and regular. A read callback returns a string of at most `max_bytes`, nil at EOF, or nil and an error. |
| `multipart` | Table of string fields or part tables. In a part, `name` supplies or overrides the field name. Set one of `value`/`body` or `path`/`file_path`. The `filename` and `content_type` fields are optional. Files stream from open descriptors. The maximum is 128 parts. |
| `write`, `download_path` | Incremental response callback or regular-file destination. The file is opened for replacement before the transfer. A failed transfer can leave it partial. `write(chunk)` returns true to continue or nil and an error to stop. |
| `progress` | `progress(info)` receives integer `download_total`, `downloaded`, `upload_total`, and `uploaded`. Return true to continue. |
| `request_schema`, `body_json`, `response_schema` | Incremental LoneJSON request encoding and response decoding. A request schema takes a Lua table as `body_json`. |
| `smtp` | Raw SMTP envelope table. Supply `mail_from`, an array `rcpt` of 1–128 addresses, and optional `use_ssl`. Also select SMTP protocols and a message body. |

Choose one request source and one response sink. File, buffered, multipart,
and schema-backed uploads can replay across a redirect that preserves the
method. An application `read` callback is forward-only. `headers` in the
result is the raw received header text, including intermediate responses.
`code` and `code_name` describe the libcurl result. `status` is the protocol
reply code when available. Inspect it separately from `ok`. `streamed`
indicates use of an incremental response sink, so `body` is empty.

```lua
local curl = require('curl')
local result = curl.perform{
  url = 'https://example.com/upload',
  multipart = {title = 'Report', {name = 'report', path = 'report.csv'}},
  download_path = 'receipt.json', timeout_ms = 5000,
}
assert(result.ok, result.error)
assert(result.status >= 200 and result.status < 300, result.status)
```

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

`mail_from` and `from` are interchangeable. The recipient aliases are `rcpt`,
`recipients`, and `to`. The corresponding values inside `smtp` take
precedence. The envelope field `smtp.use_ssl` controls STARTTLS. All other
curl fields pass through. The result adds `transport_ok` and, on transfer
failure, `error_message` and an error table with `kind`, `source`, `message`,
`code`, `code_name`, and `dependency_code`.

## Route declarations

| Registration | Fields and behavior |
| --- | --- |
| `text`, `json` | A fixed `body`, HTTP `status` (default 200), optional `content_type` and `cache_control`. Fixed JSON bodies contain already encoded JSON. Bodies may contain binary bytes. |
| `redirect` | Required `location`, optional 3xx `status` (default 302), `body`, `content_type` and `cache_control`. |
| `static_file`, `static_directory` | A `file_path` or `root_dir`, respectively. The [static service](https://github.com/c89systems/libknurl/blob/trunk/docs/assets.md) handles ranges, compression, indexes and MIME policy. |
| `upload_file` | A `file_path` destination and optional `max_body_bytes`. Core [file upload](https://github.com/c89systems/libknurl/blob/trunk/docs/uploads.md) performs native I/O. |
| `webdav` | A filesystem `root_dir` or managed storage using `cache_dir` and `site_id`. Set `auth` for a verified principal or `auth_disabled = true` for a public mount. Optional quotas include `max_file_bytes`, `max_total_bytes` and `max_resources`. |
| `route` | An inline `handler` function and optional body and buffer limits. Defaults to GET. |
| `proxy`, `application` | An HTTP(S) `target`, optional `alternate_targets` and [proxy limits and upstream TLS fields](https://github.com/c89systems/libknurl/blob/trunk/docs/proxy-http.md). `application` uses the same native proxy with a 30 s default total deadline. The default forwards the original path and query. |

The following options complete the declarations in the table.

- Static declarations accept `index_file`, `index_enabled`, `not_found_body`,
  `not_found_content_type`, `content_type`, `cache_control`, `mime_types`,
  `compression_disabled` and `precompressed_disabled`. `mime_types` maps file
  extensions to media types.
- `webdav` also accepts `max_body_bytes`, `content_type`, `cache_control`,
  `conceal_disabled`, `operation_timeout_ms` and `operation_timeout_disabled`.
  Filesystem and managed storage are alternate backends.
- `proxy` and `application` accept `oauth2`, `connect_timeout_ms`,
  `idle_timeout_ms`, `total_timeout_ms`, `buffer_limit_bytes`, protocol
  selection `upstream_http_version = 'auto'` or `'http1'`, and PEM strings
  `tls_ca_pem`, `tls_client_cert_pem`, and `tls_client_key_pem`.
- `route` accepts `handler`, `max_body_bytes`, `buffer_bytes`, `request_json`,
  `request_xml` and `hooks`. `websocket` accepts `on_open`, `on_message`,
  `on_writable`, `on_close`, `protocol`, `max_frame_bytes`,
  `max_message_bytes`, `output_queue_bytes`, and `close_timeout_ms`.
- `server:login`, `server:email_code`, `server:logout`, `server:totp_enroll`,
  and `server:auth_workflow` take `path`, a required `auth` policy, and optional
  `acl`. These are native POST endpoints on the server.

Each declaration takes a `path`, optional ACL and optional `method` or array
`methods` of HTTP method names. Fixed responses default to GET, static routes
to GET and HEAD, uploads to POST and proxies to all supported methods.
Fixed responses, uploads, inline handlers and proxies accept `path_kind`
with `literal`, `params` or `regex`. Omit it for path inference. Register
in the same order you would use in the C router.

Method names are case-insensitive. The available names are `ANY`, `GET`,
`POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS`, `PROPFIND`, `MKCOL`,
`COPY` and `MOVE`. Numeric method values are accepted in that order from 0.
A declaration that accepts `method`/`methods` takes one or the other.
WebSocket, WebDAV and native auth endpoints select their own methods.

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

An exchange event provides `request_id`, `route`, `upstream`, `kind`,
`status`, `cause`, `upstream_bytes`, `downstream_bytes`, `duration_ms`,
`disconnect_side`, and `error_category`.

### Serve a reverse proxy, static files, uploads and WebDAV

Save this as `gateway`. Create `public` and `dav-data`, then run `knurl gateway`.
Set `UPSTREAM_URL` if you have a separate HTTP or HTTPS upstream service.

```lua
local knurl = require('knurl')
local server <close> = assert(knurl.server{port = 8080})
local local_only <close> = assert(knurl.acl{
  policy = 'deny', allow = {addresses = {'127.0.0.1/32', '::1/128'}},
})
assert(server:static_directory{path = '/assets', root_dir = './public'})
assert(server:upload_file{
  path = '/upload', method = 'PUT', file_path = './upload.bin',
  max_body_bytes = 1024 * 1024, acl = local_only,
})
assert(server:webdav{
  path = '/dav', root_dir = './dav-data', auth_disabled = true, acl = local_only,
})
local target = os.getenv('UPSTREAM_URL')
if target then
  assert(server:proxy{
    path = '^/backend/.*$', target = target, connect_timeout_ms = 1000,
    rewrite = function(req) return {path = req.raw_path:sub(9)} end,
  })
end
assert(server:run())
```

## ACLs and country data

The [ACL policy](https://github.com/c89systems/libknurl/blob/trunk/docs/acl.md) supports the `allow` and `deny` sets, each with
`addresses`, `countries` and `unknown`. Set `policy` to choose the fallback.
Use `use_peer` to match the TCP peer, `country_database` for a file-backed
MMDB and `max_heap_bytes` for the reader budget. `dns_refresh_ms`,
`dns_max_addresses` and `dns_timeout_ms` control native hostname resolution.
To check an ACL outside the server, call `acl:refresh()` before
`acl:check(peer, effective)`.

`knurl.acl(options)` returns an ACL handle. `acl:check(peer, effective)`
takes a peer IP and optional effective IP and returns true or false for the
policy decision. `acl:refresh()` returns true when resolution is complete
and false when work is still pending. `acl:close()` releases the handle.
Attached routes retain their own reference. Address lists accept IPs, CIDRs
and hostnames. Country lists contain two-letter codes and require an MMDB
database. Set `unknown` to match addresses without a known country code.

Behind a reverse proxy you control, configure `client_ip` on `knurl.server`.
It accepts `trusted_proxies`, `forwarded_header`, `real_ip_header`,
`max_hops`, and `forwarded_format` (`comma` or `rfc7239`). The
[forwarding trust rules](https://github.com/c89systems/libknurl/blob/trunk/docs/acl.md#forwarding-trust) apply before country lookup.

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

## Native route authentication

Knurl can protect a native route or a Lua application route with the same auth
policy. The route ACL runs first. Auth runs on a bounded native pool while the
Kore event loop continues serving other requests. Knurl waits for auth before
it opens route resources, grants `100 Continue`, or admits the body. Protected
routes and the login endpoints require HTTPS.

### Authentication CLI

| `knurl -a auth` command | Effect |
| --- | --- |
| `init DIRECTORY` | Initialize the private JSON store and encryption key. |
| `prune DIRECTORY` | Remove expired flow state and locally invalidated machine credentials. |
| `user-password DIRECTORY USER` | Read one password from standard input and set the account password. Password changes invalidate previously issued account state. |
| `user-email DIRECTORY USER EMAIL` | Set the address for email-code login. |
| `user-totp DIRECTORY USER [--rotate] [--users-id ID]` | Print an enrollment URI once. Use `--rotate` to replace the seed and `--users-id` for directory-backed identities. |
| `user-enable DIRECTORY USER [--users-id ID]` | Enable the named account. |
| `user-disable DIRECTORY USER [--users-id ID]` | Disable the account and invalidate existing account sessions and credentials. |
| `client-issue DIRECTORY POLICY CLIENT` | Issue or replace a policy-scoped machine secret and print it once. |
| `client-revoke DIRECTORY POLICY CLIENT` | Revoke that client credential. |

### Lua auth policy and upstream clients

`knurl.auth{...}` accepts `name`, `store`, `method`, `login`,
`session_seconds`, `smtp`, `client_id`, `client_secret`, `oidc` and `steps`.
The default `method` is `session` and the default `login` is `password`.
Use `login = 'totp'` or `'email'` for the flows below. A named policy keeps
its credentials separate from other policies in the same store. Auth policy
handles support `close()` and Lua's `<close>` syntax. Route and group
attachments retain the native policy. Use `set_auth(nil)` to clear one.

The auth policy's `smtp` table takes `url`, `from`, `username`, `password`,
`ca_file` and `timeout_ms`. The `oidc` table takes `issuer`, `audience`,
`jwks_uri`, `jwks_json`, `scope`, `token_type`, `ca_file` and `pin_jwks`.
`knurl.oauth2{...}` takes `issuer`, `token_endpoint`, `client_id`,
`client_secret`, `scope`, `audience`, `resource`, `ca_file` and
`client_secret_post`. Call the client object's `token()` for an access token
or `close()` to release it. Proxy attachments retain their own reference.

### Browser sessions

Create a private state directory under the operating-system account that runs
Knurl. The password command reads a password of at least 15 bytes from standard
input and hides interactive input. The TOTP command generates and encrypts the
seed, then prints the enrollment URI once for the user's authenticator. Use
`--rotate` to replace an existing enrollment.

```sh
knurl -a auth init ./auth-state
knurl -a auth user-password ./auth-state alice
knurl -a auth user-totp ./auth-state alice
```

```lua
local knurl = require('knurl')
local server <close> = assert(knurl.server{
  port = 8443,
  tls = {host = 'localhost', certificate = 'cert.pem', private_key = 'key.pem'},
})

local account <close> = assert(knurl.auth{
  name = 'account',
  store = './auth-state',
  login = 'totp',
})

assert(server:login{path = '/login', auth = account})
assert(server:logout{path = '/logout', auth = account})
assert(server:static_directory{path = '/private', root_dir = './private', auth = account})
assert(server:route{
  path = '/account/:page',
  auth = account,
  handler = function(req)
    return {json = {user = req.auth.subject, page = req.params.page}}
  end,
})
assert(server:run())
```

The browser submits `POST /login` as `application/x-www-form-urlencoded`
with `username`, `password` and `code`. Choose `login = 'password'` for a
password-only policy, which takes `username` and `password`. On success, the
endpoint returns HTTP 204 and sets a Secure, HttpOnly, SameSite=Strict cookie.
Forms can stay on the application's HTTPS origin, then navigate to a protected
page after success. A same-origin `POST /logout` revokes the session and clears
its cookie. The default lifetime is eight hours and `session_seconds` overrides
it. Use distinct names for different policies.

To let users enroll themselves, register `server:totp_enroll` with a
bootstrap session policy. That policy can use password or email-code login.
Keep the TOTP-required policy on the protected application routes.

```lua
local bootstrap <close> = assert(knurl.auth{
  name = 'bootstrap', store = './auth-state', login = 'password',
})
assert(server:login{path = '/enroll/login', auth = bootstrap})
assert(server:totp_enroll{path = '/enroll/totp', auth = bootstrap})
```

The TOTP login returns HTTP 428 after password verification if the account
needs enrollment. First sign in under the bootstrap policy, then POST an
empty form to `/enroll/totp` with its cookie. The endpoint returns HTTP 202
with a JSON `uri` for the authenticator. POST `confirm=<new code>` with
the same cookie within ten minutes to activate the seed and receive HTTP
204. If the provisioning response was lost, repeat the first POST to stage
a fresh seed. Use `application/x-www-form-urlencoded` on the same HTTPS
origin.

To rotate, submit `code=<current unused code>` instead of an empty form.
The current seed remains active until you confirm the replacement. Successful
confirmation invalidates existing sessions tied to the account's revision.
The JSON store also applies this to sessions issued after directory-backed
password or email verification.
Sign in on the TOTP-required route with a fresh code after confirmation.
The operator CLI can replace an enrollment directly.

An application receives `req.auth.subject` and `req.auth.method`. The method
is `session` or `machine`. C handlers use `knurl_request_auth()` or
`knurl_request_snapshot_auth()` from a native reader callback. Lua can also
assign a default policy with `server:group{prefix = '/private', auth = account}`.
`route:set_auth(policy)` or `group:set_auth(policy)` replaces an attachment
before the server runs. An explicit route policy takes precedence over its group.

### Email login

Set the delivery address with `knurl -a auth user-email ./auth-state alice alice@example.com`.
Use `login = 'email'` and provide `smtp` in the policy.

```lua
local email <close> = assert(knurl.auth{
  name = 'email',
  store = './auth-state',
  login = 'email',
  smtp = {
    url = 'smtps://mail.example.com:465',
    from = 'login@example.com',
    username = 'login@example.com',
    password = assert(os.getenv('SMTP_PASSWORD')),
  },
})
assert(server:email_code{path = '/email/code', auth = email})
assert(server:login{path = '/email/login', auth = email})
assert(server:logout{path = '/email/logout', auth = email})
```

`POST /email/code` takes a form with `username`. It returns HTTP 202 once
queued. The response is the same whether or not the account has a delivery
address. Submit `username` and `code` to `POST /email/login` to redeem it.
A code expires after ten minutes and succeeds once. SMTP requires verified TLS.
Use `smtp://` for mandatory STARTTLS or `smtps://` for implicit TLS. The
optional `ca_file` overrides the system trust bundle and `timeout_ms` sets a
timeout up to ten seconds. SMTP delivery errors go to the configured Knurl
diagnostic logger. Delivery uses a separate native pool from password and
session verification. A successful send starts the resend cooldown. A failed
send releases its reservation and leaves any previously issued code usable.

### Ordered factors and machine credential issuance

```lua
local clients <close> = assert(knurl.auth{
  name = 'deployment', store = './auth-state', method = 'machine',
  steps = {'password', 'totp'},
})
assert(server:auth_workflow{path = '/provision', auth = clients})
assert(server:text{path = '/integration/health', auth = clients, body = 'ready'})
```

Select an ordered subset of `email_code`, `password` and `totp`. TOTP
requires a preceding factor. Configure SMTP when using `email_code`.
POST an HTTPS form to the workflow route with `username` and, if password
comes first, `password`. An email-first workflow sends a code to the
account's enrolled address and returns the same HTTP 202 shape for unknown
accounts.

Each HTTP 202 includes an opaque `workflow`, the current `step` and its
`required` field. POST back to the same path with that workflow and the
requested `password` or six-digit `code`. The workflow expires after ten
minutes and each successful continuation replaces its identifier.

For a machine policy, the final HTTP 201 returns `client_id` and
`client_secret` once. Use the pair as HTTP Basic credentials on routes
protected by the named policy. The authenticated subject is the account
that completed the workflow. Account changes invalidate its issued
credentials. An operator can revoke an individual credential with
`knurl -a auth client-revoke ./auth-state deployment <client_id>`. For a
browser policy, choose `method = 'session'`. The final HTTP 204 sets the
policy's session cookie and the existing logout route revokes it.

### Machine authentication

Issue a client secret with `knurl -a auth client-issue ./auth-state service deployment`.
This prints the secret once. Set `name = 'service'`, `method = 'machine'`, and
`store = './auth-state'` in the policy and attach it to the machine routes.
Each credential belongs to its named policy, so policies can share the same store.
Clients send HTTP Basic credentials over TLS. Use
`knurl -a auth client-revoke ./auth-state service deployment` to revoke it, or issue it
again to rotate its secret.

For a single client, you can supply `client_id` and a random `client_secret`
directly in the Lua policy in place of `store`. To accept signed OAuth access
tokens from an external issuer, configure `oidc` in a machine policy.

```lua
local service <close> = assert(knurl.auth{
  name = 'service', method = 'machine',
  oidc = {
    issuer = 'https://identity.example.com',
    audience = 'https://api.example.com',
    scope = 'api.machine',
  },
})
assert(server:text{path = '/integration/health', body = 'ready', auth = service})
```

LoneJSON verifies the signature, issuer, audience, expiry, required scope and
subject. The default JOSE types are `at+jwt` and `application/at+jwt`.
Set `token_type` to require one specific type. Knurl discovers the JWKS endpoint over
HTTPS and refreshes cached keys on expiry or when an unfamiliar key ID
appears. An explicit `jwks_uri` skips discovery. You can seed that cache with
`jwks_json`, or set `pin_jwks = true` to use only the supplied keys.

Responses on authenticated routes use `Cache-Control: private, no-store`.
The native reverse proxy removes Knurl session cookies from default upstream
requests on all routes. It also drops the consumed machine `Authorization`
header.

### OAuth2 for a reverse proxy

```lua
local upstream <close> = assert(knurl.oauth2{
  issuer = 'https://identity.example.com',
  client_id = 'gateway',
  client_secret = assert(os.getenv('UPSTREAM_CLIENT_SECRET')),
  scope = 'api.machine',
})
assert(server:proxy{
  path = '/gateway', target = 'https://api.example.com', oauth2 = upstream,
})
```

You can supply `token_endpoint` directly or discover it from `issuer`.
If a discovered endpoint fails during renewal, Knurl fetches the metadata
again and tries the replacement endpoint when it changes.
Knurl obtains and caches a Bearer token on a native worker. It renews before
expiry, retains rotating refresh tokens, and obtains new client-credentials
tokens when there is no refresh token or the issuer rejects it as invalid.
The proxy supplies the resulting `Authorization` header automatically. All
upstream targets must use HTTPS. The token endpoint uses
`client_secret_basic` by default and supports `client_secret_post = true`.
An upstream Bearer `invalid_token` response marks that token generation as
unusable. Knurl returns the upstream response and obtains a fresh token for a
later request. Proxy token acquisition and inbound JWT verification each use
a native executor separate from the one for local sign-in. Work for the same
OIDC issuer waits in the queue while a request for that issuer is running.
Proxy routes that share an OAuth2 client similarly queue behind that client's
acquisition or renewal, leaving other clients eligible to run.
Both the inbound OIDC and outbound OAuth2 configuration accept `ca_file`
for a private trust root. A Lua application worker can also call
`upstream:token()` for a separate outbound request. The call may wait for
the issuer and handles renewal internally.

### State and C backends

The built-in store writes a fixed set of files in one private directory.

- `accounts.json` holds users, salted password hashes, encrypted TOTP seeds
  and policy-scoped machine credentials.
- `sessions.json`, `email.json`, `totp.json` and `attempts.json` hold
  state for each flow and appear as they are used. Email and TOTP sessions
  are written to the same file as the factor they consume, in one atomic
  update. Ordered workflows keep pending steps and the final session or
  machine credential in the same file as the terminal factor. An email-first
  workflow with a password retains the original login name through the
  password step and checks that both proofs resolve to the same subject.
- `key` holds the owner-only encryption key. Back it up with the JSON files.

One interprocess lock protects updates and operations that read multiple files.
Knurl syncs and atomically replaces each changed JSON file. Disabling a user
changes the account revision, which invalidates existing sessions without a
session-file rewrite. TOTP replay checks use the enrollment ID so seed rotation
starts a new counter. Use `knurl -a auth prune ./auth-state` to remove expired
state and machine credentials invalidated by local account changes. Run the
store on a local filesystem that supports advisory locks and atomic rename.

In C, set `backend` in `knurl_auth_config` to supply storage and
verification operations. For an external user backend, set `users` for
password verification and enrolled-email lookup. Set its stable `users_id`
to a name such as `staff-ldap`. Give every policy that uses this backend the
same ID and give each independent backend a distinct ID. The state backend
receives `users_id` in each exchange and uses it with the canonical subject
for account state. The verified identity also exposes `users_id`, including
as `req.auth.users_id` when available in Lua.

The JSON store lets independent backends share a state directory. For a scoped
operator enrollment, use `knurl -a auth user-totp ./auth-state alice --users-id staff-ldap`.
The same option applies to `user-enable` and `user-disable`. The external
backend manages passwords and email addresses.

A provider verifies one named user directly and returns its canonical subject.
The interface never requires account enumeration or retrieving password hashes
from the directory. Configure `send_email` to replace the native SMTP sender
with a C callback.

Backend callbacks can run concurrently in multiple threads and worker
processes. State operations must be atomic across them. Set `session_check`
in the C policy to revalidate the account after a valid session lookup.
`KNURL_OK` admits the request, `KNURL_EOF` denies it with HTTP 401, and a
provider failure returns HTTP 503. The callback runs after the store releases
its lock. Set `machine_check` for the equivalent check after machine credential
lookup. Machine workflows with a separate `users` backend require that
callback so directory disablement also revokes access through previously issued
credentials. The public `<knurl/auth.h>` header defines the callback contract and
operation inputs.
