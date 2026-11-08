# Extraction verification

## WebDAV fallback and LoneJSON 0.47.0 (2026-11-08)

The WebDAV after-hook regression reproduced EOF before response headers. The
fallback now uses a valid Content-Type; asynchronous GET/HEAD and synchronous
OPTIONS deliver complete empty 500 responses, subsequent requests succeed, and
provider resources close. The GNU fixture failed before the fix and passed
afterward; see `build/dav-fallback-{red,green}.log` and I27 in [KORE.md](../KORE.md).

Core and Lua now require checksum-pinned LoneJSON 0.47.0, including all seven
SDKs and the matching public Lua provider source. Its ABI 27 requires rebuilding
consumers. Selected-array iteration uses public per-stream record reuse instead
of changing runtime clearing configuration. The fixed-capacity tests cover owned
runtimes and supplied runtimes with either clearing policy, repeated/absent
fields, overflow, cancellation and malformed-input cleanup. This resolves the
0.46.0 limitation recorded under I22.

All seven selected toolchains build Core, fixtures and installed-SDK Lua bindings:
x86_64 GNU/musl Debug and the five cross-target Release builds. Eleven affected
Core checks pass on each native target; all 36 GNU / 35 musl Lua checks pass.
Each of the four ARM Linux targets passes its three basic Core/Lua QEMU checks.
Darwin has compile/link evidence through osxcross. Its compiler caught an
ambiguous concatenated JSON fixture string; explicit parentheses corrected that
warning without suppressing diagnostics. Only affected fixture targets were
rebuilt afterward. Four focused Valgrind cases (JSON, WebDAV and both Lua typed
modes) and the relocated static/shared C SDK consumers pass. Formatting and
whitespace checks pass.

Evidence is in `build/lonejson047-*-build.log`,
`build/lonejson047-{gnu,musl}-focused.log`,
`build/lonejson047-{gnu,musl}-lua.log`, `build/lonejson047-*-qemu.log`,
`build/lonejson047-valgrind.log` and `build/lonejson047-installed.log`.
The signed release, final-artifact and fuzz gates were not rerun; no release,
tagging or push was performed.

## Access-log retention and disk-failure recovery (2026-11-08)

File logs now have one parent-owned writer with default 10 MiB thresholds and
three total files. C and Lua expose size, retention count and explicit rotation
disable. Eight affected GNU checks pass, including patch replay; the seven
target-runtime/API checks also pass on selected x86_64 musl. Both toolchains pass
four affected Lua configuration/logging checks. The two logging cases were then
rerun after adding explicit zero-inheritance assertions; no library rebuild was
needed for that fixture-only change.

The new file fixture covers rotation boundaries, oversized records, restart
append, permissions, truncate/disable options, interrupted/partial writes,
ENOSPC, failed rotation/reopen recovery and nonregular paths. Its two-worker,
two-listener HTTP case proves continued serving, diagnostics and recovery after
startup and runtime ENOSPC, plus parent descriptor cleanup. The same fixture
passes Valgrind with no errors or definite/indirect leaks. A basic ARM32 GNU
Release QEMU check passes with a 4 GiB configured threshold, checking large-file
normalization without a large workload. Darwin Release Core and the file fixture
compile/link through selected osxcross; a missing Darwin declaration feature
macro in the fixture was corrected before that final build. Formatting and
whitespace checks pass.

