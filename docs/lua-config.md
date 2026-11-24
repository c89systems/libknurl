# Configure libknurl from Lua

The optional Lua 5.5.1 module turns Lua declarations into C server, route and ACL
configuration before serving begins. Kore handles admission, routing, I/O and
request cleanup without entering Lua. A Lua application's HTTP listener can
serve application routes in a separate process.

## Build and load

```sh
make test LUA_CONFIG=ON BUILD_DIR=build/lua-config
cmake --install build/lua-config --prefix "$PWD/build/lua-install"
cmake --install build/lua-config --prefix "$PWD/build/lua-install" --component lua
```

Load `knurl` with a Lua 5.5.1 interpreter or embedding host for the selected SDK
target. Add `<prefix>/lib/lua/5.5/?.so` to `LUA_CPATH` and make the selected C SDK
runtime dependencies available to the platform loader. On Linux that typically
means adding the pinned cpkt, lonejson and pslog SDK library directories to
`LD_LIBRARY_PATH`. The standalone executable and release packages are described
in [runtime direction](runtime-direction.md).

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

-- Start the Lua application separately. It receives the original path/query.
check(server:application{
  path = '^/app/.*$', target = 'http://127.0.0.1:9000',
  total_timeout_ms = 30000,
})

check(server:validate())
check(server:run())
```

`knurl.server` uses the C production webserver profile with a single HTTP
listener (default `127.0.0.1:8080`) and one HTTP worker. Set `workers`,
`max_connections` or `backend_quiet` in the server declaration as needed.
`validate()` checks declarations and `preflight()` also probes startup resources.
`run()` handles termination signals for the duration of the run and returns
after native shutdown. It must run from a host that can safely fork.
`close()` releases the inactive server and its routes. An error returns
`nil, error` with a numeric status, message and optional detail.

| Registration | Fields and behavior |
| --- | --- |
| `text`, `json` | A fixed `body`, HTTP `status` (default 200), optional `content_type` and `cache_control`. JSON bodies contain already encoded JSON. Bodies may contain binary bytes. |
| `redirect` | Required `location`, optional 3xx `status` (default 302), `body`, `content_type` and `cache_control`. |
| `static_file`, `static_directory` | A `file_path` or `root_dir`, respectively. The existing [static service](assets.md) handles ranges, compression, indexes and MIME policy. |
| `upload_file` | A `file_path` destination and optional `max_body_bytes`. Core [file upload](uploads.md) performs native I/O. |
| `proxy`, `application` | A fixed HTTP(S) `target` and optional [proxy limits and upstream TLS fields](proxy-http.md). `application` uses the same native proxy with a 30 s default total deadline. |

Each declaration takes a `path`, optional ACL and optional `method` or array
`methods` of HTTP method names. Fixed responses default to GET, static routes to
GET and HEAD, uploads to POST and proxies to all supported methods. Fixed
responses, uploads and proxies accept `path_kind` with `literal`, `params` or
`regex`. Omit it for path inference. Register in the same order you would use in
the C router.

`server:group{prefix = ..., acl = ...}` scopes fixed responses, static files and
file uploads to a shared prefix and ACL. Declare proxy and application routes
directly on the server. Supply an ACL table or an object returned by
`knurl.acl{...}`; Core retains the resulting ACL for each registration. Use the
[Core ACL policy](acl.md) fields, including `allow` and `deny` rule tables with
`addresses`, `countries` and `unknown` fields.

Omitted hooks and empty `hooks = {}` or server `callbacks = {}` tables carry no
runtime behavior. Setting a request-time Lua hook is a configuration error.
Authorization here uses native ACLs.

## Application isolation

Run and supervise the Lua application's HTTP listener outside the Kore frontend
process. The frontend performs proxy I/O on its C event loop. A paused or blocked
Lua application can delay proxied requests until their deadlines without
stalling the event loop that serves native routes. Proxied routes share Core's
bounded per-worker proxy exchange capacity. Routes that proxy to the same
blocked upstream still depend on that upstream.
