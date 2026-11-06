# Extraction verification

The final repository sweep on 2026-11-06 verified implementation `463521b`,
using Vectis `6b1e263e0436687f68a7768ca55f4361a6e59e19` as the behavioral baseline.
The sibling checkout still has that exact HEAD and was not modified.
The [operation audit](server-api-audit.md) maps every contract family to its
implementation, reference forwarding adapter and observable cases.

## Final accumulated gate

| Gate | Observed result |
| --- | --- |
| GNU Core | All 97 registered checks passed, including full relocated static/shared reference consumers, SDK checks and Kore preparation/replay/invalidation/upgrade. |
| Musl Core | All 92 selected checks passed, including full relocated static/shared consumers. The five `kore.*` host-tool checks reuse the GNU run on this host. |
| Lua | The unchanged implementation reuses its complete 36 GNU / 35 musl checks from the integer-conversion gate, including source-package/source-rock evidence. |
| Seven targets | Selected-toolchain Core, C89/C++98 headers, latest Lua bindings and final SDK artifact evidence below remain valid for their unchanged inputs. Runtime execution covers x86_64 GNU/musl; the other five targets have compile/link and artifact evidence. |
| Final review | No new actionable defect was found in the contract, C/Lua conversion, record/source ownership and failure-path pass. Formatting and diff checks passed. No executable changes were needed. |

The complete Core suites ran once after the accumulated corrections; selected
builds retain `final-sweep-build.log`, `final-sweep-tests.log` and the GNU
`final-sweep-format.log`. This is development verification, not actual Vectis
normal/packed cutover or release verification. Historical isolated fault-coverage
limits in [KORE.md](../KORE.md) remain explicit.

The sections below retain the initial `f6bc8cb` evidence and subsequent
assessment/sweep corrections. G1–G5 and I01–I14 explain why the initial family-level
gate was insufficient; the final gate includes their added regressions.

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

## Configuration and C/Lua sweep

The sweep after `41c707c` traced configuration defaults, callback/source ownership,
error handling and C/Lua forwarding against Vectis. It found the Lua provider
static-mount index default missing from the binding; the C reference adapter
already enables it. I13 in [KORE.md](../KORE.md) records the failing preload and
dynamic regressions and the correction through the existing public C setting.

GNU passed six affected Lua checks: server/group provider mounts, filesystem
routes in both loading modes, source-package verification and source-rock
reconstruction. Musl passed the same four runtime cases and source reconstruction
(five checks); the target-independent package check reuses GNU. The wire cases
cover default indexes, query-preserving redirects, HEAD, explicit disable aliases,
filesystem-directory defaults and provider ownership/cleanup. All seven selected
toolchains built the updated binding and fixture hosts against installed public
SDKs with strict C89. Formatting and diff checks passed.

Core production code, public APIs, ABI layouts, dependencies and Kore patches did
not change. Their prior Core runtime, header and seven-target SDK artifact gates
remain applicable. Selected Lua builds retain `sweep2-*.log` evidence. No further
mismatch was found in the reviewed paths; actual Vectis cutover remains unverified.

## Integer conversion sweep

On 2026-11-06, the conversion/failure pass after `9f7689a` found Lua integer-subtype
checks rejecting Vectis's accepted exact floats and numeric strings. Six
configuration/foundation/typed preload and dynamic cases failed before the fix.
I14 in [KORE.md](../KORE.md) records the shared checked conversion and preserved
field bounds, error metadata and zero-status aliases. SSE retry keeps its explicit
integer/count requirement; its float/text rejection and full-width count cases
remain covered.

GNU passed all 36 Lua checks and musl all 35, including source-rock reconstruction;
the target-independent source-package check runs on GNU. All seven selected
toolchains built the binding, embedded facade and fixture hosts against installed
public SDKs in strict C89. The other five targets have compile/link evidence only.
Formatting and diff checks passed. Selected Lua builds retain
`conversion-before.log`, `conversion-after.log`, `conversion-build.log` and
`conversion-tests.log` as applicable.

Core source, public C APIs/ABI, dependency pins, pristine Kore and its patches are
unchanged; their previous gates are reused. This corrects an input-conversion
contract, without adding a server mechanism or a second parser. Vectis remains
read-only at the declared baseline, and actual consumer cutover is still a
separate verification gate.

## Initial development snapshot

Initial development verification at `f6bc8cb` completed on 2026-11-05. The
integration assessment subsequently identified G1–G5 gaps absent from that gate.

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
