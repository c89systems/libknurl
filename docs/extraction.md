# Extraction ownership

The [small extraction contract](server-api-spec.md) is authoritative; the
[integration boundary](vectis-integration-contract.md) records baseline sources.

Move Vectis's enhanced Kore integration, reverse proxy, WebDAV and reusable
server helpers into librotmos behind its own C API and Lua facade/binding.
Vectis consumes the library and removes those implementations. Kore remains
its private backend, pristine in vendor/kore with external build-time patches.

Keep Vectis's app/service graph, outer supervisor, business/auth/proxy decisions,
credentials, metrics aggregation, packed lookup, durable-store clients, DSV/MCP
adapters and outbound clients in Vectis. Connect these through callbacks,
userdata and provider registrations without importing private Vectis types.

Preserve behavior through that boundary. A new public API is not permission to
change streaming, serialization timing, limits, storage layouts or application
requirements. Supporting runtime machinery stays private unless a real adapter
consumer requires it. The [audit](server-api-audit.md) records operation coverage
and the Vectis integration assessment corrections; [verification](verification.md)
distinguishes tested development scope from actual consumer cutover and release.
