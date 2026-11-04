# LoneJSON codecs and typed routes

Include [`<rotmos/lonejson.h>`](../include/rotmos/lonejson.h) explicitly. The core
umbrella remains independent of LoneJSON declarations. CMake consumers link
`rotmos::shared` or `rotmos::static`; both expose the matching public LoneJSON
dependency. Its header and library come from the selected released SDK.

`rotmos_json_schema_from_map` converts a C-owned map into LoneJSON's public schema
view. A supplied runtime is borrowed; NULL selects an independently owned default
runtime for the operation or request. Existing public schema views can be passed
directly. Views must match the released LoneJSON ABI and their map's record size,
which is bounded to 1..10 MiB. Map metadata and borrowed runtimes remain valid
through route destruction or producer close. Registration copies view values,
patterns, hooks and userdata pointers without parsing or executing callbacks.

Initialize `rotmos_json_route_config`, set independent input/output schemas and a
typed handler, then register with `rotmos_router_json_route`. The handler receives
read-only view descriptors whose `record` pointers refer to mutable, initialized
typed storage. NULL schemas produce NULL views. Fields follow LoneJSON's public
assignment and cleanup rules. Successful handlers with output schemas select a
generated 200 JSON reply; no output schema selects 204. Route-group variants use
the existing prefix and hook machinery.

```c
rotmos_json_route_config json;
rotmos_json_route_config_init(&json);
json.method = ROTMOS_HTTP_POST;
json.path = "/echo";
rotmos_json_schema_from_map(NULL, &input_map, &json.input, &error);
rotmos_json_schema_from_map(NULL, &output_map, &json.output, &error);
json.handler = echo;
status = rotmos_router_json_route(rotmos_server_router(server), &json, NULL, &error);
```

The default transport policy is MATERIALIZED with the ordinary inherited body
limit and 256 KiB memory threshold. Select a finite memory-only budget, or declare
`server_config.limits.body_spool_directory` and allow spooling. Startup validates
the whole declared policy; it rejects a potentially spilling plan without that
permission and directory. Vectis maps its existing effective body/spool settings.
Explicit BUFFERED policies retain their memory-bound rules. A JSON policy supplied
to this typed facade selects its materialized transport path. Generic raw JSON
body-plan execution remains a separate implementation surface.

`rotmos_request_json_into` and `rotmos_request_json_validate` operate on completed
BUFFERED/MATERIALIZED bodies. They rewind before and after parsing or validation,
including malformed-input failure. File-backed bodies feed LoneJSON incrementally
through the ordinary body reader, without another complete JSON text allocation.
Decoding a typed object still materializes that record. Callers of `json_into`
initialize their destination and clean up partial fields through LoneJSON.
Errors retain the LoneJSON status and line/column/offset in the public error;
input shape/field-overflow failures become HTTP 400 in the typed facade.

Auth/before hooks finish before typed decoding and handler execution. After hooks
may inspect or replace the selected staged source before transport submission.
Each request owns its input/output storage through producer and request cleanup,
so an asynchronous reply can borrow an input record safely. There is no JSON text
round trip or mandatory table conversion between parser, handler and generator.

`rotmos_router_json_typed_route` instead gives the handler a staged reply. It can
choose a different output schema and status per response. An unselected successful
reply retains the ordinary 204 behavior. `rotmos_reply_json_materialized` finishes
serialization synchronously and accepts stack records. Its separately named
materialized operation has a finite output bound, defaulting to 10 MiB; failure
leaves the prior reply intact. `rotmos_json_serialize_materialized` returns an owned
byte value released by `rotmos_owned_bytes_cleanup`.

`rotmos_reply_json_spooled` preserves Vectis's `response_json_generated` behavior:
LoneJSON's public sink serializer writes directly into a private unlinked response
file and completes before returning. It retains no record/schema pointer and
constructs no full JSON buffer, so stack records are safe. The directory defaults
to `/tmp`; this explicit file-backed helper adds no payload bound. Replacement
and failure preserve headers and release file ownership through the common
[reply materialization path](responses.md). The
[spooled fixture](../tests/reply-spooled.c) verifies independent serialized bytes
after record mutation, descriptor cleanup and actual disk-write failure.

`rotmos_source_json` and `rotmos_reply_json` use LoneJSON's public pull generator.
They keep output bounded by the consumer's read budget and preserve generic
source backpressure, cancellation and suppression behavior. Caller-owned records
stay valid and unchanged until the configured release callback. Success owns one
release notification; failed construction/reply selection takes nothing. Schema
and record owners can be retained by a Lua binding through that public callback.
The source configuration is copied; the record itself is borrowed. Materialized
serializers and generated sources are distinct operations.

`rotmos_json_array_new/feed/finish/destroy` provide actual incremental selected-
array input through LoneJSON's public push parser. The selector is copied. Each
feed offers at most 64 KiB; completed items reach the callback before input EOF.
One typed record is cleaned and reused after each synchronous callback. The
callback accepts with OK or fails terminally; it cannot pause a partially consumed
chunk with AGAIN. A producer waits for consumer capacity before offering its next
chunk. The parser never collects an array, and recursive feed/finish/destroy
latches STATE. Truncation and callback failures are sticky.

The [API fixture](../tests/json.c) checks small generator reads over a 128 KiB
record, explicit output limits, release ownership, selected-item delivery before
EOF, reentry and truncation. The [wire fixture](../tests/json-runtime.c) checks
copied registrations, group hooks, file-backed input and rewind, malformed and
overflowing input, after-hook replacement, auth short circuit, per-response output
schemas, a 64 KiB reply borrowing its input record, and startup spool policy.
Both also run against relocated static/shared SDKs.

The [Lua codecs](lua.md) now bind materialized serialization and generated
Source/callback output through these public operations and released LoneJSON
interop. Their private table preparation, mutation/GC/reentry, spooled input and
buffered external-record cases preserve Vectis's writer policy. Lua mapped input
uses the provider's public owned-record parser over rotmos body/file readers.
Fixed-schema and general typed Lua routes compose the public router/codecs, with
materialized record replies and live output from private table preparation.
Their [installed fixture](../tests/lua/typed.lua) covers captured JSON, file/spooled
input, record lifetime, multiple output schemas and the shared hook chain.

[Buffered REST](rest.md) adds generic decode/validate/error policy over ordinary
staged routes and explicit materialized replies. [OpenAPI](openapi.md) projects
borrowed LoneJSON maps into router-owned document annotations. Both retain the
public schema/dependency boundary.
