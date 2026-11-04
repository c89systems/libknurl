# Buffered REST routes

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
It also runs against relocated static and shared SDKs. The Lua facade remains
part of the [full implementation goal](implementation-plan.md).
