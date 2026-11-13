# Features and integration

libknurl is a C89 HTTP server library for websites, REST services and APIs.
It combines a public C API and Lua facade with a [Kore](https://kore.io/) backend,
covering server configuration, routing, request bodies, replies, TLS and worker
lifecycle.

Applications supply handlers, schemas, authentication policy and storage
providers. libknurl owns HTTP transport and the reusable server mechanisms.

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
  and ranges, and WebDAV with filesystem or custom storage providers, plus
  standalone filesystem operations without a server.
- Reverse proxying for HTTP, SSE and WebSocket traffic, including HTTP/2
  upstream connections and terminal exchange accounting, alongside application
  WebSocket callbacks.
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
The standalone [MMDB country readers](mmdb.md) provide free functions and receiver
methods without starting a server. The embedded libmaxminddb reference backend
keeps its upstream C99 dialect isolated; Knurl's own reader remains C89.

The `knurl` Lua module provides an ergonomic facade over a C binding of the
public shared library. It runs in the host's Lua 5.5.1 state, including embedded
Lua, and shares the same routing, codecs, proxy, WebDAV and lifecycle APIs.
The host owns the Lua runtime. Lua packaging comprises facade/binding sources,
a LuaRocks rockspec and a source rock.

## Application integration

libknurl exposes a complete server facade for Vectis's HTTP/web/REST/API
workflows.
The integration preserves Vectis's handlers, route declarations, response/source
contracts and application C/Lua interfaces through forwarding wrappers and
generic callbacks. Application services, authentication providers, metrics
aggregation, packed assets, durable storage and outer supervision stay with
the application.

The [Vectis integration contract](https://github.com/c89systems/libknurl/blob/trunk/docs/vectis-integration-contract.md) defines
facade coverage and compatibility requirements. The
[server API specification](server-api-spec.md) describes configuration,
ownership, streaming and lifecycle behavior.

## Dependencies and platforms

| Dependency | Role |
| --- | --- |
| cpkt | OpenSSL, curl, nghttp2 and libxml2 packages and their native link dependencies |
| LoneJSON | Typed JSON records, parsing, serialization and generators |
| libpslog | Structured runtime and access logging |
| libmaxminddb 1.14.1 (embedded) | Private reference MMDB reader; no consumer installation required |
| Lua 5.5.1 | Host runtime for the Lua module |

Dependency SDKs and Lua providers come from `github.com/c89systems/`.
C-only applications use the native library independently of the Lua module.
Platforms comprise GNU and musl Linux on x86_64, aarch64 and armhf, and
Darwin on arm64.


## Backend

Kore is a pinned, pristine source snapshot. Ordered patches apply to disposable
build copies. See [Development](https://github.com/c89systems/libknurl/blob/trunk/docs/development.md#upgrading-kore) for maintenance
and [KORE.md](https://github.com/c89systems/libknurl/blob/trunk/KORE.md) for defect history and regression references.
