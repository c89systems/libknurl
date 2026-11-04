# Vectis integration boundary

The [extraction contract](server-api-spec.md) is the sole normative product spec.
Baseline: Vectis `6b1e263e0436687f68a7768ca55f4361a6e59e19`.

Librotmos receives the existing enhanced Kore server, router/replies/body/source
execution, proxy, WebDAV/filesystem, static serving, codecs, REST/OpenAPI,
WebSocket/SSE, policy/logging and server lifecycle mechanisms. Vectis removes
those implementations when it switches. It keeps app services/business policy,
outer supervision, credentials, metrics aggregation, packed lookup, durable-store
clients, DSV/MCP integrations and outbound clients.

The adapter translates config/types/errors, forwards callbacks/handles, registers
host providers and connects readiness/control. Application declarations and
handlers remain unchanged. HTTP/codec/queue logic in that adapter is a missing
library operation, not permission to rebuild the feature in Vectis.

The baseline sources below define detailed accepted inputs/defaults/lifetimes;
they are read-only evidence, never build inputs:

- [Public app/server/request/reply/config declarations](../../vectis/include/vectis/vectis.h)
- [Server helpers, body/source/JSON/XML route execution](../../vectis/src/vectis.c)
- [Kore integration and callback/response ordering](../../vectis/src/vectis_kore_bridge.c)
- [Proxy API](../../vectis/include/vectis/proxy.h), [HTTP adapter](../../vectis/src/vectis_kore_proxy.c), [WebSocket adapter](../../vectis/src/vectis_kore_proxy_ws.c)
- [WebDAV implementation](../../vectis/src/vectis_webdav.c)
- [XML implementation](../../vectis/src/vectis_xml.c), [SAX](../../vectis/src/vectis_xml_sax.c), [Lua DOM](../../vectis/src/vectis_lua_xml_objects.c)
- [Lua bindings](../../vectis/src/vectis_cli.c), [REST helpers](../../vectis/lua/vectis/rest.lua)
- [Lua server contract](../../vectis/manual/lua-app.md), [XML contract](../../vectis/manual/lua-xml.md)
- [ACME host persistence](../../vectis/src/vectis_acme_state.c)
- [Runtime regressions](../../vectis/tests/unit/test_vectis_runtime.c), [Lua lifetime regressions](../../vectis/tests/lua/app_source_lifetime.lua), [interop regressions](../../vectis/tests/lua/upgrade_seams.lua)

Readiness requires a concrete installed reference adapter here and the affected
Vectis regressions during the separately authorized consumer cutover. A broad
library API alone does not establish a simple replacement.
