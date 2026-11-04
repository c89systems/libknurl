# Buffered REST routes

> Implementation reference under correction. The [extraction contract](server-api-spec.md) governs required behavior and scope; this document describes current code, not compatibility or an additional product requirement. See the [audit](server-api-audit.md) for known failures.


Include `<rotmos/lonejson.h>`. The REST facade lifts Vectis's buffered JSON
workflow onto the ordinary route/reply chain: decode, optional validation,
handler, error policy and reply composition. Application schemas, business
logic, custom envelopes and outbound HTTP/REST clients remain application-owned.

Initialize `rotmos_rest_route_config`, set the route and handler, and register
with `rotmos_router_rest_route` or `rotmos_route_group_rest_route`. Registration
copies the declaration and path and invokes no callback. Its default body is
BUFFERED with a 1 MiB bound and memory budget. An explicit BUFFERED limit must
fit its memory budget. NONE supports bodyless routes. These operations use the
existing managed Content-Length admission and never spool or stream the body.

Auth and before hooks run before REST decoding. With nonempty input, the default
decoder uses LoneJSON to validate and compact one retained JSON value. The
handler's `decoded` pointer borrows a `const lonejson_json_value *`; use its
public visitor/value operations. Empty input produces NULL, while JSON `null`
produces an actual value handle. `decode_disabled` passes NULL and leaves raw
request body access available through the ordinary public helpers.

A custom `decode(request, body, &decoded, userdata, error)` can prepare an
application value, including a Lua facade's retained registry value. Success
transfers one configured `release(decoded, userdata)` notification when decoded
is non-NULL. Without a release callback the caller manages that custom value's
lifetime. Failure cleans its own partial result. The value stays alive through response
source cleanup, including when an output source borrows it. All callbacks and
release notifications execute on the owning loop. No runtime/private layouts
cross this API boundary.

Validation receives the decoded value and staged reply. It returns OK to
continue, or selects a reply and sets finish to short-circuit the handler, or
returns an error. Successful handlers use the ordinary reply helpers; an
unselected result retains the ordinary 204 behavior. After hooks may inspect or
replace the result before submission, including selected validation/error replies.
An auth/before short circuit retains the ordinary middleware ordering.

The optional `on_error` receives the decode/validate/handler phase, full public
error, request and staged reply. Return OK with a selected reply to handle it;
an unselected result or callback failure takes the default error path. The core
removes partial handler payloads and keeps accumulated headers before invoking
this callback. The default envelope is `{"ok":false,"error":{...}}`, with kind,
message, status and source_code. Input/validation INVALID/LIMIT/OVERFLOW errors
become 400; other defaults map INVALID/CONFLICT/NOT_SUPPORTED/TIMEOUT to
400/409/501/504, and remaining failures to 500. An encoding failure uses a fixed
finite fallback without recursively calling application error policy.

`rotmos_reply_rest_json` synchronously composes a buffered response from a public
LoneJSON value. Initialize `rotmos_rest_reply_config`; zero/default values select
200, `application/json; charset=utf-8`, a 10 MiB output bound and a newline.
`newline_disabled` omits the newline. The bound includes it. Add or replace
headers through the normal reply operations. Failure preserves the previous
selected reply. `rotmos_reply_error_json` supplies the same generic envelope
independently, with an 8192-byte output bound and optional HTTP status override.
These helpers materialize output before returning. Fast typed routes and live
JSON producers retain their separate [record/generator APIs](json.md).

The [public wire fixture](../tests/rest-runtime.c) exercises hooks, parse and
validation failures, empty versus null input, custom decode/release, a reply
source borrowing decoded state, custom and failed error handlers, bounds,
bodyless/raw routes, JSON newline behavior and OpenAPI generation during a run.
It also runs against relocated static and shared SDKs.

`rotmos.rest` lifts the server half of Vectis's Lua REST facade. `route(server,
options)` and `group(server, options)` use the ordinary public router and its
auth/before/after chain. Nonempty buffered input becomes `request.json` through
the released LoneJSON Lua decoder; empty input leaves it nil and JSON null uses
LoneJSON's null sentinel. `decode_json=false` leaves explicit buffered input raw.
Body policy accepts BUFFERED or NONE, defaulting to a 1 MiB buffered limit/budget
when decoding is enabled. No request spool or stream is hidden in these helpers.

`validate(json, request)` may return nil/true to continue, an ordinary reply to
short-circuit, or false/detail to reject with 400. Validator exceptions also
produce 400. `handler(request)` returns an ordinary reply or a generic JSON
value, optionally followed by response options; nil without options means 204.
`response` supplies default JSON options. `error_handler(original_error,
request)` receives the original thrown Lua value and may return a reply.
Unhandled exceptions produce a 500 envelope. After hooks receive the original
selected Lua declaration, with conversion deferred until the hook chain ends.
Group defaults and per-route overrides retain Vectis's existing forms.

`rest.json(value, options)` composes an ordinary buffered response. Options are
status/status_code, content_type, headers, newline (default true) and max_bytes
(zero/default adds no output-size ceiling; an explicit bound includes newline).
Generic traversal/validation uses the released LoneJSON Lua encoder once. Its
encoded bytes become the buffered response body directly, with an optional
newline; no second JSON parser/writer or native reply buffer is involved.
Invalid keys/types, cycles, nonfinite numbers
and output limits return nil/error. This explicitly materialized path does not
replace typed-record or live JSON APIs. `rest.error_response(error, options)`
preserves custom error fields, decorates a copied value, maps the public statuses
to HTTP, and uses a finite fallback if that envelope cannot be encoded.

The [installed Lua fixture](../tests/lua/rest.lua) covers these server workflows,
raw/null/empty bodies, validation/handler errors, recovery identity, live source
replies, group middleware, admission limits and allocation failures.
