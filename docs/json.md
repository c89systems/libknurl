# LoneJSON codecs and typed routes

Include [`<knurl/lonejson.h>`](https://github.com/c89systems/libknurl/blob/trunk/include/knurl/lonejson.h) explicitly. The core
umbrella remains independent of LoneJSON declarations. CMake consumers link
`knurl::shared` or `knurl::static`; both expose the matching public LoneJSON
dependency. Its header and library come from the selected released SDK.

The selected LoneJSON 0.49.0 SDK uses ABI 29. Record lifecycle calls
`lonejson_init`, `lonejson_reset` and `lonejson_cleanup` return a status and take
an optional error output. Check initialization/reset before using records.
Deep-map traversal grows checked heap workspaces; allocation failure leaves the
record unchanged and can be retried after restoring allocator availability.

`knurl_json_validate_cstr` preserves Vectis's standalone validation helper:
NULL is INVALID, one NUL-terminated value is validated by LoneJSON, and parser
failures return INVALID with the LoneJSON source/code and line/column message.
It builds no mapped record or DOM and clears diagnostics on success.

`knurl_json_schema_from_map` converts a C-owned map into LoneJSON's public schema
view. A supplied runtime is borrowed; NULL selects an independently owned default
runtime for the operation or request. Existing public schema views can be passed
directly. Views must match the released LoneJSON ABI and their map's record size,
which must match the map's nonzero struct size. No additional record-size cap is
imposed by this generic schema API. The Vectis adapter separately preserves its
baseline route declaration checks: nonzero, at most 10 MiB and exactly the map's
struct size when a map is present. This does not cap encoded output.
Map metadata and borrowed runtimes remain valid
through route destruction or producer close. Registration copies view values,
patterns, hooks and userdata pointers without parsing or executing callbacks.

Initialize `knurl_json_route_config`, set independent input/output schemas and a
typed handler, then register with `knurl_router_json_route`. The handler receives
read-only view descriptors whose `record` pointers refer to mutable, initialized
typed storage. NULL schemas produce NULL views. Fields follow LoneJSON's public
assignment and cleanup rules. Successful handlers with output schemas select a
materialized 200 JSON reply before after-hooks, preserving Vectis's ordinary
route serialization timing without a new output-size ceiling. Input/output
records are then cleaned. No output schema selects 204. Route-group variants use
the existing prefix and hook machinery.

```c
knurl_json_route_config json;
knurl_json_route_config_init(&json);
json.method = KNURL_HTTP_POST;
json.path = "/echo";
knurl_json_schema_from_map(NULL, &input_map, &json.input, &error);
knurl_json_schema_from_map(NULL, &output_map, &json.output, &error);
json.handler = echo;
status = knurl_router_json_route(knurl_server_router(server), &json, NULL, &error);
```

The default transport policy is MATERIALIZED with the ordinary inherited body
limit and 256 KiB memory threshold. Select a finite memory-only budget or allow
spooling, with an optional `server_config.limits.body_spool_directory`. A NULL
directory uses the ordinary temporary directory. Registration validates the
whole declared policy. Vectis maps its existing effective body/spool settings.
Explicit BUFFERED policies retain their memory-bound rules. A JSON policy supplied
to this typed facade selects its materialized transport path. Generic raw JSON
body-plan execution remains a separate implementation surface.

`knurl_request_json_into` and `knurl_request_json_validate` operate on completed
BUFFERED/MATERIALIZED bodies. They rewind before and after parsing or validation,
including malformed-input failure. File-backed bodies feed LoneJSON incrementally
through the ordinary body reader, without another complete JSON text allocation.
Decoding a typed object still materializes that record. Callers of `json_into`
initialize their destination and clean up partial fields through LoneJSON.
Errors retain the LoneJSON status and line/column/offset in the public error;
input shape/field-overflow failures become HTTP 400 in the typed facade.

`knurl_json_parse_source` uses the same mapped reader path from a borrowed
source's current position. It never resets or closes the source. The caller
initializes and cleans the record, including partial failures. `AGAIN` is a
terminal STATE error for this synchronous call; a live blocking source belongs
on a native reader thread. Lua `parse`/`parse_record` and `request.json_record`
create their own record with public LoneJSON interop and use these Core parse
operations. The binding has no separate body-reader or rewind implementation.

Auth/before hooks finish before typed decoding and handler execution. After hooks
may inspect or replace the selected staged source before transport submission.
Ordinary route hooks see completed JSON bytes. Typed-reply routes retain their
input storage through producer and request cleanup, so an explicitly selected
asynchronous reply can borrow that record safely. There is no JSON text round
trip or mandatory table conversion between parser and handler.

`knurl_router_json_typed_route` instead gives the handler a staged reply. It can
choose a different output schema and status per response. An unselected successful
reply retains the ordinary 204 behavior. `knurl_reply_json_materialized` finishes
serialization synchronously and accepts stack records. Its separately named
materialized operation accepts an optional output bound; zero adds no ceiling. Failure
leaves the prior reply intact. `knurl_json_serialize_materialized` returns an owned
byte value released by `knurl_owned_bytes_cleanup`.

`knurl_json_write_sink` emits bounded chunks synchronously through LoneJSON's
sink serializer without collecting output. A failed callback stops output and
preserves its status/diagnostics. `knurl.json.write` binds this operation;
`knurl.xml.write` binds the existing XML sink operation. The binding protects
Lua callbacks, counts accepted bytes and retains its private record; it does
not construct a temporary Source or run another serialization path. External
mutable records use buffered serialization; callback writers require value tables.

`knurl_reply_json_spooled` preserves Vectis's `response_json_generated` behavior:
LoneJSON's public sink serializer writes directly into a private unlinked response
file and completes before returning. It retains no record/schema pointer and
constructs no full JSON buffer, so stack records are safe. The directory defaults
to `/tmp`; this explicit file-backed helper adds no payload bound. Replacement
and failure preserve headers and release file ownership through the common
[reply materialization path](responses.md). The
[spooled fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/reply-spooled.c) verifies independent serialized bytes
after record mutation, descriptor cleanup and actual disk-write failure.

`knurl_source_json` and `knurl_reply_json` use LoneJSON's public pull generator.
They keep output bounded by the consumer's read budget and preserve generic
source backpressure, cancellation and suppression behavior. Caller-owned records
stay valid and unchanged until the configured release callback. Success owns one
release notification; failed construction/reply selection takes nothing. Schema
and record owners can be retained by a Lua binding through that public callback.
The source configuration is copied; the record itself is borrowed. Materialized
serializers and generated sources are distinct operations.

`knurl_json_array_each_source` and `knurl_request_json_array_each` lift Vectis's
selected-array pull loop. The caller supplies initialized, empty mapped item
storage; each item is reset, parsed, passed with a zero-based index, and reset
before the next read, including callback/parse failures. Reset retains
caller-owned fixed-capacity array backing and enforces its declared capacity;
the caller owns final cleanup. Record reuse is selected with LoneJSON's
public per-stream operation for both owned and supplied runtimes. A supplied
runtime can retain its default clearing policy; its configuration is unchanged.
The source stays borrowed and open. NULL/empty selector selects a root array;
a non-empty selector names one
direct object key. Request iteration rewinds its completed memory/file body
before and after, including errors. Live blocking sources run on native threads;
a synchronous reader returning AGAIN fails with STATE. No document/array is
collected. Callback failures stop immediately and retain their error details.
The source and item storage must remain valid and exclusively used through
return.

`knurl_json_array_rewrite_source` lifts Vectis's reader/sink rewrite helper via
LoneJSON's public rewrite options. It borrows the current source position, mapped
item storage and callbacks, supports the dependency's KEEP/DROP/replacement and
append operations, and emits output before input EOF. Neither the document nor
selected array is collected. The C reference adapter preserves the original
source/path/memory declaration precedence and legacy error translation.

Lua `knurl.json.array_each{schema,selector,callback,source=...}` binds that public
C operation. Supply exactly one of `source`, `path` or already-buffered `data`.
`request.json_array_each{schema,selector,callback}` binds its request counterpart.
Both call `callback(table,index)` and return `true,count` or `nil,error`; callbacks
can throw or return `nil,error` to stop. Each table comes from the binding's own
Lua-owned record via released `lonejson_lua_record_to_table`, before C cleans the
record. Retaining a table therefore does not require exposing C-owned storage or
adding dependency APIs. A borrowed Lua Source rejects read/reset/close/transfer
during iteration, remains open afterward, and retains its normal error state.

The [API fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/json.c) checks small generator reads over a 128 KiB
record, explicit output limits, release ownership, caller-owned selected items
before EOF, source failures, callback stopping, cleanup and truncation. The
[wire fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/json-runtime.c) checks
copied registrations, group hooks, file-backed input and rewind, malformed and
overflowing input, after-hook replacement, auth short circuit, per-response output
schemas, a 64 KiB reply borrowing its input record, file-backed array iteration
through the reference adapter, rewind after callback failure and startup spool
policy. Lua cases also cover retained tables, GC, allocation/callback failure,
borrowed source ownership and request iteration.
Both also run against relocated static/shared SDKs.

The [Lua codecs](lua.md) now bind materialized serialization and generated
Source/callback output through these public operations and released LoneJSON
interop. Their private table preparation, mutation/GC/reentry, spooled input and
buffered external-record cases preserve Vectis's writer policy. Lua mapped input
uses the provider's public owned-record parser over knurl body/file readers.
Fixed-schema and general typed Lua routes bind the public C typed registrations,
using their optional record provider to supply pinned Lua-owned storage. C owns
input parsing, dispatch, serialization and hook ordering. The binding releases
its record reference on completion/failure; a record retained by Lua remains
owned by Lua. No second route preparation path or record conversion is needed.
Their [installed fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/lua/typed.lua) covers captured JSON, file/spooled
input, record lifetime, multiple output schemas and the shared hook chain.

[Buffered REST](rest.md) adds generic decode/validate/error policy over ordinary
staged routes and explicit materialized replies. [OpenAPI](openapi.md) projects
borrowed LoneJSON maps into router-owned document annotations. Both retain the
public schema/dependency boundary.
