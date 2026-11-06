# Extraction verification

The final repository sweep on 2026-11-06 verified implementation `463521b`,
using Vectis `6b1e263e0436687f68a7768ca55f4361a6e59e19` as the behavioral baseline.
The sibling checkout still has that exact HEAD and was not modified.
The [operation audit](vectis-integration-contract.md) maps every contract family to its
implementation, reference forwarding adapter and observable cases.

## Server implementation gate

| Gate | Observed result |
| --- | --- |
| GNU Core | All 97 registered checks passed, including full relocated static/shared reference consumers, SDK checks and Kore preparation/replay/invalidation/upgrade. |
| Musl Core | All 92 selected checks passed, including full relocated static/shared consumers. The five `kore.*` host-tool checks reuse the GNU run on this host. |
| Lua | The unchanged implementation reuses its complete 36 GNU / 35 musl checks from the integer-conversion gate, including source-package/source-rock evidence. |
| Seven targets | The server gate built selected-toolchain Core, C89/C++98 headers and Lua bindings; all seven SDK archives passed final-byte and relocated CMake/pkg-config static/shared consumers. Runtime execution covers x86_64 GNU/musl; the other five targets have compile/link and artifact evidence. |
| Final review | No new actionable defect was found in the contract, C/Lua conversion, record/source ownership and failure-path pass. Formatting and diff checks passed. No executable changes were needed. |

The complete Core suites ran once after the accumulated corrections; selected
builds retain `final-sweep-build.log`, `final-sweep-tests.log` and the GNU
`final-sweep-format.log`. This is development verification, not actual Vectis
normal/packed cutover or release verification. Historical isolated fault-coverage
limits in [KORE.md](../KORE.md) remain explicit.

## Documentation and editor gate

The documentation pass preserves all parsed public function types, record field
order/types and enum values. Compiler AST inspection finds attached comments on
all 361 public functions, 238 typedefs, 625 fields and 181 enum values. Opaque
handle comments attach to their public typedefs. Source text comparison also
confirms that the Lua facade changed only in comments/annotations.

All seven selected toolchains pass C89 and C++98 public-header checks. Clang's
C89 documentation diagnostics are clean. Normal clangd hovers show the request
lookup error contract, terminal worker callback lifetime and storage acknowledgement
rules. LuaLS reports no warning/error diagnostics, and ordinary `require("rotmos")`
inference resolves server creation, typed routes, request readers and XML node
mutation hovers without caller annotations or private implementation fields.

Four selected GNU Lua checks pass: installed preload, dynamic loading, reproducible
source-package verification and LuaRocks source-consumer reconstruction. The
installed fixtures compare exact editor metadata bytes in the CMake install and
LuaRocks tree; both paths are shipped by the source manifest. Local Markdown links,
formatting and diff checks pass. Evidence is under `build/documentation/`.
The full server runtime suites above were not repeated for this documentation-only
change; no server behavior, dependency pin or ABI changed.

## Reproducing and interpreting evidence

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
