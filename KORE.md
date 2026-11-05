# Kore defect record

Record every defect found in upstream Kore, the imported Vectis patch stack,
and librotmos's Kore patches/adapter here, including defects fixed before a
commit. Add the reproducer, cause, fix and evidence when finding a new bug;
update its status as verification progresses. Keep feature additions separate.

The pristine snapshot is `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b` in
[`vendor/kore`](vendor/kore). Patches 0001–0042 were imported from the Vectis
revision recorded in [`patches/kore/vectis.revision`](patches/kore/vectis.revision).
[`series`](patches/kore/series) is the authoritative application order. Patches
apply to disposable build trees; fixes never modify the pristine snapshot.

“Upstream” below means the defective code is present in that pinned snapshot,
not a claim about current upstream releases. “Patched stack” identifies behavior
introduced by, or specific to, our integration. The fixture column identifies
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
| K02 | `Host...` header names could match the first four bytes of `Host` and be treated as the authority header. | [0032](patches/kore/0032-kore-match-host-header-name-exactly.patch) requires the exact `host:` prefix. | The baseline [prebody fixture](../vectis/tests/unit/test_kore_prebody_handoff.c) sends `Hostile:`. That isolated case still needs porting into librotmos's accumulated compatibility gate. |
| K03 | Embedded NUL, malformed CR boundaries and excess header fields could be hidden by C-string/split-array parsing. | [0033](patches/kore/0033-kore-validate-request-header-block.patch) validates the complete header block before splitting. | Baseline `check_embedded_header_nul`, `check_bare_header_cr` and `check_header_count_boundary` in the [prebody fixture](../vectis/tests/unit/test_kore_prebody_handoff.c); isolated librotmos parity cases remain to be ported. |
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

## Portability and features are not automatically bugs

Strict C89 conversion, platform integer/queue declarations and independent SDK
prefixes are build requirements handled by patches 0042/0043/0046/0048. Direct
TLS, optional DH configuration, seccomp selection, WebDAV verbs, admission hooks,
loop callbacks, typed library entry points and ACME host callbacks also include
intentional product integration. Their presence in the patch series alone is
not evidence of an upstream defect. Any defect discovered in those changes gets
its own entry above with an explicit cause and regression.

## Verification and outstanding evidence

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

No remote upstream fix, report, release or CVE status has been inferred. When
upgrading Kore, compare each recorded cause against the new pristine source,
retain needed patches/regressions, and record why a patch can be removed.
