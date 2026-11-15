# Buffered REST routes

Include `<knurl/lonejson.h>`. The REST facade lifts Vectis's buffered JSON
workflow onto the ordinary route/reply chain: decode, optional validation,
handler, error policy and reply composition. Application schemas, business
logic, custom envelopes and outbound HTTP/REST clients remain application-owned.

Initialize `knurl_rest_route_config`, set the route and handler, and register
with `knurl_router_rest_route` or `knurl_route_group_rest_route`. Registration
copies the declaration and path and invokes no callback. Its default body is
BUFFERED with a 1 MiB bound and memory budget. Explicit policies preserve the
ordinary buffered route's memory/spill settings. NONE supports bodyless routes.
These operations use the existing admission and completed-body access; REST
decoding itself is buffered, including a completed spill when selected. A
completed file-backed body is explicitly copied into memory for decoding, up to
its admitted limit; lengths that cannot fit `size_t` fail with OVERFLOW. The
transport body remains file-backed and its reader is rewound before and after
the copy. Memory-backed bodies retain their borrowed view without another copy.

Auth and before hooks run before REST decoding. With nonempty input, the default
decoder uses LoneJSON to validate and compact one retained JSON value. The
handler's `decoded` pointer borrows a `const lonejson_json_value *`; use its
public visitor/value operations. Empty input produces NULL, while JSON `null`
produces an actual value handle. `decode_disabled` passes NULL and leaves raw
request body access available through the ordinary public helpers.

A custom `decode(request, body, &decoded, userdata, error)` can prepare an
application value. It runs even on empty input unless decoding is disabled. Success
transfers one configured `release(decoded, userdata)` notification when decoded
is non-NULL. Without a release callback the caller manages that custom value's
lifetime. Decoder input bytes remain borrowed through request cleanup, including
custom-value release. Failure cleans its own partial result. The value stays alive through response
source cleanup, including when an output source borrows it. All callbacks and
release notifications execute on the owning loop. No runtime/private layouts
cross this API boundary.

Validation receives the decoded value and staged reply. It returns OK to
continue, or sets finish to short-circuit the handler, or
returns an error. Successful handlers use the ordinary reply helpers; an
unselected result retains the ordinary 204 behavior. After hooks may inspect or
replace the result before submission, including selected validation/error replies.
Setting finish does not require immediate reply selection: after hooks can
compose deferred replies, and an unselected result uses the ordinary 204.
An auth/before short circuit retains the ordinary middleware ordering.

The optional `on_error` receives the decode/validate/handler phase, full public
error, request and staged reply. Return OK with a selected reply to handle it;
an unselected result takes the default JSON error path. Callback failure instead
propagates as an ordinary route failure, preserving its diagnostic and skipping
after hooks. The core
removes partial handler payloads and keeps accumulated headers before invoking
this callback. The default envelope is `{"ok":false,"error":{...}}`, with kind,
message, status and source_code. Input/validation INVALID/LIMIT/OVERFLOW errors
become 400; other defaults map INVALID/CONFLICT/NOT_SUPPORTED/TIMEOUT to
400/409/501/504, and remaining failures to 500. An encoding failure uses a fixed
finite fallback without recursively calling application error policy.

`knurl_reply_rest_json` synchronously composes a buffered response from a public
LoneJSON value. Initialize `knurl_rest_reply_config`; zero/default values select
200, `application/json; charset=utf-8`, no added output ceiling and a newline.
`newline_disabled` omits the newline. The bound includes it. Add or replace
headers through the normal reply operations. Failure preserves the previous
selected reply. `knurl_reply_error_json` supplies the same generic envelope
independently, with an 8192-byte output bound and optional HTTP status override.
These helpers materialize output before returning. Fast typed routes and live
JSON producers retain their separate [record/generator APIs](json.md).

The [public wire fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/rest-runtime.c) exercises hooks, parse and
validation failures, empty versus null input, custom decode/release, a reply
source borrowing decoded state, custom and failed error handlers, bounds,
bodyless/raw routes, JSON newline behavior and OpenAPI generation during a run.
It also runs against relocated static and shared SDKs.
