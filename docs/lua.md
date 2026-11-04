# Lua server facade

`require("rotmos")` uses the host's Lua 5.5.1 state. The host creates, configures
and closes that state. The separately built C binding links the installed public
shared librotmos API and the same shared Lua runtime as the host. It includes no
Kore declarations, core-private headers, alternate router or transport executor.
`<rotmos/lua.h>` belongs to this binding product; it is absent from the C SDK.

The first binding surface covers server declaration/run/stop/close, listener
snapshots, lifecycle callbacks, route counts, ordinary routes, groups, request
lookups, buffered bodies, staged bytes/files and live response sources. Its
behavior comes from Vectis's [callback routes](../../vectis/manual/lua-app.md)
and [existing bridge](../../vectis/src/vectis_cli.c), rather than a new server
framework. The [integration contract](vectis-integration-contract.md) remains
the complete replacement requirement.

```lua
local rotmos = require("rotmos")
local server = assert(rotmos.server {
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

Before hooks may short-circuit. After hooks receive the original handler value
and may retain, mutate or replace it. Only the final selected declaration is
converted, under protection that includes response/source getters and allocation.
Nil means 204; strings and numbers produce text. Response declarations accept
`status_code`/`status`, headers, content type and body/file/stream source. Repeated
headers can use an ordered array of `{name=..., value=...}` pairs.

Legacy `stream_source` tables retain `read(max_bytes)`, optional `reset()` and
`close()`. The binding calls reset once before transfer when present. Reads accept
strings/numbers and nil/false/empty-string EOF, retain only an oversized chunk's
remaining suffix, and drain it before another callback. Delivery uses the public
source executor with bounded transport chunks; it creates no full-body buffer or
temporary response file. `rotmos.source{...}` directly exposes the explicit
producer convention: bytes or `nil, rotmos.EOF/AGAIN`, and `close(cause)`.

Request tables copy scalar metadata and retain scoped lookup helpers. A saved
lookup returns `nil, error` after request close. Binary bodies preserve NUL.
Ordered headers/query pairs and first-value lookups use public request APIs.
Absent values return nil. The backend retains Vectis's rejection of empty wire
header fields; decoded empty query values remain distinct from absence.

`rotmos.count(decimal_text)` or `rotmos.count(high, low)` represents unsigned
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

## Building and embedding

```sh
make lua                    # Selected x86_64-linux-gnu toolchain, build/debug/lua.
make lua-all                # All seven explicit lifecycle targets, serially.
ctest --test-dir build/debug/lua --output-on-failure
```

The ordinary core build first installs its SDK under the selected build tree.
The independent Lua CMake project discovers that installed shared SDK plus the
selected dependency SDKs. Its compilation database is `build/debug/lua`, matching
the Lua paths in `.clangd`. Neither the build nor the shipped binding owns an
interpreter. Binding install layout is `lib/lua/5.5/rotmos/native.so`,
`share/lua/5.5/rotmos.lua`, its embedding header and `rotmos_lua` CMake package.

Embedding applications may link `rotmos::lua` and call
`rotmos_lua_preload(existing_state, &error)` before `require("rotmos")`. The host
must open the base, package and string libraries. Registration is idempotent and
rejects conflicting loaders/loaded modules. It neither opens host libraries nor
changes unrelated globals. A standalone Lua host instead uses the installed Lua
and C module search paths.

The short [installed-runtime fixture](../tests/lua/foundation.lua) exercises both
preloading and dynamic loading through a [host-owned VM](../tests/lua/host.c).
It covers wire behavior, route/group parity, binary bodies, protected getters,
chunk remainders, scoped invalidation, source cleanup, and allocation failure in
registration/reply conversion/producer setup.

## Remaining binding surfaces

This is the first Lua implementation slice, not replacement readiness. TLS and
remaining policy/deadline/admission settings, auth providers, body materialization
and upload callbacks, file-backed `spooled_source`, schemas/JSON/XML, REST/OpenAPI,
assets/WebDAV, proxy/WebSocket/SSE and direct loop/response operations still need
their public C bindings. Known unsupported options return an error. No transport
or application mechanism belongs in a temporary Vectis adapter to fill those
gaps. Lua source archive, rockspec/source-rock packaging and final installed
consumer/artifact checks remain part of the active implementation goal.
