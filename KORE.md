# Kore defect record

Record every defect found in upstream Kore, the imported Vectis patch stack,
and librotmos's Kore patches/adapter here, including defects fixed before a
commit. Add the reproducer, cause, fix and evidence when finding a new bug;
update its status as verification progresses. Keep feature additions separate.

The pristine snapshot is `c5fe59f521bdc1f2c486b5d8cc90227d72fcc628` in
[`vendor/kore`](vendor/kore). Patches 0001–0042 were imported from the Vectis
revision recorded in [`patches/kore/vectis.revision`](patches/kore/vectis.revision).
[`series`](patches/kore/series) is the authoritative application order. Patches
apply to disposable build trees; fixes never modify the pristine snapshot.

"Upstream" attributes the defect to Kore; each entry records whether the pin
imports an upstream correction or retains an external fix. "Patched stack" means
behavior introduced by, or specific to, our integration. The fixture column identifies
coverage sources; it does not certify every historical fix on every platform.
An entry with no isolated regression explicitly says so.

## K01: shared worker PID race — upstream, fixed by patch 0062

**Symptom:** intermittent failure before readiness, false child-exit reports,
parent execution entering the worker branch, or a worker surviving its parent.
This was observed in musl configuration and host-forwarding cases. The libc is
not the cause; scheduling exposes the race on either runtime.

**Cause:** `kore_worker_spawn()` assigns `fork()` directly to `kw->pid`, which
lives in shared memory. The child's zero return can overwrite the parent's PID
publication. Branching on that shared field can select the wrong fork branch.
In the parent startup loop, `waitpid(..., WNOHANG)` can return zero while the
shared PID is temporarily zero, satisfying `pid == kw->pid` for a live child.
The uninitialized wait status can then be reported as an exit. The same
assignment is present in the pristine `src/worker.c`.

**Fix:** [`0062-parent-owned-worker-pid.patch`](patches/kore/0062-parent-owned-worker-pid.patch)
keeps the fork return in a process-local `pid`, branches on that value, and
publishes the child PID from the parent only.

**Evidence:** syscall traces showed the parent reporting/exiting while its child
was still initializing and later serving an event loop. The regression in
[`auxiliary-runtime.c`](tests/auxiliary-runtime.c) coordinates the fork-complete
hooks so the child reaches its former zero-publication window first; it also
rejects terminal worker cleanup in the original parent. The uncorrected stack
failed this case; patched musl HTTP/key-manager/ACME lifecycle, restart,
termination and host-forwarding cases pass. The remaining target matrix is part
of the enclosing implementation batch. The final matrix results are recorded below.

## Other upstream and inherited defects

