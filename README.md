# librotmos

librotmos is a C89 HTTP server library for websites, REST services and APIs.
It combines a public C API and Lua facade with a [Kore](https://kore.io/) backend,
covering server configuration, routing, request bodies, replies, TLS and worker
lifecycle.

Applications supply handlers, schemas, authentication policy and storage
providers. librotmos owns HTTP transport and the reusable server mechanisms.

## Server features

- HTTP/1.0 and HTTP/1.1 listeners, worker processes, readiness controls,
  graceful shutdown and configurable resource limits.
- Literal, parameter and regex routes, method masks, groups, middleware and
  staged replies that remain editable until the handler chain completes.
- Bounded request and response streaming with backpressure, uploads, native
  readers and sources, plus explicit buffered and file-backed operations.
- Typed JSON routes through LoneJSON, libxml2 XML codecs and routes, buffered
  REST helpers, and OpenAPI JSON/YAML generation over the route registry.
- Static files and directories, asset-provider mounts, conditional requests
  and ranges, and WebDAV with filesystem or custom storage providers.
- Reverse proxying for HTTP, SSE and WebSocket traffic, including HTTP/2
  upstream connections, alongside application WebSocket callbacks.
- TLS termination, mutual TLS, ACME certificate management and HTTPS redirects.
- Trusted-proxy address resolution, admission and autoblock rules, structured
  access logging, and transport/lifecycle observations.

## C and Lua interfaces

The C API uses opaque handles, configuration values, explicit ownership and
returned errors. Route handlers construct staged replies; the server handles
framing, source execution and delivery. Public channels and loop operations
support direct I/O integrations, with an explicit [Kore](https://kore.io/)
backend escape hatch.
Public headers support C89 and C++ consumers; library and backend code use
strict `-std=c89`.

The `rotmos` Lua module provides an ergonomic facade over a C binding of the
public shared library. It runs in the host's Lua 5.5.1 state, including embedded
Lua, and shares the same routing, codecs, proxy, WebDAV and lifecycle APIs.
The host owns the Lua runtime. Lua packaging comprises facade/binding sources,
a LuaRocks rockspec and a source rock.

## Application integration

librotmos exposes a complete server facade for Vectis's HTTP/web/REST/API
workflows.
The integration preserves Vectis's handlers, route declarations, response/source
contracts and application C/Lua interfaces through forwarding wrappers and
generic callbacks. Application services, authentication providers, metrics
aggregation, packed assets, durable storage and outer supervision stay with
the application.

The [Vectis integration contract](docs/vectis-integration-contract.md) defines
facade coverage and compatibility requirements. The
[server API specification](docs/server-api-spec.md) describes configuration,
ownership, streaming and lifecycle behavior.

## Dependencies and platforms

| Dependency | Role |
| --- | --- |
| cpkt | OpenSSL, curl, nghttp2 and libxml2 packages and their native link dependencies |
| LoneJSON | Typed JSON records, parsing, serialization and generators |
| libpslog | Structured runtime and access logging |
| Lua 5.5.1 | Host runtime for the Lua module |

Dependency SDKs and Lua providers come from `github.com/c89systems/`.
C-only applications use the native library independently of the Lua module.
Platforms comprise GNU and musl Linux on x86_64, aarch64 and armhf, and
Darwin on arm64.

## Backend and documentation

[Kore](https://kore.io/) is a pinned, pristine source snapshot. Ordered patches
live outside its source tree and apply to disposable build copies, keeping
upstream upgrades and local changes separate.

- [Build availability, commands and backend maintenance](docs/development.md)
- [Upload callbacks](docs/uploads.md)
- [Source execution and response delivery](docs/responses.md)
- [Application WebSocket API](docs/websockets.md)
- [HTTP framing and proxy core](docs/proxy-core.md)
- [HTTP/SSE reverse proxy routes](docs/proxy-http.md)
- [WebSocket/WSS reverse proxy relays](docs/proxy-websocket.md)
- [LoneJSON codecs and typed routes](docs/json.md)
- [XML mapping, routes, DOM and SAX](docs/xml.md)
- [Assets and static serving](docs/assets.md)
- [Storage providers, transactions and native jobs](docs/storage.md)
- [WebDAV mounts and authorization](docs/webdav.md)
- [Server API specification](docs/server-api-spec.md)
- [Vectis integration contract](docs/vectis-integration-contract.md)
- [Ownership boundary](docs/extraction.md)
- [Specification audit](docs/server-api-audit.md)

## License

librotmos code is [MIT licensed](LICENSE). [Kore](https://kore.io/) retains its
upstream copyright and permission notices in [vendor/kore/LICENSE](vendor/kore/LICENSE).
