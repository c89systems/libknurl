# libknurl documentation

Start with the [quick starts](../README.md) or find a guide below. The
[product contract](server-api-spec.md) defines the public behavior.

| Use | Reference |
| --- | --- |
| Features, supported platforms and application boundary | [Overview](overview.md) |
| clangd comments, completion and LuaLS annotations | [Editor setup](editors.md) |
| Public C API and ownership/units/error hovers | [Installed headers](https://github.com/c89systems/libknurl/tree/trunk/include/knurl/) |
| Lua facade, callbacks, shapes and lifetimes | [Lua guide](lua.md), [annotated facade](https://github.com/c89systems/libknurl/blob/trunk/lua/knurl.lua), [editor types](https://github.com/c89systems/libknurl/blob/trunk/lua/knurl.meta.lua) |
| Host-owned Lua VM and provider interop | [Embedding header](https://github.com/c89systems/libknurl/blob/trunk/lua/include/knurl/lua.h), [Lua source packages](../lua/README.md) |
| Core SDK archives and relocated consumers | [SDK delivery](sdk.md) |
| Lifecycle, signals, control and fork ownership | [Processes](processes.md) |
| Routing, request metadata and body policies | [Requests](requests.md) |
| Exact input validation and HTML escaping | [Input helpers](input.md) |
| File-backed MMDB country readers and comparison | [MMDB](mmdb.md) |
| Route IP/DNS/country ACLs and proxy trust | [ACLs](acl.md) |
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

Contributor instructions and verification history are in the source repository.
[Development](https://github.com/c89systems/libknurl/blob/trunk/docs/development.md)
