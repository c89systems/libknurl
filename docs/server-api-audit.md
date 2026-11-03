# Server API specification audit

Audited 2026-11-03 against the [server API specification](server-api-spec.md) at
librotmos `750c489`, Vectis `6b1e263e0436687f68a7768ca55f4361a6e59e19`, the
pristine Kore pin and its complete 42-patch composition. This report records the
documentation repair; no server, binding or consumer implementation is implied.
The revised spec is authoritative where an earlier description differs.

## Resolved findings

| Gap and concrete failure | Resolution in the revised spec |
| --- | --- |
| A producer returning AGAIN had no public resume path and could stall forever or be polled on every writable event. | Owner-loop producer resume, coalescing, strict data/EOF results and exactly-once producer close; section 9. |
| Body completion could be mistaken for request completion while a reply, producer or channel still borrowed its data. | Explicit request terminal conditions and teardown order, separate input/response outcomes and finite unanswered-reply deadline; sections 6–7. |
| COMMITTED and FINISHING competed for one state slot, losing information needed for safe 200-to-502 replacement. | Separate drain state and irreversible commitment latch, including TLS retry without a positive write count; section 9. |
| Trailer retries, known-length overflow and informational statuses left room for duplicate output or false success after failure. | Atomic end acceptance, finite trailer bounds, bodyless representation-length exceptions, strict byte counts, restricted final/interim paths and failure cleanup; section 9. |
| Takeover could duplicate coalesced pipeline tail bytes or continue reading beyond a declared body boundary. | One all-or-nothing tail transfer, disabled post-completion input and preserved earlier response ownership; section 10. |
| Cancellation said to wait for retirement without specifying an actual notification; same-batch cancellation could free live userdata. | Explicit retirement callbacks for watch/timer/defer/waker, repeat cancellation and shutdown ordering; section 11. |
| Wake coalescing and full deferred queues lacked progress/ownership rules. | No lost signal across a callback, explicit queue-full failure and no ownership transfer on rejection; section 11. |
| Shutdown could join a producer which was waiting for the shutdown loop itself. | Nonblocking cancellation hook, continued completion delivery, join only finished threads and bounded escalation; section 5. |
| Startup and a handler which never starts a reply had no finite completion bound; timeout precision was left as an implementation choice. | Explicit startup/reply budgets, ceiling-to-second Kore timer policy and observable effective values; sections 5–6. |
| A 64 KiB header byte limit could be mistaken for freedom from the parser's independent field limit. | Record the current 23-field limit including Host, with boundary tests; section 6. |
| Wide counters alone did not establish large-file support or truthful delivery observations. | Require actual target file/seek range checks and 32-bit large-offset tests; distinguish body, plaintext HTTP framing, queued and transmitted counts; sections 9 and 14. |
| Router attachment ownership was ambiguous and broad rejection of overlaps would break valid Vectis routes. | Single-owner attachment/take semantics and the actual exact-conflict predicate, while preserving general overlap ordering; section 19. |
| Application WebSocket backpressure had no specified receive resume or writable path. | Atomic frame acceptance, writable notifications, held-frame resume and state cleanup before/after successful open; section 13. |
| WebDAV promised generic providers without specifying asynchronous completion, cancellation, lock wait or the mutation commit boundary. | Public operation completion/retirement, finite waits, declared capabilities and explicit post-commit cancellation outcomes; section 19. |
| Importing filesystem code unchanged would carry blocking flock into the HTTP loop. | Nonblocking lock attempts or bounded post-fork native work, with public completion and no foreign-thread Lua; section 19. |
| STREAM prohibited a complete parser result even though a typed JSON route must materialize a record; XML budget claims were underspecified. | Separate transport streaming from explicit typed/DOM materialization, finite codec limits and no false XPath/RSS or resumability claim; sections 8 and 19. |
| The audit treated mutable LoneJSON record interop as unresolved instead of tracing Vectis's existing writer input policy. | Preserve private records assigned from value tables, pinned owners, buffered external-record support and callback-writer rejection of external mutable records. No new copy/lease API is an extraction prerequisite; section 19. |
| ACME trust documentation named CURL_CA_BUNDLE, which the imported patch does not use. | Correct to SSL_CERT_FILE/SSL_CERT_DIR with explicit source precedence; section 12. |
| ACME restore/validation and atomic certificate/key activation were insufficiently separated. | No hydration during validate, cooperative parent budget, auxiliary persistence deadline, retained acknowledged generation and explicit ambiguous external-write outcomes; section 12. |
| “Standalone Lua product” could imply a new interpreter or exclude an embedded host. | Host-owned Lua VM, public preload helper, packaged loaders and an installed embedded-host test; section 20. |
| Lua ownership/error language omitted wide integers and allocation failure while acquiring native resources. | Exact count values, protected callbacks, unwind-safe initialization, rooted owners and allocation-failure tests; section 20. |
| Lua shared module and fully static packed Vectis could accidentally load two native runtimes. | One shared closure for the shipped module; preserve the packed Vectis facade over its C adapter, with direct static-module composition explicitly unclaimed; section 20. |