| ID | Symptom and cause | Fix in this stack | Coverage or limitation |
| --- | --- | --- | --- |
| K02 | `Host...` header names could match the first four bytes of `Host` and be treated as the authority header. | [0032](patches/kore/0032-kore-match-host-header-name-exactly.patch) requires the exact `host:` prefix. | [tls-runtime.c](tests/tls-runtime.c) sends the baseline prebody fixture's `Hostile: attacker.invalid` before `Host: localhost` and verifies successful dispatch on cleartext and TLS. |
| K03 | Embedded NUL, malformed CR boundaries and excess header fields could be hidden by C-string/split-array parsing. | [0033](patches/kore/0033-kore-validate-request-header-block.patch) validates the complete header block before splitting. | [tls-runtime.c](tests/tls-runtime.c) ports the baseline prebody fixture's embedded NUL, bare CR and field-count boundary inputs. Cleartext and TLS accept the last allowed field count, then reject an extra hidden Transfer-Encoding field with 400. NUL input remains length-delimited. |
| K04 | Read-half-close was treated as immediate failure despite readable bytes, dropping the final input chunk. | [0031](patches/kore/0031-kore-drain-readable-bytes-before-read-half-close.patch) drains readable Linux/BSD events first. | Body and proxy wire cases; BSD execution still requires a runnable BSD/Darwin host. |
| K05 | A pooled connection could inherit event flags from its previous use, causing false readiness/error/closure. | [0056](patches/kore/0056-reset-connection-event-flags.patch) clears flags when allocating the connection. | [proxy-runtime.c](tests/proxy-runtime.c), [proxy-websocket.c](tests/proxy-websocket.c); the patch's initialization is also checked during source replay. |
| K06 | TLS setup retained references returned by `X509_get_pubkey()` and its private-key wrapper. Repeated setup leaked them. | [0034](patches/kore/0034-kore-release-tls-domain-key-references.patch) keeps separate references and releases each after transfer to `SSL_CTX`. | [tls-runtime.c](tests/tls-runtime.c); no isolated allocation-count regression is claimed. |
| K07 | Shutdown could leave the worker accept lock held; parent-side unlock also dereferenced the absent parent `worker` and failed to notify replacements. | [0040](patches/kore/0040-kore-release-worker-accept-lock-on-exit.patch) releases intake and wakes eligible workers. | [process-runtime.c](tests/process-runtime.c), including restart and termination. |
| K08 | Reinitializing inherited static net/connection pools lost their allocated names; cleanup left stale name pointers. | [0041](patches/kore/0041-kore-static-pool-name-ownership.patch) gives those static owners explicit name cleanup/reset. | Restart/runtime fixtures; no isolated heap-accounting test is claimed. |
| K09 | Clean-tree Make builds lacked the explicit rule for the generated version object. | [0004](patches/kore/0004-kore-makefile-generate-version-object-from-clean-tree.patch). | Patch/source preparation and generated-version compilation; the standalone Kore CLI is outside this product. |
| K10 | Curl varargs used the wrong integer width, and bundled toolchains exposed incompatible curl option/callback types. | [0003](patches/kore/0003-kore-curl-bundled-toolchain-type-fixes.patch) uses the required types. | Strict selected-toolchain compilation and curl/proxy/ACME fixtures. |
| K11 | Public HTTP declarations depended on prior includes; AArch64 seccomp diagnostics used the wrong register declaration. | [0008](patches/kore/0008-kore-lifecycle-portability-fixes.patch) supplies declarations and the AArch64 register type. | Selected-target compilation; seccomp execution is not part of the embedded backend configuration. |
| K12 | A new signal could overwrite an already pending termination signal and lose shutdown intent. | [0019](patches/kore/0019-kore-vectis-preserve-termination-signals.patch) preserves termination priority. | Process/shutdown fixtures; no isolated signal-interleaving test is claimed. |
| K13 | File-reference MIME inference could overwrite a caller's explicit Content-Type. | [0025](patches/kore/0025-kore-vectis-preserve-fileref-content-type.patch). | Explicit MIME forwarding in [vectis-services-runtime.c](tests/vectis-services-runtime.c); no isolated native `http_response_fileref` regression is claimed. |
| K14 | HTTP pipeline surplus was discarded or mistaken for excess bytes of the current body. | [0029](patches/kore/0029-kore-preserve-pipelined-request-bytes.patch); later managed-path corrections are P04/P05 below. | [body-runtime.c](tests/body-runtime.c) and request/body forwarding wire cases. |
| K15 | IPC headers transmitted uninitialized structure padding. | [0038](patches/kore/0038-kore-zero-ipc-header-padding.patch) initializes the complete header before assigning fields. | Source-level initialization evidence and IPC runtime use; no isolated padding-byte regression is claimed. |
| K16 | Embedded restart/teardown retained HTTP cookie/runlock pools, MIME tables, access-log state, message registrations/cache and parent worker resources. | [0018](patches/kore/0018-kore-vectis-embedded-restart-cleanup.patch), retained by the library lifecycle adaptation. | [kore-runtime.c](tests/kore-runtime.c) and lifecycle/restart cases. This is an embedding defect; standalone process exit can hide it. |
| K17 | Access-log formatting failed to subtract its record header from remaining capacity; an absent User-Agent was also passed to string formatting. | [0057](patches/kore/0057-bounded-access-and-worker-logs.patch) accounts for the header and supplies the missing-field value. | [log-backend.c](tests/log-backend.c), [policy-logs-runtime.c](tests/policy-logs-runtime.c). |
| K18 | Parent access-log drain could spin forever on a lock left held by a killed worker. | [0057](patches/kore/0057-bounded-access-and-worker-logs.patch) uses nonblocking lock acquisition and avoids an unnecessary empty forced buffer. | [log-backend.c](tests/log-backend.c) deliberately leaves an unfinished locked record. |

## Upstream comparison: 2026-11-06

The pin `c5fe59f521bdc1f2c486b5d8cc90227d72fcc628` imports three upstream commits
after the previous `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b` snapshot:

