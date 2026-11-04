# Upload callbacks

Include `<rotmos/upload.h>` or `<rotmos/rotmos.h>`. The stream facade lifts
Vectis's open/write/finish/close lifecycle onto the existing managed body plan.
It adds no transport, parser, Vectis state, or private dependency types.

Initialize `rotmos_upload_stream_config`, set method/path, callbacks and userdata,
then register with `rotmos_router_upload_stream` or
`rotmos_route_group_upload_stream`. Registration copies the declaration and path;
userdata remains caller-owned. It opens no resource and invokes no callback.
The default body policy retains Vectis's upload bound; set `body.max_bytes` for
the route and respect the server's hard ceiling. STREAMING_UPLOAD and STREAM
both select the same live STREAM delivery here. They never materialize or spool
the body. Managed POST/PUT/PATCH still require Content-Length.

Auth and before hooks run before body admission. An early staged reply bypasses
the upload callbacks. Open runs on the first admitted body callback, or at body
completion for an empty upload. Oversized or invalid framing/length declarations
cannot open a destination. Open is optional; without it state is NULL. Open
failure owns its own partial cleanup; successful open owns one close callback.

Write receives a bounded chunk and returns a positive consumed prefix, or AGAIN
with zero. The unconsumed suffix remains transport-owned. Both a partial prefix
and AGAIN pause input until `rotmos_request_body_resume`. A callback ready for
another turn may call resume before returning; delivery never recurses. A
callback waiting for external work instead resumes from its later owner-loop
notification. Zero-progress OK, overconsumption and bytes accompanying an error
are invalid. No callback may wait for another thread, perform blocking reader
work, or call Lua on a foreign thread.

Finish runs once after all admitted input has been consumed, including for an
empty body. It selects a staged reply; then normal after hooks may inspect or
replace that reply before submission. Failure or an absent reply takes the
normal bounded error response path. Close runs once on the owner loop after
success, callback failure, disconnect, timeout or shutdown, with still-valid
request metadata and the terminal cause. Cancel application timers and watches
there. The facade does not consume `rotmos_request_userdata`; hooks and handlers
retain its application meaning.

The [public-only fixture](../tests/upload-runtime.c) demonstrates registration,
group hooks, prefix consumption, a timer-driven resume, empty input, rejection
before open, error cleanup, and disconnect during input. It also checks the
stateless form with no open callback.

File destinations and blocking upload-reader registration remain subsequent
implementation surfaces under the [full server contract](server-api-spec.md).
[Native output sources](responses.md) already use the shared bounded post-fork
executor; Lua callbacks use owner-loop delivery.