## Evidence behind the corrections

- [CA environment patch](../patches/kore/0020-kore-vectis-curl-ca-environment.patch)
  uses SSL_CERT_FILE and SSL_CERT_DIR for ordinary curl and ACME helper paths.
- [Header validation patch](../patches/kore/0033-kore-validate-request-header-block.patch),
  the pristine [HTTP header](../vendor/kore/include/kore/http.h), and Vectis's
  [prebody regression](../../vectis/tests/unit/test_kore_prebody_handoff.c)
  establish the line-count bound independently of byte capacity.
- [Vectis route registration](../../vectis/src/vectis.c), specifically
  `vectis_route_conflicts`, rejects intersecting method masks only for equal path
  kind and path text. It does not attempt general regex overlap rejection.
- [Vectis WebDAV](../../vectis/src/vectis_webdav.c), specifically
  `vectis_webdav_lock`, retries blocking flock; its descriptor-relative direct
  filesystem operations also show why lexical path checks are insufficient.
- Public LoneJSON declarations were checked at **v0.45.0**, rather than inferred
  from a potentially modified sibling worktree. Schema/record views and pull
  generators exist. Vectis's `vectis_lua_writer_record` already allocates private
  records and uses public schema assignment, while rejecting external record
  userdata for callback output. The [Lua XML contract](../../vectis/manual/lua-xml.md)
  and [upgrade regression](../../vectis/tests/lua/upgrade_seams.lua) document and
  exercise original-input mutation, GC/reentry, spooled-field isolation and the
  buffered-versus-callback input distinction. This is an existing integration to
  preserve, not missing LoneJSON functionality to add for this extraction.
- cpkt's **v0.6.1** CMake declarations and libpslog's **v0.12.0** public header
  supply the recorded dependency baselines and native logging capability.
  These source inspections are not an installed-package compatibility test.
- The selected lifecycle's Lua contract requires installed public shared-library
  use, owner pinning and separate Lua source/rockspec/source-rock delivery. It
  does not require a new interpreter. The project additionally requires embedded
  host support with one VM and one native runtime.

## Remaining implementation obligations

There is no unresolved choice about the main architecture: librotmos owns the
server toolkit/API, Kore is private behind external build-time patches, Vectis
owns its application policy/supervision, and Lua reuses the same public C API.
The following are extraction implementation/verification tasks. Existing Vectis
behavior is the baseline to carry over, not an unresolved dependency capability:

1. Port the selected Kore backend to C89 and replace fatal parent error paths;
   prove lifecycle, transport, cancellation and real streaming through the
   observable acceptance matrix. Vendoring tests cannot establish any of these.
2. Carry Vectis's existing private-record assignment, retention and cleanup into
   the rotmos response-producer lifetime. Port its writer isolation/GC/reentry
   cases and input acceptance/rejection tests; no new LoneJSON copy/lease API or
   support for arbitrary borrowed mutable records is required.
3. Implement and prove resumable XML output and asynchronous storage operations;
   the existing synchronous XML sink and blocking filesystem lock do not satisfy
   those contracts simply by being wrapped.
4. Test the shipped Lua module in executable and embedded hosts with the same
   shared core/provider closure. Direct upstream module use inside a fully static
   packed Vectis is outside that composition; preserve its current application
   facade during cutover. Supporting that additional composition needs an
   explicit single-runtime design, not a duplicate static library in the binding.
5. Recheck published c89systems releases when locking implementation SDKs, select
   the newest compatible closure and pin identities/digests. Baselines recorded
   here are not a promise that those releases remain latest.

Verification for this documentation checks local links, code-fence balance, all
42 patch dispositions and all 34 Vectis server configuration mappings, followed
by readback of state, ownership and cancellation paths. The repository's four
CTest cases exercise source preparation, replay, invalidation and upgrades only.
Vectis runtime tests were inspected, not rerun; the API acceptance matrix remains
future work. No sibling repository is changed by this audit.