| ID | Cause and upstream correction | Scope and evidence |
| --- | --- | --- |
| K19 | New HTTP requests were inserted at the head of the global request queue ahead of older pending requests. [c5a1e77](https://git.kore.io/kore/commit/c5a1e77535e3073f7e530d2dab2074078162e4f8), dated 2026-10-03, inserts them at the tail. | [kore-request-order.c](tests/kore-request-order.c) failed on the previous pin and passes on the new pin on GNU/musl. It checks FIFO creation, a pending older handler returning RETRY, sleep/wakeup tail order and terminal cleanup through the actual parser/scheduler. The other five targets compile/link the fixture. |
| K20 | PostgreSQL connection loss could strand asynchronous work or recycle a broken connection. [315c508](https://git.kore.io/kore/commit/315c5085b79c067358960f4b88bc5136999edfe3), dated 2026-11-01, repairs notification, cleanup and connection reuse. | Kore PostgreSQL is excluded from our selected backend. No librotmos runtime defect or PostgreSQL runtime verification is claimed. Our C89 queue-declaration patch addresses a separate concern. |

The 2026-11-04 commit adds `kore_task_channel_pending`, a feature in Kore's task
module, which is also excluded from our selected backend. It supersedes no patch.
All 62 ordered patches replay unchanged onto the new pin. None is superseded,
wholly or partially, by these three commits. The updater preserves pristine
upstream sources and applies patches only to disposable build trees.
[Verification](docs/verification.md) distinguishes replay, selected-target builds
and executable runtime evidence.

## ACME defects and recovery

These affect the imported/patched ACME implementation. Some paths were replaced
while extracting the private library backend, so they are recorded as stack
findings rather than attributing every subcase to untouched upstream code.

| ID | Symptom and cause | Fix | Coverage |
| --- | --- | --- | --- |
| A01 | A persisted account key whose account no longer existed at the CA could not recover. | [0021](patches/kore/0021-kore-vectis-acme-account-recovery.patch) added re-registration using the existing key; [0055](patches/kore/0055-acme-protocol-recovery.patch) recognizes the actual `accountDoesNotExist` problem instead of treating every HTTP 400 as missing account. | Account-error/local-CA cases in [acme-protocol.c](tests/acme-protocol.c). |
| A02 | Restarted ACME/key-manager processes could miss account-key/configuration delivery and strand account/order recovery. | [0055](patches/kore/0055-acme-protocol-recovery.patch) adds account request/re-delivery and resets obsolete order state. | Protocol restart and runtime renewal cases. |
| A03 | Transient directory/account/nonce/signing failures left no scheduled retry, or repeated requests raced an unfinished account setup. | [0055](patches/kore/0055-acme-protocol-recovery.patch) retains pending account intent and reschedules work after transient failures. | Directory/transport/account-error scenarios in the protocol fixture. |
| A04 | Overlapping order timers and signing operations could outlive their order, reschedule stale work or lose key ownership before installation/failure. | [0055](patches/kore/0055-acme-protocol-recovery.patch) associates work with its order, cancels it on cleanup and retains keys through terminal handling. | Issuance/recovery/renewal cases; no claim that all possible allocation failures are covered. |
| A05 | CA challenge notification could precede HTTP workers installing the challenge certificate. | [0055](patches/kore/0055-acme-protocol-recovery.patch) waits for worker installation acknowledgement. | Local-CA challenge/issuance checks. |
| A06 | The original library ACME patch added startup-key persistence and a separate candidate-install path, changing Vectis's runtime-file and durable-store ordering. | [0061](patches/kore/0061-direct-acme-host-persistence.patch) removes the extra startup PERSIST/candidate-install hook and retains Kore's working files plus host persistence callback. | [acme-runtime.c](tests/acme-runtime.c), [tests/lua/acme.lua](tests/lua/acme.lua), and the TLS/provider forwarding fixtures. This was our integration error, not an upstream requirement. |

## Patch and adapter defects

| ID | Symptom and cause | Fix | Coverage |
| --- | --- | --- | --- |
| P01 | The pslog worker bridge passed a length-delimited, non-NUL-terminated message as a C string, permitting an over-read. | [0028](patches/kore/0028-kore-bound-worker-pslog-messages.patch) validates frame length and creates a terminated view; [0057](patches/kore/0057-bounded-access-and-worker-logs.patch) bounds worker log frames to 8192 bytes. | [log-backend.c](tests/log-backend.c) sends long and malformed frames. |
| P02 | Response-write timeout arithmetic could subtract a later send-start timestamp from an earlier sweep timestamp and expire immediately. | [0035](patches/kore/0035-kore-guard-response-write-time-against-future-start.patch) checks timestamp ordering first. | HTTP guardrail/runtime coverage; no isolated clock-order regression is claimed. |
| P03 | Partial-header connections had no `http_request` to keep the loop awake, so the added connection timeout sweep could sleep past its deadline. | [0036](patches/kore/0036-kore-wake-for-connection-timeout-sweep.patch) constrains the wait by the next sweep. | [policy-runtime.c](tests/policy-runtime.c) sends incomplete headers and expects 408 without another request keeping the loop active. |
| P04 | Pipeline/prebody splitting could subtract an unchecked header length and reject the next request's bytes as current-body overflow. | [0037](patches/kore/0037-kore-guard-header-length-before-pipeline-split.patch) checks before subtraction and preserves surplus. | Body/pipeline wire cases. |
| P05 | Managed body delivery needed to retain surplus for the next request; restoration could exceed the next receive capacity or encounter another saved chunk. | [0050](patches/kore/0050-managed-body-surplus.patch) exposes private handoff; [0051](patches/kore/0051-bounded-pipeline-restoration.patch) restores bounded chunks and checks combined lengths. | [body-runtime.c](tests/body-runtime.c), [vectis-request-reply.c](tests/vectis-request-reply.c). |
| P06 | The initial async response integration could free a native request while output still drained, or retire managed requests after native header/request pools were destroyed. | [0049](patches/kore/0049-response-lifetime-and-progress.patch) retains managed requests and performs library cleanup before native pool cleanup. | [response-runtime.c](tests/response-runtime.c), [channel-runtime.c](tests/channel-runtime.c), cancellation/source ownership cases. |
| P07 | Deferred access logging could dereference a disconnected request's absent connection owner. | [0058](patches/kore/0058-retained-peer-access-logs.patch) accepts the retained peer snapshot and handles absent owners. | [policy-logs-runtime.c](tests/policy-logs-runtime.c) and transport/disconnect logging cases. |
| P08 | Managed response generation diverged from Kore's native headers, cookies, error pages and file conditional metadata. | [0059](patches/kore/0059-shared-response-head-codec.patch) and [0060](patches/kore/0060-shared-file-response-metadata.patch) share those private codecs with Core. | [vectis-response-head.c](tests/vectis-response-head.c), [vectis-body-file.c](tests/vectis-body-file.c), TLS/native-header cases. The gap was in our adapter, not Kore's native replies. |
| P09 | Early library lifecycle integration lacked complete ownership/acknowledgement for auxiliary children, replacement and cancellation; host processes must not reap unrelated children. | [0053](patches/kore/0053-owned-child-lifecycle.patch) keeps declared child ownership and lifecycle/deadline events. | [process-runtime.c](tests/process-runtime.c), [process-backend.c](tests/process-backend.c), [auxiliary-runtime.c](tests/auxiliary-runtime.c). |
| P10 | The terminal worker callback could not reclaim its host/server because Core still marked it running and accessed host state afterward; derived loggers also had to retire before host logger destruction. | [src/kore_runtime.c](src/kore_runtime.c) detaches the exiting fork's server after native/logger teardown and snapshots callback context. | [vectis-host-runtime.c](tests/vectis-host-runtime.c) destroys app/server inside the callback and returns both success and failure afterward. |
| P11 | An intermediate teardown change acknowledged a child as clean before its terminal callback returned, concealing abandonment if that callback hung. | The completion marker remains after callback return and touches only retained backend child state. Fixed before landing this implementation batch. | The host fixture blocks after app reclamation and verifies deadline escalation reports abandonment. |

## Vectis integration assessment corrections

These are extraction omissions or defects in librotmos's server integration,
not defects attributed to pristine Kore. They were identified by Vectis's
2026-11-05 assessment of `f6bc8cb` and the resulting baseline fault regressions.

| ID | Cause and correction | Evidence |
| --- | --- | --- |
| I01 | Standalone filesystem calls were stranded behind worker/loop-only storage operations. Public synchronous filesystem calls now share the disk implementation, cursor and mutation paths, require no server, and preserve read locking, configuration, conditions, quotas, ETags, tombstones and recovery. | `vectis-filesystem.c` ports Vectis standalone condition/copy/move assertions through the installed public API on a default-stack thread, before any server run. |
| I02 | C/Lua WebDAV declarations lost the file-quota-to-body-limit translation. A mount limit above the server hard ceiling also produced a plan error/500 for an otherwise admissible small body. Translation now forwards the quota; WebDAV admits against the lower hard ceiling. Zero inheritance and explicit Lua body overrides are retained. | Isolated default=2/mount=4/hard=8 or 3 cases in `vectis-services-runtime.c`; Lua route cases cover inheritance, override and hard ceilings. |
| I03 | Compatibility wrappers inherited new 30-second storage and deferred-response deadlines absent in Vectis. C translation and Lua defaults disable them; explicit library deadlines remain available. | Effective C/Lua settings; `storage-deadline.c` deterministically expires the actual operation timer before acknowledgement and checks disabled completion, commit and retirement. Existing cancellation/abandonment checks retain shutdown coverage. |
| I04 | Fixed 16-PUT and 256-operation/resource ceilings could reject valid uploads independently of configured server/storage capacity. Those guards are removed; native execution retains its bounded executor/queue and admission backpressure. | `webdav-runtime.c` holds 257 incremental PUTs at observable staging barriers, completes/cancels them and verifies content, cleanup and reuse under explicit server capacity. |
| I05 | Proxy cleanup discarded accounting needed for `vectis.proxy.exchange`; generic wire counters could not replace it. Terminal C/Lua snapshots now retain selected target, kind, accepted relay/body bytes, status, duration, request ID and error/disconnect classification. Upstream classification also reaches request observations. | HTTP/alternate/upload/pressure/disconnect/shutdown and WS/WSS fixtures assert exactly-once reporting and exact accepted byte counts; the reference service adapter and Lua facade forward the snapshot. |
| I06 | Heap allocation failure in path confinement returned a boolean false and became a path conflict. It now propagates NOMEM without mutation. | `filesystem-failure.c` observed CONFLICT before the fix and NOMEM afterward, with unchanged content and later reuse. |
| I07 | PUT renamed its staged file before closing it, so a close failure could be reported as a successful commit. It now hashes/syncs/closes before rename, clears the descriptor once and retains the old value on failure. | Vectis's atomic close/sync/descriptor-reuse assertions observed OK on injected close failure before the fix. The shared native/synchronous implementation now reports IO, preserves old bytes and leaves the reused descriptor open. |
| I08 | Physical write failures such as ENOSPC were classified as a configured quota limit, conflating disk failure with policy rejection. They now report IO, matching the baseline disk-write outcome; explicit quota checks still report LIMIT. | `filesystem-failure.c` injects ENOSPC during staged PUT, verifies IO, unchanged old content and subsequent successful replacement. |
| I09 | Successful WebSocket upstream EOF lost Vectis's upstream disconnect classification and reported none. The relay now retains the upstream side and distinguishes an upstream event error from ordinary EOF. | Adding the missing side assertion in `proxy-websocket.c` failed on both WS/WSS before the fix; the existing exact-byte and terminal-report assertions remain. |
| I10 | Filesystem listing used request lookup/open/hash for each child instead of Vectis's directory metadata. Unreadable files and external filenames could fail the entire listing; HTTP listing also read file contents unnecessarily. The shared cursor now uses no-follow stat metadata, retaining tombstone/internal-entry filtering and bounded iteration. | Managed/direct `vectis-filesystem.c` cases and live PROPFIND in `webdav-runtime.c` reproduced the failures; files with no read permission and literal punctuation now remain visible. |
| I11 | Asset/storage list-result validation treated literal URL punctuation as another request path and rejected legitimate packed/filesystem directory names. Listing validates one component; HTTP encodes it once. Request-path validation remains separate. | `asset.c` reproduced the literal-name failure and covers dot, slash/traversal and control-byte rejection. Mutable and read-only PROPFIND cases assert escaped percent/question/backslash names. |
| I12 | Content-directory calculation required unrelated cache settings for a direct root, ignored invalid managed quotas, and imposed an extra 3500-character/configuration dot-segment restriction. The pure calculation and provider construction now follow the baseline configuration rules and 4096-byte storage capacity. Trusted configuration segments retain no-follow traversal; request paths still reject traversal. | New standalone calculation cases and provider configuration with `/../cache/.` failed before the correction. Managed/direct filesystem operations exercise that configuration; long direct roots and managed overflow are checked without filesystem I/O. |
| I13 | The Lua static-asset binding inherited disabled filesystem-directory indexes, although Vectis's packed mount and the C reference adapter enable indexes. The binding now forwards that baseline default; explicit Lua disable and filesystem-directory defaults are retained. | `tests/lua/providers.lua` with a public provider index failed in preload/dynamic modes before the fix. Wire checks cover default server/group indexes, query-preserving redirect, HEAD, explicit disable aliases and unchanged filesystem-directory behavior. |
| I14 | Integer fields in the Lua binding required Lua's integer subtype instead of the checked conversion used by Vectis. Exact whole-valued floats and numeric strings failed for settings, counts, body limits and replies; callback error fields and zero-status aliases also diverged. The binding now reuses `lua_tointegerx` with existing sign/range checks. Method/mode numeric values and signed control descriptors use the same conversion; names retain string handling. SSE retains Vectis's explicitly strict retry type. | Six configuration/foundation/typed preload/dynamic cases failed before the correction. Cases cover float/text settings and limits, HTTP statuses and zero fallback, callback status/source/native-code preservation, fractions/negatives/NaN/infinity/overflow rejection, exact full-width counts and strict SSE retries. |

## Review stall corrections

These are Core extraction defects, not bugs in pristine Kore. The 2026-11-06
review supplied focused reproductions despite a passing accumulated Core suite.

| ID | Cause and correction | Evidence |
| --- | --- | --- |
| I15 | Normalization accepted HTTP output queues of 32 bytes or less, but both explicit writes and producer pulls reserved 32 bytes and waited for impossible capacity. Normalization now rejects these declarations with INVALID and an actionable 33-byte minimum. A shared private framing-reserve constant keeps validation and delivery aligned. Zero still inherits 256 KiB. | `declarations.c` rejects every positive size through 32 and accepts 33/zero inheritance. `response-runtime.c`'s `minimum` case delivers explicit writes, known-length sources and unknown-length chunked sources with one-byte chunks and a 33-byte queue. |
| I16 | Body-result readers opened paths synchronously before checking for regular files. A substituted FIFO could freeze the HTTP owner loop waiting for a writer. `body_reader.c` now opens with O_NONBLOCK before fstat, matching the existing file-source constructor. | `source-file.c` opens a FIFO with no writer and requires IO/NULL, with the existing five-second CTest timeout bounding a regression. Directory/missing/short paths, binary reads/reset, empty files and reader reuse after rejection remain covered. |
| I17 | Buffered REST dispatch requested only an in-memory byte view, so valid admitted bodies that spilled to disk failed before decoding. It now explicitly copies completed file-backed input through the public body API, checks `size_t` representability, and retains that copy through decoded-value release. The Lua binding now forwards the public decoder input instead of discarding it; the facade uses it for spilled bodies while preserving explicit body replacements. Transport storage and admission limits remain unchanged. | `rest-runtime.c` runs with both 64-byte and one-byte memory thresholds, covering valid/default/custom decoding, malformed spilled input, over-limit rejection, phases and borrowed bytes through source/custom-value cleanup. Lua REST wire cases cover valid and malformed spills and a before-hook replacement. |
| I18 | Managed NONE plans inherited the default body budget and compared it against the hard ceiling, rejecting bodyless requests at runtime when the hard limit was smaller. Admission now exempts NONE from that unused-budget comparison, matching registration and validation. Positive bodies remain forbidden, and payload-bearing plans still enforce the hard ceiling. | `server-runtime.c` configures a 1024-byte hard ceiling with the unchanged 10 MiB default; GET, explicit zero-length GET, HEAD, grouped routes and reruns succeed. A positive NONE body and an oversized buffered request return 413; registration rejects a buffered budget above the hard ceiling. The wire fixture also checks a configured Server header distinct from the instance name. |
| I19 | Parameter matching stopped at an exhausted path unless exactly one final optional segment remained. It now continues omission-first backtracking through all remaining optional segments, while still requiring mandatory parameters and literal suffixes. | `server-runtime.c` exercises zero, one and both optional values on the wire, verifies captured names/values and omission precedence, and rejects missing suffixes and extra path segments. |
| I20 | SSE encoding split only on LF, leaving lone CR bytes able to introduce unprefixed fields in EventSource input. Structured field encoding now splits CR, LF and CRLF and prefixes every resulting line, including trailing empty lines. Explicit preformatted frames retain their verbatim contract. | `foundation.c` checks data/comment framing for empty, mixed, repeated and trailing boundaries, field-injection text, exact capacities and unchanged output on LIMIT. Lua events cases cover the public facade, binary data, each delimiter and capacity failure in preload/dynamic modes. |

### Upload rate and mapped item storage

| ID | Cause and correction | Evidence |
| --- | --- | --- |
| I21 | The upload wait path scheduled only the body-idle deadline, while the minimum-rate check ran only on input progress. Disabling idle timeout allowed silent clients to wait indefinitely. The owner loop now waits for the earlier independent idle/rate deadline. Rate accounting retains grace and excludes application pauses; deadline arithmetic avoids 64-bit overflow. | `body-runtime.c`'s `rate` case disables idle timeout and checks silent/partial uploads return 408 after grace, paused delivery completes and the worker remains responsive. `unlimited` disables both protections and accepts a body after silence; the original case checks the idle deadline before the rate grace expires. |
| I22 | Selected-array iteration called init/cleanup on caller-owned reusable records, discarding fixed-capacity backing. Core now calls public LoneJSON reset before/after each item, including failures. LoneJSON 0.46.0 required supplied runtimes to disable default clearing; the 0.47.0 upgrade removes that restriction through public `lonejson_array_stream_set_record_reuse()`, without changing runtime configuration. No sibling repository changes were required. | `json.c` checks repeated and absent fields, capacity-one overflow, callback cancellation and malformed-input cleanup for owned runtimes and supplied runtimes with either clearing policy. Every case preserves backing pointer, capacity and flags and releases allocated fields. |

### Static MIME resolution

| ID | Cause and correction | Evidence |
| --- | --- | --- |
| I23 | Core's small extension list omitted media, playlists, modern images, fonts, archives and documents, so common files such as MP3/MP4 fell back to application/octet-stream. Core now has a broader catalogue and copied instance/route extension overrides, shared by filesystem and custom-provider serving. Explicit route types override mappings and provider metadata consistently. | `mime.c` checks format families, filename edges, normalization, isolation, configuration copying and invalid/duplicate mappings. `static-runtime.c` exercises MP3 ranges, MP4 HEAD, route/instance/explicit/provider/default precedence and copied declarations on the wire. Lua config/routes checks exercise the same public C API in preload/dynamic modes. |
| I24 | Content-Type properties previously checked only generic header characters, permitting malformed media types/parameters. A shared RFC 9110 syntax validator now protects extension mappings, static/404 declarations, DAV types, asset metadata and staged reply/header types. It validates tokens, quoted strings/escapes and legal whitespace, rejecting control/folding injection, malformed parameters and media-type lists before mutation. | `mime.c` exercises all 255 non-NUL byte values in six token/quoted contexts, legal quoted parameters/opaque bytes, malformed and escaped-control cases, and unchanged reply/header selection on failure. Lua tests reject embedded NUL and malformed configured types and verify quoted parameters survive ranged delivery. |

The separate Vectis reassessment's D1/D2 documentation findings are corrected in the C/Lua
proxy accounting comments and standalone filesystem quota contract: WebSocket
counts include relayed frame bytes but exclude the handshake, successful error
categories are `"none"`, and standalone quotas must be positive after explicit
configuration initialization. Their implemented baseline behavior is unchanged.

## WebDAV after-hook error replies

I27: WebDAV's uncommitted-error fallback passed NULL as the Content-Type to
`rotmos_response_bytes()`, which rejects that declaration. An asynchronous
after-hook failure therefore aborted the connection before headers instead of
delivering HTTP 500. The fallback now supplies `text/plain` with its empty body.
`webdav-runtime.c` reproduces the original EOF with a failing after hook, then
checks complete empty 500 replies for asynchronous GET/HEAD and synchronous
OPTIONS, subsequent normal delivery, and balanced provider-resource closure.
This is a Core adapter defect, not a pristine Kore defect.

## Parent-owned access-log files

Size-based retention is a feature addition. External patch
[0065](patches/kore/0065-parent-access-log-sink.patch) forwards complete worker
records from Kore's parent drain to one Core-owned file sink. Its external-sink
marker enables worker formatting without assigning a file descriptor to each
domain. The pristine vendor snapshot is unchanged.

| ID | Cause and correction | Evidence |
| --- | --- | --- |
| I25 | The adapter opened a parent access-log descriptor per listener/domain, but domain teardown did not close it; only worker copies were closed. Repeated embedded runs could leak parent descriptors. The new single parent-owned file is explicitly closed after the final drain; workers close their inherited copy before serving. | Source ownership trace through `kore_tls.c`, `kore_domain_free()` and worker-only `kore_domain_closelogs()`. `access-log-file.c` checks the actual writer descriptor is closed after a two-listener/two-worker run; `policy-logs-runtime.c` retains server reuse and startup-failure coverage. |
| I26 | The first rotation adapter draft reported startup ENOSPC through `kore_log()` before `kore_library_init()`. Its buffer allocation entered uninitialized Kore pools and terminated instead of serving. The same dependency would be unsafe after allocator cleanup. Diagnostics now call the already-live public root logger directly, before owned logger destruction. | Injected startup ENOSPC reproduced `fatal("cannot map pool storage")`; the debugger traced it through `kore_log()` and pool allocation. `access-log-file.c` now verifies startup and live-write ENOSPC diagnostics, continued HTTP, recovery and normal shutdown. |
| P12 | The initial external-sink patch draft retained native per-domain flushing after bypassing domain buffers. A non-forced drain could dereference NULL `dom->logbuf`. Source review caught this before build; patch 0065 skips that flush when the external sink handles the record. | `access-log-file.c` sends enough large complete records across two workers/listeners to exercise non-forced draining, and verifies whole-record file framing. Forced cleanup safely accepts absent native domain buffers. |

## SHA-1 Release diagnostics

K22: The first selected GCC Release build diagnosed possible 64-byte reads from
the one-byte/eight-byte inputs used by SHA-1 padding. The inherited update loop
used additive block-bound checks; the transform also cast a byte-aligned array
to a word-access union. External patch 0064 uses remaining-length subtraction
and an actual union workspace, eliminating the optimizer diagnostic and the
alignment/aliasing dependency without disabling warnings. `kore-runtime.c`
compares incremental digests against OpenSSL for lengths 0..129, chunk sizes
1/8/64 and unaligned inputs, covering both padding/block boundaries. No remote
upstream report/fix or observed production overread is claimed.

## Markdown media types

K21: Kore's built-in media registry omits `.md` and `.markdown`, so native file
responses fall back to `application/octet-stream`. The Core filesystem mapping
inherited the same omission. External patch 0063 registers both extensions as
`text/markdown; charset=utf-8`; `rotmos_static_content_type()` now agrees. The
charset follows the [required Markdown media-type parameter](https://www.rfc-editor.org/rfc/rfc7763.html#section-2)
and the existing UTF-8 convention for inferred text types. Pristine Kore remains
unchanged. Explicit filesystem overrides and packed-provider metadata/fallbacks
retain their existing precedence.

Evidence: `asset.c` checks both extensions, mixed case, exact-extension matching
and unknown types. `static-runtime.c` checks directory GET/HEAD, an extensionless
single-file route and an explicit override on the wire. `kore-runtime.c` checks
the native registry and its emitted response type across backend restart.

## Portability and features are not automatically bugs

Strict C89 conversion, platform integer/queue declarations and independent SDK
prefixes are build requirements handled by patches 0042/0043/0046/0048. Direct
TLS, optional DH configuration, seccomp selection, WebDAV verbs, admission hooks,
loop callbacks, typed library entry points and ACME host callbacks also include
intentional product integration. Their presence in the patch series alone is
not evidence of an upstream defect. Any defect discovered in those changes gets
its own entry above with an explicit cause and regression.

## Verification and outstanding evidence

The final accumulated repository gate passes all 97 GNU Core checks and all 92
selected musl Core checks, including full installed static/shared consumers.
The unchanged Lua implementation retains its complete 36 GNU / 35 musl gate and
seven-target build evidence. The final pass found no new defect; see
[verification](docs/verification.md) for exact scope and remaining consumer proof.

The assessment follow-up passes 17 affected C checks on each of GNU/musl,
36 GNU Lua checks and 35 musl Lua checks. All seven toolchains compile the
corrected Core, public headers, affected fixtures and installed-SDK Lua binding;
relocated static/shared reference consumers and final SDK artifacts pass on
all seven, with runtime execution on GNU/musl. See
[verification](docs/verification.md) for the current gate and historical scope.

The following paragraphs retain earlier defect-specific evidence.

The PID regression was observed failing before patch 0062 and passing afterward
on musl, including HTTP, key-manager and ACME children. Final selected-toolchain
builds, C89/C++ headers, Lua bindings and relocated static/shared codec/host
consumers pass on all seven targets. GNU and musl each pass 17 affected C checks
and all 30 Lua preload/dynamic cases. Patch replay preserves the pristine tree;
the GNU SDK final-byte packaging/relocation/negative checks pass. The other five
targets have compile/link evidence, not runtime execution. Historical fixtures
listed above are evidence locations, not substitutes for those checks.
Cases marked without an isolated regression
remain coverage limitations; the final compatibility/failure audit tracks them.

The accumulated wire gate now covers K02/K03 on cleartext and TLS, together with
proxy producer backpressure/resume/cancel/recovery and interrupted WebDAV
replacement/recovery. Those three fixtures pass on GNU and musl and compile/link
on all seven toolchains. The pressured upstream may observe FIN or reset after
downstream cancellation; the fixture accepts both terminal socket outcomes.
No new backend defect was found by these cases.

The completed accumulated development gate and seven-target SDK artifact
evidence are recorded in [verification](docs/verification.md). Historical
isolated-regression limitations above remain explicit; actual Vectis cutover
and signed clean release verification have separate gates.

No remote upstream fix, report, release or CVE status has been inferred. When
upgrading Kore, compare each recorded cause against the new pristine source,
retain needed patches/regressions, and record why a patch can be removed.
