# OpenAPI documents

> Implementation reference under correction. The [extraction contract](server-api-spec.md) governs required behavior and scope; this document describes current code, not compatibility or an additional product requirement. See the [audit](server-api-audit.md) for known failures.


Include `<rotmos/openapi.h>`. This surface lifts Vectis's route annotations and
LoneJSON-map projection into the router. It produces materialized JSON or YAML
OpenAPI 3.1.0 metadata without invoking handlers or requiring another application
route registry. The header forward-declares the optional LoneJSON map type;
include `<rotmos/lonejson.h>` when constructing map-backed schemas.

Initialize `rotmos_openapi_route_doc`, assign summary, operation_id and tags,
and use `rotmos_openapi_request_json` / `rotmos_openapi_response_json` with
`rotmos_openapi_lonejson_schema(name, map)` to add schema declarations. Names
default to the map name, then Anonymous. A raw response declaration with no map
represents a response without JSON content. The builder borrows strings/maps and
owns its helper-allocated response vector; `rotmos_openapi_route_doc_cleanup`
frees that vector only.

`rotmos_router_openapi_doc(router, methods, path, doc, error)` copies selectors,
strings, schema names, tags and response declarations. Maps and child-map
metadata remain borrowed, immutable and alive through router destruction; a Lua
binding retains their public schema owners. Builder strings and response storage
can be changed or released after successful attachment. Parameter paths use
Vectis's `:id` / `:id?` to `{id}` conversion. Disjoint method masks at the same
normalized path merge into one path item. Overlapping annotations and duplicate
response statuses fail with CONFLICT, preserving the previous annotations.
Attachment is inert and forbidden while the router is running. Route count still
counts actual route declarations; annotations can be supplied before their routes.

Initialize `rotmos_openapi_document` and call `rotmos_router_openapi`. NULL uses
the defaults: title `librotmos API`, version `0.0.0`, and a 10 MiB output bound.
Set max_bytes for another finite bound. Release successful output with
`rotmos_owned_bytes_cleanup`; failure returns an empty output. Generation is
synchronous and never creates a server, runs callbacks, or performs network I/O.
It can run inside an ordinary handler against the immutable running registry.

The projection retains Vectis's summaries, operation IDs, tags, JSON request and
response content, required fields, scalar types, arrays and nested-map references,
including its eleven HTTP/WebDAV method names. Schema collection is iterative;
recursive maps produce references without recursively walking the C stack.
Different maps sharing one component name fail with CONFLICT. Component
references escape JSON Pointer tokens and URI-fragment bytes. Dynamic strings
and keys use LoneJSON's quoting in both formats. Empty paths/components are
mappings, and empty properties/responses remain mappings in YAML.

The [public fixture](../tests/openapi.c) covers copied metadata, normalization,
merged methods, optional response schemas, aliases, reference escaping, required
fields, JSON validation, YAML layout, conflicting metadata and output bounds.
Its 512-node recursive graph runs on a default-stack thread, including musl.
The [REST wire fixture](../tests/rest-runtime.c) serves a document during a run
and verifies that annotation mutation is rejected there. Both are relocated
static/shared SDK consumers.

Lua `server:openapi_doc{path, method/methods, ...}` accepts Vectis's summary,
operation_id/operationId, tags, request and responses declarations. A schema is
`{name/schema_name=..., schema=lonejson_schema}`; names must be nonempty.
Responses accept status/status_code, description/desc and an optional named
schema. A provided responses array must be nonempty. The binding extracts only
released public schema views, pins their owners until server close, and copies
metadata through the public router API. Mutation of the original declaration
does not change the annotation. Failed attachments release their temporary
owners, including allocation failures.

`server:openapi{title, version, format, max_bytes}` returns buffered JSON by
default; yaml/yml select YAML. It retains C defaults and output bounds and may
run inside a handler. Attachment during a run returns STATE. Scope guards free
builder/output allocations if a Lua allocation fails while returning results.

Ordinary, typed, upload, static, WebDAV and SSE route declarations accept an
optional `openapi` table (nil/false skips it). Groups preserve the composed path
and methods. As in Vectis, this convenience registers the route before attaching
its annotation: an annotation failure leaves that route registered. Treat such
declaration failures as setup failures before run. Individual native route and
annotation operations each retain their own failure guarantees. Explicit
`openapi_doc` remains available independently of route registration.
The [installed Lua fixture](../tests/lua/rest.lua) covers schema retention and
release, aliases, copied metadata, generation, conflicts and allocation failure.
