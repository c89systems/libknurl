# libknurl documentation

The [product contract](server-api-spec.md) defines scope and preserved behavior.
The [Vectis integration boundary and forwarding inventory](vectis-integration-contract.md)
identify the baseline and reference adapters. Feature guides explain the public
operations; they do not expand the contract. [Verification](verification.md)
records measured evidence and its limits; [KORE.md](../KORE.md) tracks backend,
patch and extraction defects with regression references.
The [musl worker-stack assessment](musl-stack-assessment.md) records historical
frame measurements and the findings behind the delivered traversal corrections.

| Use | Reference |
| --- | --- |
| Features, supported platforms and application boundary | [Overview](overview.md) |
| Build, selected toolchains and Kore upgrades | [Development](development.md) |
| Test profiles, Debug worker stacks and traversal performance | [Testing](testing.md) |
| clangd comments, completion and LuaLS annotations | [Editor setup](editors.md) |
| Version/signing boundary and complete artifact inventory | [Release preparation](releasing.md) |
| Public C API and ownership/units/error hovers | [Installed headers](../include/knurl/) |
| Lua facade, callbacks, shapes and lifetimes | [Lua guide](lua.md), [annotated facade](../lua/knurl.lua), [editor types](../lua/knurl.meta.lua) |
| Host-owned Lua VM and provider interop | [Embedding header](../lua/include/knurl/lua.h), [Lua source packages](../lua/README.md) |
| Core SDK archives and relocated consumers | [SDK delivery](sdk.md) |
| Lifecycle, signals, control and fork ownership | [Processes](processes.md) |
| Routing, request metadata and body policies | [Requests](requests.md) |
| Staged replies, live output and source ownership | [Responses](responses.md) |
| Live chunk/native-reader/file uploads | [Uploads](uploads.md) |
| Takeover input and opaque tunnel output | [Channels](channels.md) |
| Mapped JSON and selected-array operations | [JSON](json.md) |
| Mapped XML, owned DOM/XPath and incremental SAX | [XML](xml.md) |
| Buffered server REST and route annotations | [REST](rest.md), [OpenAPI](openapi.md) |
| Static files, directories and host asset providers | [Assets](assets.md) |
| Storage provider acknowledgement and standalone filesystem calls | [Storage](storage.md) |
| WebDAV mounts, quotas and authorization | [WebDAV](webdav.md) |
| Proxy declarations, callbacks and transport | [Proxy core](proxy-core.md), [HTTP/SSE/H2](proxy-http.md), [WS/WSS](proxy-websocket.md) |
| Application WebSockets and SSE | [WebSockets/SSE](websockets.md) |
| Identities, mTLS, SNI and redirects | [TLS](tls.md) |
| Host persistence, issuance and renewal | [ACME](acme.md) |
| Trusted proxies, admission, autoblock and observations | [Policy](policy.md) |
| Borrowed pslog loggers and independent access files | [Logging](logging.md) |

C declarations use Doxygen comments recognized by clangd. Lua uses LuaLS/EmmyLua
annotations, including dynamic methods implemented in C. Neither editor metadata
nor these guides expose private Kore or dependency layouts. Public indexes are
zero-based in C and one-based in Lua unless an operation explicitly documents an
exception, such as selected-array callback indexes.
