# Knurl executable

Archived pre-Luthrum reference; this product is deferred. See the [handoff](../README.md).

Run `knurl server.lua`. The script returns a configured `require('knurl').server`.
`--check server.lua` executes and validates it without running listeners.
`--dump-examples [DIRECTORY]` writes four servers without overwriting files.
Create `www/index.html` for the static example and a writable `dav` directory for
WebDAV, passing its absolute path in `WEBDAV_ROOT` (default `/srv/knurl/dav`).
Replace its illustrative key before use and configure HTTPS.

The executable embeds Lua 5.5 and `knurl`, `curl`, `async.curl`, `lonejson`, and `pslog`.
No interpreter, shared libraries, or installed module tree is needed. Lua scripts
are trusted programs. Relative paths use your working directory. SIGTERM requests
graceful shutdown; libpid0 handles PID 1 signal forwarding and adopted children.
`arg[0]` is the script path and `arg[-1]` the executable path in both run and
check modes. Use environment variables for deployment-specific example settings.

The production profile is selected by default. Override fields in `knurl.server{}`;
inspect `app:effective_config()`. One worker is the default. Each worker handles
multiple connections. The production profile hides automatic server-error
diagnostics from clients; set `http.hide_server_errors=false` explicitly when
debugging in a trusted setting. Outbound `async.curl` calls inside ordinary routes yield while
waiting, allowing that worker to serve other connections. Native curl callbacks
must complete synchronously. Use the synchronous `curl` module for startup calls;
`async.curl` requires a yieldable route.

```lua
local k = require('knurl')
local curl = require('async.curl')
local log = require('pslog').new_json()
local app = assert(k.server{bind='0.0.0.0',port=8080,logger=log})
assert(app:get('/lookup',function()
  local r = curl.json{url='https://example.com/api',timeout_ms=2000}
  if not r.ok then return {status=502,body='Upstream failed'} end
  return {status=r.status,body=r.body,content_type='application/json'}
end))
return app
```

`curl.perform` buffers responses up to 8 MiB and headers up to 64 KiB.
`write(chunk)` streams directly to your callback; return true to accept a chunk.
`read(capacity)` streams uploads; return a string no larger than capacity, or nil
at EOF. `upload_path` and `download_path` transfer files incrementally.
`curl.stream_json{request={schema=...,value=...},response={schema=...}}` feeds
the LoneJSON adapters directly. It does not build an intermediate JSON string.
By default two task threads serve at most 16 admitted transfers per worker;
configure bounds with `knurl.async_configure`. Timeout defaults are 30s total,
10s connect; override with `timeout_ms` and `connect_timeout_ms`.

Install system trust at `/etc/ssl/certs/ca-certificates.crt` (for example copy it
from Alpine when building a scratch image). No CA bundle is embedded. Explicit
trust overrides fail closed if invalid. Configure manual TLS/ACME through the
existing Knurl Lua TLS API. For ACME set `tls.acme.state='file'` and an explicit
`tls.acme.runtime_directory` on durable writable storage, created with mode 0700.
The file policy fsyncs keys, certificates and directories before publication;
custom state callbacks can implement an external durable store.

The combined example uses `COUNTRY_MMDB` and logs download/extraction guidance
when absent. Obtain the MMDB from https://db-ip.com/db/download/ip-to-country-lite
and extract it with `gzip -dk dbip-country-lite-YYYY-MM.mmdb.gz`. DB-IP Lite data
requires CC BY 4.0 attribution and a link to https://db-ip.com/ in web applications.
The database is not included in this distribution.
