# Extraction verification

Initial development verification at `f6bc8cb` completed on 2026-11-05,
using Vectis `6b1e263e0436687f68a7768ca55f4361a6e59e19` as the behavioral baseline.
The sibling checkout still has that exact HEAD and was not modified.
The [operation audit](server-api-audit.md) maps every contract family to its
implementation, reference forwarding adapter and observable cases.

Vectis's subsequent integration assessment found gaps G1–G5 that these earlier
family-level checks did not cover. The counts below are historical evidence for
that snapshot, not proof of the missing standalone APIs, mount admission/defaults,
concurrency or proxy diagnostics. The assessment follow-up adds explicit
regressions and corrections for those cases; its evidence is recorded in the
[operation audit](server-api-audit.md) and [KORE.md](../KORE.md).

## Assessment follow-up

G1–G5 corrections passed the following focused gate on 2026-11-05. This reuses
the initial gate for unaffected behavior; the entire historical suite was not
rerun for each correction.

| Gate | Observed result |
| --- | --- |
| GNU and musl Core | Each passed 15 affected checks plus HTTP/2 proxy and response runtime checks (17 per target). These cover standalone calls before any server, managed/direct conditions and recovery, allocation/write/sync/close faults, mount admission, deterministic storage expiry, cancellation/abandonment, 257 simultaneous PUTs, HTTP terminal accounting, worker shutdown and WS/WSS. |
| GNU Lua | All 36 checks passed: 34 preload/dynamic cases, source-package verification and source-rock reconstruction. |
| Musl Lua | All 35 checks passed: the same 34 runtime cases and source-rock reconstruction; the target-independent source-package check reuses GNU. |
| Seven toolchains | Static/shared Core, affected fixture executables, C89/C++98 public headers and Lua bindings against freshly installed shared Core SDKs built successfully. GNU/musl standalone cases execute on default-stack threads. |
| Installed reference consumers | Standalone filesystem, service forwarding, proxy HTTP/WebSocket and configuration adapters compiled against relocated public static/shared SDKs on all seven targets and executed on GNU/musl. No sibling source or private Core header is part of those consumers. |
| Seven SDK archives | Each rebuilt archive passed final-byte inventory/checksum, dependency/target/ABI/export and relocated CMake/pkg-config static/shared example checks. GNU/musl examples also executed; the other five targets have compile/link and artifact evidence. |

The added Lua terminal cases inject callback exceptions and allocation failure,
then verify transport cleanup and the final reported callback error. Standalone
filesystem mutations and asynchronous WebDAV share the corrected disk engine.
The extraction contract remains 145 lines. No dependency pins, pristine Kore
sources, external patch series or sibling repositories changed in this follow-up.

Selected-build `assessment-*.log` files and CTest `Testing/Temporary` results
retain the local evidence; SDK archives remain under each build's `packages/`.
This closes the assessment's demonstrated library gaps. Actual Vectis
normal/packed integration and release verification remain separate gates.

## Follow-up sweep

The sweep after `c9e9e11` rechecked the assessment, operation inventory and
reference forwarding paths against the same Vectis baseline. I09–I12 in
[KORE.md](../KORE.md) record four additional compatibility corrections and their
failing regressions: WebSocket upstream EOF classification, metadata-only
filesystem listing, literal names through mutable/read-only listing, and
content-directory/provider configuration. No public API, ABI layout, dependency
pin, Kore patch or contract family was added.

GNU and musl each passed 19 affected Core checks and 10 Lua preload/dynamic
checks. The Core selection extends the assessment follow-up with asset and
static-serving coverage; Lua checks cover routes, providers, proxy and terminal
callback exception/OOM handling. Standalone and asset cases run on default-stack
threads. Path/traversal rejection, conditional mutation, cancellation, recovery
and 257 simultaneous uploads remain in the affected checks.

