# Run a Knurl application from Lua

The `knurl` executable runs a Lua 5.5.1 file that can declare native services
and inline application handlers. It embeds the same optional `knurl` module
available to a Lua host. The module calls libknurl's public C API.

## Build and run

```sh
make build KNURL_BINARY=ON BUILD_DIR=build/knurl
build/knurl/knurl app.lua
```

Use `LUA_CONFIG=ON` to also build the loadable module. Install the C library
with a normal `cmake --install`, then use `--component binary` for the
executable and `--component lua` for the module. With an external Lua 5.5.1
host, add `<prefix>/lib/lua/5.5/?.so` to `LUA_CPATH`. The runner uses the target's
standard interpreter and finds libknurl relative to the installed executable.
The platform loader also needs access to the selected cpkt, lonejson and pslog
runtime libraries, typically via `LD_LIBRARY_PATH` on Linux.
See [runtime direction](runtime-direction.md).

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

`knurl.server` uses the C production webserver profile with one HTTP
listener (default `127.0.0.1:8080`) and one HTTP worker. Set `workers`,
`max_connections` or `backend_quiet` as needed. `application_max_tasks`
limits admitted Lua requests per worker, including queued work. Its default
is 16 and a full pool returns 503. `application_timeout_ms` defaults to
30000 including queue time. Expiry returns 504. Cancellation is cooperative,
so a running handler retains its slot until it returns. Kore continues
serving native services while the application thread runs.

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
`knurl.acl{...}`. Core retains the ACL for each registration. Use the
[Core ACL policy](acl.md) fields, including `allow` and `deny` rule tables with
`addresses`, `countries` and `unknown` fields.

Native routes serve entirely in C. Empty `hooks = {}` and server
`callbacks = {}` tables are accepted. The inline handler and worker hook
methods are the Lua callback entry points. Authorization uses native ACLs.
