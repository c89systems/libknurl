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

The installed fixture [vectis-server.c](../tests/vectis-server.c) translates all
34 server fields from the test-only baseline declarations in
[vectis-server.h](../tests/vectis-server.h). It retains Vectis's application name,
server header and explicit body-limit inheritance; normalization remains in the
library. [vectis-config.c](../tests/vectis-config.c) checks defaults, overrides,
copied declarations, baseline timeout saturation and foreground/supervised worker
lifecycle. A supervised host attaches its control FD and signal policy after
translation. Successful construction transfers that FD; shutdown closes it.
This is configuration/lifecycle evidence, not a completed application adapter.

[vectis-route.c](../tests/vectis-route.c) adds the ordinary app/request/response
callback shape, status/error translation, NUL-terminated request strings and
host-source vtable forwarding. Request associations and route callback owners are
the only retained adapter state. Source reset/read/close, live delivery, spooling,
reply replacement and HTTP error selection stay in public library operations.
The [runtime case](../tests/vectis-route-runtime.c) checks duplicate query/headers,
captures, binary input, 255-byte diagnostics, source ownership and hook/reset/
suppression/failure timing. No Lockd SDK or sibling source is a build input.

[vectis-body-file.c](../tests/vectis-body-file.c) extends that adapter with the
borrowed body-reader/path and file-reply shapes. It checks JSON/buffered/legacy
upload spill behavior, defaults and registration failures, disabled spilling,
spool I/O failure and cleanup, then after-hook file creation/replacement/removal
and final 404/HEAD/bodyless handling. The adapter never collects input or opens
reply files; those operations remain in librotmos.

[vectis-response-head.c](../tests/vectis-response-head.c) uses the baseline server
translation to check configured Server, keep-alive timeout/budget, ordinary error
pages, HEAD/304, optional header limits and large-head queueing/replacement.
Kore's head codec is shared through an external patch; the adapter adds no encoder.
The TLS/backend cases separately cover HSTS and native cookies/header merging.
[vectis-request-reply.c](../tests/vectis-request-reply.c) extends the ordinary
adapter with fixed/owned memory materialization, independent spill files/readers,
read-all copying and binary responses. It also checks registration order/masks/
conflicts, file Last-Modified and exact conditional matching, empty files, HEAD
representation lengths, duplicate cookies/headers and staged transport fields.
Its source adapters only forward public read/reset/close; collection, files and
HTTP execution stay in the library.

[vectis-services.c](../tests/vectis-services.c) adds proxy preflight/rewrite/
response/error and edit operations; upload stream, native reader and file
helpers; WebSocket callbacks/send/close; WebDAV filesystem/provider and auth
translation; and resolved TLS identity/redirect/ACME configuration. It retains
callback owners and converts value/error/string shapes. Native-reader ownership
uses the public acquire/release pair to preserve the host request facade after
HTTP cancellation, without sharing live Core handles with native work.
The [combined case](../tests/vectis-services-runtime.c) exercises real proxy
requests, local/error replies, file output, implicit 204, reader progress before
EOF, cancellation metadata, acquisition failures, DAV PUT/GET/PROPFIND/auth,
WebSocket raw frames and SSE through the ordinary source adapter. TLS declarations
are checked as values here. The [TLS fixture](../tests/tls-runtime.c) also uses
that adapter for file/memory/borrowed material, cipher/version, live TLS/mTLS and
raw-URI redirects. Borrowed source reset/read goes through public sources without
closing the host owner. The [ACME protocol fixture](../tests/acme-protocol.c)
forwards host hydrate/persist callbacks through real issuance/renewal against a
local HTTPS CA. The directory is the configured Kore runtime root, exactly as
the host's existing state helpers expect. The generation store and extra startup
PERSIST are removed. Remaining service options and issuance-driven Lua PERSIST
still need their corresponding adapter evidence.