All seven selected toolchains built static/shared Core, affected fixtures,
C89/C++98 headers and bindings against refreshed installed shared Core SDKs.
Relocated public static/shared standalone-filesystem, asset, filesystem-provider,
WebDAV and WebSocket consumers compiled on all seven targets and executed on
GNU/musl. Each rebuilt SDK archive passed final-byte validation and four relocated
CMake/pkg-config example consumers; only GNU/musl examples executed.

Selected-build `sweep-*.log` files retain this evidence, including the deliberate
before-fix failures. Earlier unaffected checks and the unchanged Lua source
product reuse their recorded gates. This is a focused compatibility sweep,
not an actual Vectis cutover or an exhaustive syscall fault-injection claim.

## Initial development snapshot

| Gate | Observed result |
| --- | --- |
| GNU Core | All 95 registered CTest checks passed, including full relocated static/shared reference consumers, SDK positive/negative checks and backend preparation/invalidation/upgrade. |
| Musl Core | All 90 selected checks passed, including full relocated static/shared consumers. The five `kore.*` host-tool checks reuse the GNU run on this same host. |
| GNU Lua | All 32 checks passed: 30 preload/dynamic cases, source-package verification and source-rock reconstruction against installed public SDKs. |
| Musl Lua | All 31 checks passed: the same 30 runtime cases and source-rock reconstruction. The target-independent source-package check reuses the GNU run. |
| Seven toolchains | Static/shared Core, C89/C++98 public headers, installed shared Lua binding and host modules built with the selected supported compilers. Other than x86_64 GNU/musl, this is compile/link evidence, not runtime execution. |
| Seven SDK archives | Final payloads passed inventory/checksum, license/provenance/dependency, target/ABI, path/export and relocated CMake/pkg-config static/shared consumer checks. Each target built four example consumers; GNU/musl also executed them. |
| Dependencies | The same released cpkt 0.6.1, LoneJSON 0.46.0 and pslog 0.13.0 closure is declared by the Core/Lua metadata. Lua uses the host's 5.5.1 VM. |
| Kore isolation | All 42 imported patches match Vectis byte for byte. Patch replay applies the external series without modifying the pristine snapshot; invalidation/upgrade checks passed. |
| Editor | Ordinary `.clangd` discovery loaded `build/debug` for Core and `build/debug/lua` for the binding. Core implementation check and normal LSP diagnostics for Core/Lua public headers and Lua implementation passed. |

Core checks used `ctest --test-dir build/debug --output-on-failure
--stop-on-failure --no-tests=error`; musl used its concrete target directory and
excluded `^kore\.`. Lua checks used the corresponding `lua/` build directories.
Build, test and package operations ran serially. Compile databases show strict `-std=c89` for C and
`-std=c++98` for the C++ header consumer.

Compatibility cases cover the corrected JSON/after-hook timing, live XML input,
caller-owned selected-array storage, one-pass REST replies and shared C/Lua
typed execution. The accumulated run also covers source/native-work cleanup,
worker/auxiliary restart and termination, host readiness/control/fork exit,
cleartext/TLS malformed headers, HTTP/1 proxy pressure/resume/cancel/recovery,
live TLS/ALPN HTTP/2 duplex forwarding, DAV conditions and interrupted overwrite
recovery. The reference wrappers contain translation/forwarding and providers;
server, codec and transport execution remain in Core.

Each Core archive and its `BINARY-CHECKSUMS` are under the selected build's
`packages/`. Lua facade/binding sources, rockspec, source rock and their checksum
inventory are under `build/lua-source-package/`. These are local rehearsals.

Clangd 23.1.2's auxiliary `--check` mode reports DefineOutline failures in the
standalone header and SwapBinaryOperands failures on Lua macros when trying
every refactoring at every token. Normal LSP diagnostics for those same files
are empty; this is not a compiler/header failure or a passed refactoring check.

[KORE.md](../KORE.md) retains explicit historical coverage limitations where a
defect lacks an isolated regression. They are not claims of exhaustive fault
injection. Actual Vectis normal/packed cutover regressions require separately
authorized consumer edits. Release readiness additionally requires the
lifecycle's default-branch/signing/tagging and clean release-build gates.
No consumer cutover, release, push or publication was performed.