Logs are `build/access-log-gnu-core.log`, `build/access-log-musl-core.log`,
`build/access-log-{gnu,musl}-lua.log`,
`build/access-log-{gnu,musl}-lua-defaults.log`,
`build/access-log-valgrind.log`, `build/access-log-armhf-qemu.log` and
`build/access-log-darwin-final-build.log`. Startup diagnostic allocator misuse
reproduced during development is corrected and recorded with the other findings
in [KORE.md](../KORE.md#parent-owned-access-log-files). The full release, SDK,
seven-target and fuzz matrices were not repeated; no release artifacts or refs
were changed.

## MIME defaults, overrides and Content-Type validation (2026-11-08)

Core has 125 built-in extension mappings, copied instance/route overrides and
one public instance resolver, shared with the Lua facade. Nine affected Core
checks pass on both selected x86_64 GNU and musl Debug toolchains: MIME, assets,
static serving, replies, DAV, REST, direct responses, request/reply forwarding
and the public symbol boundary. The MIME fixture checks constructor defaults
from nonzero-filled storage, copied mapping ownership, normalized keys, invalid
declarations and all 255 non-NUL byte values in six Content-Type syntax contexts.
Wire tests cover ranged MP3/HEAD MP4 delivery, override precedence and quoted
parameters without changing payload delivery.

All 36 GNU Lua checks and the six affected musl config/routes/provider checks
pass, including installed preload/dynamic modules and the GNU source consumer.
The new MIME Valgrind check passes. Relocated static/shared SDK consumers pass
with the same public headers and Core implementations. Their prepared SDK was
reused after replacing an invalid Content-Type placeholder in a forwarding
fixture; only that fixture was rebuilt before resuming the consumer gates.
Repository formatting and whitespace checks pass.

Final evidence is in `build/mime-validation-final-build.log`,
`build/mime-validation-musl-final-build.log`, `build/mime-gnu-lua-final.log`,
`build/mime-musl-lua-final.log`, `build/mime-valgrind-final.log`,
`build/mime-installed-final.log` and `build/mime-installed-final-resume.log`.
Earlier byte-check results following a fixture compile failure are superseded;
the final gates stop on build failure and use rebuilt fixtures. No dependencies,
upstream Kore sources or release artifacts changed. The seven-target release,
QEMU and fuzz matrices were not repeated for this feature.

## Upload rate and selected-array corrections (2026-11-07)

The five affected Core checks (`json`, `json-runtime`, `body-runtime`,
`body-rate`, `body-unlimited`) pass on selected x86_64 GNU and musl Debug
toolchains, using incremental builds and strict C89. Rate cases cover silence,
partial input, grace, pause exclusion, independent idle enforcement and disabled
protections. Mapped-array cases cover caller-owned capacity limits, repeated
reuse, omitted fields and cleanup on parse/callback failures.

These original 0.46.0 array checks used NULL schema runtimes or supplied runtimes
configured with `clear_destination_by_default = 0`. A separate GNU probe using
supplied default clearing failed: LoneJSON 0.46.0 initialized its first stream
item and lost configured backing. Its public API had no per-stream override; that
dependency limitation was open at that point, with no sibling repository changes.
The 0.47.0 follow-up above resolves it through the released public stream API. See
[I21/I22](../KORE.md#upload-rate-and-mapped-item-storage). The full release matrix
and existing rehearsal artifacts were not rebuilt for these changes.
Repository formatting and whitespace checks pass.

## XML rejection fixture correction

The XML wire fixture now permits terminal ECONNRESET for its header-time auth
and body-limit rejections even when the client sent the complete request body.
It still requires the full expected HTTP status, framing and body first. Normal
responses and unrelated socket errors retain their strict checks. Added late-body
cases wait for rejection headers before writing, exercising unread input without
microsecond sleeps. The XML runtime check passes on GNU Debug/Release and musl
Debug; only the fixture executable was rebuilt. Formatting and diff checks pass.
Logs are `build/xml-reset-*.log`. Server behavior and release artifacts are unchanged.

## Artifact privacy follow-up

Core and Lua verification now share byte checks for known workstation paths,
instrumentation, complete private-key PEM blocks and long GitHub token forms.
The Lua verifier covers source files, standalone and nested rockspecs, and exact
nested-tar identity; installed source-consumer payload uses the same check.
Synthetic rejection fixtures recompute inner/outer checksums. Positive fixtures
permit public certificates, PEM labels, API names and redacted examples. Error
checks ensure matched token content is not printed.

Repackaging exposed GNU strip retaining host UID/GID and rewriting static-archive
member timestamps. Linux SDK stripping now enables deterministic archives; the
selected target archiver checks zero IDs and epoch timestamps. The SDK test
separates repacks in time and rejects a synthetic archive with a changed member
UID. All seven final SDK verifiers and relocated consumers pass. Existing Core
and Lua binaries were reused; no server matrix recompilation was needed.

The privacy fixtures, Lua package negatives, GNU SDK package gate, both native
LuaRocks source-consumer checks, formatting and diff checks pass. All seven built
Lua bindings and the rehearsal Lua products pass the expanded scan. The new ten
product inventory is `build/privacy/CHECKSUMS`; six Linux SDK hashes changed due
to normalized archive metadata. The earlier rehearsal inventory is preserved as
historical evidence. Logs are under `build/privacy/`. All credential fixtures
use synthetic, unusable data. Release refs remain unchanged.

## Release rehearsal

The incremental rehearsal builds all seven Release Core/Lua products at version
`0.0.0`, using product sources from `c18b2be`. All 110 GNU / 97 selected musl
Core checks and 36 GNU / 35 musl Lua checks pass. Each ARM Linux target passes
its three basic QEMU checks. All seven SDK archives pass final-byte inspection
and four relocated consumers, including target-correct Darwin inspection.
Twenty Valgrind cases, six fuzz seed replays, the ten-second AFL smoke campaign
and formatting pass. AFL reports zero saved crashes/hangs; its 34,581 execution
count is cumulative across resumed smoke campaigns.

The initial preparation command stopped after 94 passing GNU checks because
the Testing fixture inherited `CONFIG=release` from Make while inspecting its
Debug directory. The fixture now selects Debug explicitly and simulates that
parent override even under direct CTest execution. Its focused check and the
remaining 15 GNU checks pass; the completed builds and unaffected results were
reused. Only the fixture and this verification record changed.

Seven Core archives and three Lua products are checksum-verified in
`build/rehearsal-c18b2be-CHECKSUMS`. This is a rehearsal inventory under `build/`,
not a release upload set. Logs are `build/rehearsal-prerelease.log`,
`build/rehearsal-testing-fix.log`, `build/rehearsal-prerelease-resume.log` and
`build/rehearsal-checksum-verification.log`. The real release identity guard
correctly rejects the feature branch. No default-branch integration, full clean,
product signing, release tagging, push or publication occurred; the signed clean
release gate remains separate.

## Routing and SSE review corrections

The consecutive-optional-parameter and lone-CR SSE defects are corrected and
recorded as I19/I20 in [KORE.md](../KORE.md). Five affected Core checks pass on
each of GNU and musl: foundation/SSE framing, server routing/lifecycle, the
reference Vectis route adapter, channels and loop smoke. The GNU foundation case
passed separately after replacing an incorrect hand-counted regression input
length; the other four earlier passing results are reused against the same Core.
Both GNU Lua events checks pass in preload/dynamic modes, including binary data,
CR/LF/CRLF data/comment framing and capacity failures through the public facade.

Selected-toolchain GNU Core/Lua and the affected musl targets build successfully;
formatting and diff checks pass. Logs are `build/review-route-sse-*.log`. The full
runtime/matrix, memory and fuzz gates were not repeated for these focused fixes.

## Lifecycle alignment sweep

The follow-up sweep reproduced ownership validation accepting a path through a
symlink followed by `..`, and source packaging accepting an untracked declared
input. Ownership now examines raw ancestors before normalization; package inputs
must be tracked in owned Git checkouts and canonical paths without symlinks.
Valid owned paths with a trailing slash are also accepted.
Direct fuzz configuration now rejects a missing toolchain/target before compiler
discovery, matching Core/Lua; the existing pinned AFL configuration is reused.

Six `lifecycle.*` checks pass, covering those regressions, normal Make
configuration restoring required tests, full cleanup including hidden state,
borrowed-root rejection, existing version/release identity, and archive
acquisition against a local fixture origin. Verified local/renamed/global hits
make zero requests; corruption triggers refetch, and checksum, HTTP, truncated
download and corrupt-global failures publish no archive or temporary bytes.

The two affected GNU Lua checks pass: reproducible final/nested source packaging
and LuaRocks source reconstruction against installed public SDKs. Bash syntax,
formatting and diff checks pass. The normal incremental build reports no work;
unchanged server binaries and earlier matrix, memory, QEMU and fuzz evidence
are reused. Logs are `build/alignment-{configure,contracts,packages,build,format}.log`.
No release or actual Vectis cutover is claimed.

The second ownership pass reproduced staging cleanup deleting a Kore source
export, component SDK symlinks being reused, and a Lua fixture writing its install
prefix before rejecting it. Kore preparation now checks both inputs against
output/staging overlap and rejects borrowed patch inputs. SDK preparation and
Core discovery validate each component prefix; Lua fixtures validate their build
and install paths before installation. The shared path validator is a declared
preparation dependency, with invalidation and unchanged-build regressions.

All eight lifecycle and five Kore host checks pass, including real patch replay,
source/build invalidation and the offline upgrade workflow. Two installed Lua
checks pass through the repaired fixture runner (preload/dynamic). The replayed
Kore tree is byte-identical to the one used by the existing Debug binaries, so
no server rebuild or repeat matrix/memory/QEMU/fuzz campaign was needed. Bash
syntax, formatting and diff checks pass; logs are under `build/alignment2/`.

## Lifecycle hardening gate

The selected-toolchain Release gate now builds static/shared Core, C89/C++98
headers, fixtures and the public-SDK Lua binding for all seven targets. The first
complete run passed 103 GNU Core checks, 96 musl Core checks (reusing seven GNU host-tool checks),
36 GNU / 35 musl Lua checks, all seven final SDK archive/relocated-consumer gates,
and formatting. No native Darwin execution is claimed.

The four ARM Linux targets each pass exactly three QEMU smoke checks: basic
public values/HTTP lifecycle and preload/dynamic Lua JSON/declaration workflows.
The deep native suites, fuzzing and memory checking do not run through QEMU.

Twenty Valgrind cases pass on GNU: twelve Core cases and eight Lua cases. The
initial nineteen-case gate is reused; the added spilled REST/custom-decoder case
passes separately against the same binaries.
The AFL++ GCC-plugin build instruments owned parsers, retaining strict C89 and
enabled assertions. Six canonical byte seeds replay successfully; the initial
10-second smoke campaign executed 10,403 inputs without saved crashes or hangs.
After finalizing the harness buffer capacity, a 10-second resumed smoke and a
fresh 60-second campaign pass; the latter executes 73,921 inputs. Saved findings
are rejected before resume; clean resume is also checked. Counts in resumed
campaign statistics are cumulative.
These are bounded fuzz results, not exhaustive source-coverage claims.

The default GNU Debug pass covers its complete inventory and all 36 Lua checks.
It exposed the old combined installed-consumer deadline: rebuilding both variants
and running every case together could exceed 60 seconds. Preparation is now
incremental and separate from static/shared execution via native CTest fixtures,
with a per-program deadline and diagnostic. Its three cases pass on Debug and
both Release runtimes; Release preparation logs show no consumer recompilation.
The redundant JSON-only producer is removed without dropping its cases. The
gate's inventory was 104 GNU / 97 selected musl Core checks. Unchanged earlier
passes are reused; only the changed consumer groups and remaining Debug checks
are rerun, not the matrix.

Release compilation exposed Kore's SHA-1 diagnostic/alignment issue, corrected
in external patch 0064 and documented as K22 in [KORE.md](../KORE.md). Incremental
digests at block/padding boundaries match OpenSSL. Private JSON/response/header
helper outputs now initialize their empty failure state explicitly, removing
optimized-build warnings without suppressing diagnostics.

Git version fixtures cover untagged/candidate/lightweight identities, malformed,
annotated/symbolic/ambiguous tags, and extracted/enclosing-checkout ownership.
Release fixtures reject wrong branches, dirty trees, unsigned commits, candidate
overrides and altered artifacts, and exercise signed lightweight-tagged artifact
staging using an ephemeral SSH key entirely under owned build scratch. No active
release ref, workstation key, sibling checkout or remote state is changed.

Both upstream lifecycle resolver contracts pass. Ordinary clangd LSP discovery
publishes empty diagnostics for the changed JSON implementation and new fuzz
harness, using their Debug and instrumented Release databases respectively.

`build/lifecycle-release-gate.log` records the complete Release operation; the
selected target directories retain CTest results and SDK archives. Valgrind logs
are under `build/release/checks/memory/`, and AFL inputs/results are under
`build/release/fuzz/`. [Development](development.md) defines commands and scope.
Actual signed default-branch release and Vectis cutover remain separate actions.

## Earlier evidence

The earlier accumulated sweep on 2026-11-06 verified implementation `463521b`,
using Vectis `6b1e263e0436687f68a7768ca55f4361a6e59e19` as the behavioral baseline.
That sweep left the sibling checkout unchanged at the baseline.
The [operation audit](vectis-integration-contract.md) maps every contract family to its
implementation, reference forwarding adapter and observable cases.

## Kore snapshot upgrade

The upgrade after `7c379ac` imports pristine upstream
`c5fe59f521bdc1f2c486b5d8cc90227d72fcc628` with all 62 external patches unchanged.
The native parser/scheduler [ordering regression](../tests/kore-request-order.c)
failed on the previous pin and passes on GNU/musl with the new pin. It covers
older pending handlers, FIFO creation, sleeper wakeup order and cleanup.

| Gate | Observed result |
| --- | --- |
| GNU/musl Core | 29 affected checks pass on each runtime: native ordering, request/body/reply delivery, routes, TLS, static/uploads, XML/REST, DAV, WebSocket/SSE, proxy HTTP/2/WS/WSS and serving lifecycle. |
| Kore tools | All five preparation, replay, invalidation and updater checks pass on GNU; these host-tool results are reused for the other targets. |
| Lua | All 36 GNU / 35 musl checks pass against freshly installed shared Core SDKs, including source-rock reconstruction. |
| Seven targets | Static/shared Core, C89/C++98 public headers, fixture executables and installed-Core Lua bindings compile/link with the selected toolchains. |
| Seven SDK archives | Final bytes, dependency/ABI/target/export metadata and relocated CMake/pkg-config static/shared examples verify. Each archive's BACKEND metadata and shared-library bytes contain the new pin. GNU/musl examples execute; the other five targets supply compile/link and ELF/Mach-O evidence. |

The updater's export matches the pristine source tree byte for byte. Formatting
and diff checks pass; no patch or dependency pin changed beyond Kore. Evidence is
under `build/kore-upgrade-evidence/`, with SDK archives in each selected build's
`packages/`. This pass selects affected Core checks; it does not repeat the full
Core suites or full installed reference-adapter suites recorded below. Actual
Vectis normal/packed cutover and clean signed release verification remain
separate gates.

## Review follow-up

The 2026-11-06 review after `821765d` reproduced two stalls outside the earlier
suite's cases: an accepted output queue too small to make body progress, and a
body-result path replaced by a FIFO. [I15/I16](../KORE.md#review-stall-corrections)
record their corrections and regression locations.

Nine affected Core checks pass on each of x86_64 GNU/musl, including the new
33-byte output-queue wire case and no-writer FIFO rejection, existing body/file
ownership and response behavior, and the filesystem compatibility cases. All
seven selected toolchains compile/link static/shared Core, affected fixtures
and C89/C++98 public headers. The other five targets have compile/link evidence.

The GNU binding rebuilt against the updated installed public Core SDK; installed
preload/dynamic loading, reproducible source packaging and LuaRocks reconstruction
pass (four checks). Formatting and LuaLS diagnostics pass. Logs are under
`build/review-stalls/`. The earlier full Core suites and SDK archive rehearsals
below were not repeated for this focused repair; this is not release or Vectis
cutover verification.

Today's Vectis reassessment closes G1–G5 and recommends starting the consumer
migration. Its D1/D2 proxy accounting and standalone quota documentation findings
are corrected here; actual Vectis dependency alignment and normal/packed cutover
remain consumer work. No sibling repository changed.

## Earlier server implementation gate

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
