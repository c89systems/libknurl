# OpenAPI documents

Include `<knurl/openapi.h>`. This surface lifts Vectis's route annotations and
LoneJSON-map projection into the router. It produces materialized JSON or YAML
OpenAPI 3.1.0 metadata without invoking handlers or requiring another application
route registry. The header forward-declares the optional LoneJSON map type;
include `<knurl/lonejson.h>` when constructing map-backed schemas.

Initialize `knurl_openapi_route_doc`, assign summary, operation_id and tags,
and use `knurl_openapi_request_json` / `knurl_openapi_response_json` with
`knurl_openapi_lonejson_schema(name, map)` to add schema declarations. Names
default to the map name, then Anonymous. A raw response declaration with no map
represents a response without JSON content. The builder borrows strings/maps and
owns its helper-allocated response vector; `knurl_openapi_route_doc_cleanup`
frees that vector only.

`knurl_router_openapi_doc(router, methods, path, doc, error)` copies selectors,
strings, schema names, tags and response declarations. Maps and child-map
metadata remain borrowed, immutable and alive through router destruction.
Builder strings and response storage
can be changed or released after successful attachment. Parameter paths use
Vectis's `:id` / `:id?` to `{id}` conversion. Disjoint method masks at the same
normalized path merge into one path item. Overlapping annotations and duplicate
response statuses fail with CONFLICT, preserving the previous annotations.
Attachment is inert and forbidden while the router is running. Route count still
counts actual route declarations; annotations can be supplied before their routes.

Initialize `knurl_openapi_document` and call `knurl_router_openapi`. NULL uses
the defaults: title `libknurl API`, version `0.0.0`, and no added output ceiling.
Set max_bytes for another finite bound. Release successful output with
`knurl_owned_bytes_cleanup`; failure returns an empty output. Generation is
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

The [public fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/openapi.c) covers copied metadata, normalization,
merged methods, optional response schemas, aliases, reference escaping, required
fields, JSON validation, YAML layout, conflicting metadata and output bounds.
Its 512-node recursive graph runs on a default-stack thread, including musl.
The [REST wire fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/rest-runtime.c) serves a document during a run
and verifies that annotation mutation is rejected there. Both are relocated
static/shared SDK consumers.
