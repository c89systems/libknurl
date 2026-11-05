# Extraction verification

Development verification completed on 2026-11-05 for the extraction contract,
using Vectis `6b1e263e0436687f68a7768ca55f4361a6e59e19` as the behavioral baseline.
The sibling checkout still has that exact HEAD and was not modified.
The [operation audit](server-api-audit.md) maps every contract family to its
implementation, reference forwarding adapter and observable cases.

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
