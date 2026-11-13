# Extraction verification

## HTTP completeness (2026-11-13)

The [implementation specification](http-completeness-spec.md) is delivered:
shared C/Lua validators and HTML escaping with typed request receivers; bounded
multipart ranges and date conditions; negotiated gzip/Brotli sidecars and true
incremental gzip; shared proxy/managed chunk framing with separate trailers;
application WebSocket fragmentation, incremental UTF-8, validated close handling
and transactional fragment sends. The public guides and LuaLS declarations match
the new interfaces. Knurl has not been released, so ABI remains 0. Darwin's
install name and compatibility/current versions track ABI independently of the
development release identity.

The ABI 0 correction repeats the seven-target build/SDK gates and focused native
C/Lua loading checks. The functional suites below remain applicable because this
correction changes library identity and documentation only.

The focused cases prove malformed/duplicate/NUL/overflow input, allocation
failures without ownership loss, multipart offsets and complete lengths above
4 GiB, exact wire bytes and resets, encoding negotiation/freshness/validators,
HEAD and source replacement, and first gzip output before full source
consumption. Delivered gzip is independently inflated. The Brotli wire fixture
was generated and independently decoded through the public Brotli APIs; Brotli
is not a product/runtime test dependency. Chunked cases cover metadata splits,
extensions/trailers, prefix/AGAIN resume, pipelining, decoded limits, invalid
framing and metadata-only rate stalls. Native-reader and XML tests consume
decoded input; XML parsing acknowledges progress before the final chunk arrives.
WebSocket wire cases cover HTTP/HTTPS fragments, interleaved controls, split
UTF-8, exact protocol close codes, close deadlines and retry state invariance.

Completed serial gates:

| Gate | Evidence |
| --- | --- |
| GNU Release Core / Lua | 130 / 38 passing tests, including installed static/shared consumers and proxy HTTP/2. |
| Native musl Release Core / Lua | 115 / 37 passing tests. |
| GNU Debug Core / Lua | 132 / 38 passing tests, with constrained worker/thread stack and guard controls. |
| Valgrind | 44 passing C/Lua checks, including allocation failures, static compression, managed bodies, WebSockets and uploads. |
| Seven Release target Core/Lua builds | Both native Linux targets, four ARM Linux targets and arm64 Darwin. |
| Four ARM QEMU suites | 10 checks each, including wide multipart arithmetic, gzip, protocol validation and both Lua loading modes. |
| Seven SDK archives | Final checksums/inventories, public symbols, architecture/ABI, documentation, and four relocated CMake/pkg-config static/shared consumers per target. |
| Editors | Seven ordinary clangd checks report zero errors; live LSP hovers and receiver completions pass. LuaLS accepts ordinary require/facade usage and rejects an invented validator in the negative control. |

Darwin evidence is cross-build/package/consumer compilation, not native execution.
Final public-comment edits preserve preprocessed declarations. Formatting,
documentation relative links and diff checks pass. Raw logs and editor fixtures
are under `build/http-completeness/`; memory logs are also under
`build/release/checks/memory/`. The existing parser fixture was migrated and
syntax-checked without running a fuzz campaign. SOAP, listener HTTP/2/3, sibling
consumer edits, release publication and pushes remain excluded. Vectis consumer
cutover and release remain separately governed work.

## First-release readiness repair (2026-11-10)

The first 0.1.0 release attempt stopped during an added GNU Debug pass, before
the declared Release matrix or publication. The relocated shared SDK's JSON
consumer started a client inside `on_parent_ready` without opening serving.
That callback enters `READY_PAUSED`; automatic serving begins after return.
The first request could therefore race the callback and receive HTTP 503.
The earlier Release matrix and Debug consumer passes had exercised successful
scheduling orders without establishing this synchronization requirement.

A bounded pipe handshake now holds the JSON ready callback until its first
application response. Before the repair this reproduces the 503 deterministically;
opening serving before client startup makes both JSON modes pass. The same
early-client pattern is corrected in 23 other test fixtures. Production library
code, public interfaces, ABI, timeouts and optimization settings are unchanged.

Optimized GNU diagnosis also captured `ECONNRESET` after the complete expected
413 response when the server closed with unread request input. As already done
for XML, JSON accepts that terminal reset only after validating the status,
framing and body. Truncated replies, other errors and resets on successful
responses still fail.

All 124 GNU Debug Core checks pass. After the terminal-reset correction, the
two JSON checks and five installed static/shared consumer cases pass again.
All 34 Lua runtime checks pass against unchanged binaries; both source/package
checks pass after aligning Core/Lua candidate versions following the rewind.
The two focused JSON cases also pass in GNU Release and native musl Release.
Whole-tree formatting and diff checks pass. Other targets and the full release
matrix were not repeated during this focused repair. The failed release's
commit/tag and diagnostics remain local; nothing was pushed or published.

## Documentation structure and live editor comments (2026-11-10)

The project README is reduced from 108 to 27 lines. Feature coverage, testing,
release inventory and editor setup have dedicated guides linked from it and the
documentation index. Development guidance delegates those topics and references
the authoritative Kore revision/patch files. Current verification coverage is
separate from historical counts. Vectis source links now use the recorded public
baseline commit; all referenced paths exist in that revision.

Public comments now distinguish initialization from cleanup, known empty source
length from unknown length, response commitment from storage outcomes, route
policy/group prefixes, observed endpoint ports, XML defaults and the paused
parent-ready serving gate. Compiler AST comparison preserves all 362 public
function types, 239 typedefs, 634 fields and 181 enum values; every declaration
and member retains an attached parsed comment. C89/C++98 documentation diagnostics
are clean. No executable implementation, API layout or ABI changed.

Live clangd LSP verification passes 18 hover assertions across functions,
callback/configuration fields, an enum, an opaque typedef, storage acknowledgement,
the embedding header and real consumer call sites. Completion documentation also
contains the serving-gate contract. The server log confirms ordinary `.clangd`
discovery loaded both native Core and Lua databases, with no database or driver
overrides. Separate Core/header/embedding `--check` runs pass with zero errors
using `--tweaks=none`; the previously recorded code-action limitation remains.
LuaLS reports zero warning/error diagnostics for the unchanged Lua metadata.

All local Markdown file/heading links pass validation. Whole-tree `make format`
and `make format-check` pass. Normal GNU Debug Core/Lua builds pass, as do nine
focused Core cases (including SDK final bytes and relocated consumers) and four
Lua install/loading/package/source-consumer cases. The broader runtime matrix
was not repeated for this documentation-only change. Evidence, compiler snapshots
and raw LSP results are under `build/documentation-sweep/`.

## Release-matrix rehearsal and consumer fixtures (2026-11-10)

The complete `make prerelease JOBS=1` rehearsal passes in Release configuration
using development version 0.0.0. GNU passes 122 Core and 36 Lua checks; musl
passes 107 Core and 35 Lua checks. All four ARM Linux targets pass their five
QEMU checks, and all seven SDK archives pass final-byte and relocated-consumer
verification. Darwin proof remains compile/link/package only. The GNU Valgrind
gate passes all 25 cases. Six parser seeds replay successfully, followed by the
standard 10-second local AFL smoke: 12,023 executions and no saved crashes or
hangs. Formatting passes. This is preparation evidence, not a signed/tagged clean
release or publication.

The first rehearsal exposed a test-budget error: one 60-second setup fixture
combined SDK preparation with both optimized static/shared consumer builds.
Static compilation completed, but shared compilation was still running when
the aggregate deadline expired. SDK preparation, each variant's configure/build
and each execution sweep now have separate CTest cases joined by fixtures, preserving the
existing 60-second limits and Release optimization. A cold GNU Release check
passes all five tests; static/shared builds take 39.52/29.63 seconds.
All five Debug tests pass too. Selecting only the static execution case
schedules exactly SDK preparation, the static build and static execution.
Consumer objects and unchanged SDK timestamps remain reusable.

The rehearsal bundle is `build/release-matrix-rehearsal/artifacts/`: seven
`libknurl-0.0.0-<target>.tar.gz` SDKs plus the Lua source archive, rockspec and
source rock, with one `CHECKSUMS` manifest. All ten copied payloads match their
producer inventories and the combined manifest; the directory contains exactly
those ten files and the manifest. Logs retain the initial timeout, cold repair
verification and successful full rerun under `build/release-matrix-rehearsal/`.
No release refs or published assets changed.

## libknurl project identity (2026-11-10)

The repository and product use `libknurl`, with C headers under `<knurl/...>`,
`knurl_` symbols, `KNURL_` macros/options, `knurl::` CMake targets, the `knurl`
pkg-config package and `require("knurl")` in Lua. SDK archives, the LuaRocks
package, embedding interface, annotations, examples, documentation and local
Kore integration macros use the same naming. The pristine Kore snapshot and
dependency selections are unchanged. There are no prior published releases or
tags; the new library identity retains initial ABI major 0. Existing Git history
and preserved historical logs are unchanged.

Clean GNU and musl Debug Core/Lua builds pass all 122/36 and 105/35 native checks,
respectively, including installed C consumers, C++ header checks, embedding,
source packages and both Lua loading modes. The final public-boundary checks also
pass after removing temporary identity assertions from the tracked test. All 25 GNU Valgrind checks
pass with no reported memory errors or definite/indirect leaks. Five cross-target
Release builds pass; the four ARM Linux targets pass all 20 QEMU HTTP/traversal/Lua
checks. All seven selected Core SDK archives pass final-byte verification and
four relocated CMake/pkg-config consumer checks each. Darwin evidence is
compile/link/package only.

GNU Release also builds the benchmark and existing optional AFL++ harnesses;
five focused Core checks pass. No fuzz campaigns run for this rename. Temporary
export/spelling checks ran under ignored `build/libknurl-rename/` and were removed
before committing. Existing tests were migrated to the new public API;
no project-name regression tests were added. Logs and preserved pre-rename build
outputs are under the same ignored directory.
Formatting and whitespace checks pass. Ordinary `.clangd` discovery parses the
Core implementation, public umbrella header and Lua binding with zero errors
using `--tweaks=none`, retaining the previously documented code-action limitation.

## cpkt 0.7.0 SDK upgrade (2026-11-09)

All seven cpkt archive pins now select the published
[0.7.0 release](https://github.com/c89systems/cpkt/releases/tag/v0.7.0), with
the release assets' SHA-256 digests. Both build-time and exported Core CMake
metadata require that exact version. Current SDK/specification/Lua prerequisite
documentation agrees with the pins. LoneJSON and pslog selections are unchanged.

Core and Lua compile/link against the new SDKs on all seven targets: Debug on
native GNU/musl and Release on the five cross targets. All 122 GNU Core and 36
GNU Lua checks pass. Musl passes its 105 Core checks and all 35 Lua checks;
common host lifecycle/Kore checks are reused from the GNU run. The 25-check GNU
Valgrind gate reports no memory errors or definite/indirect leaks. Each of the
four ARM Linux targets passes five HTTP/traversal/Lua QEMU checks, including
ARMHF musl within the existing timeouts. All seven Core SDK binary rehearsals
verify their final archive bytes and four relocated CMake/pkg-config consumers.
Darwin proof is cross-build/link/package only, with no native execution claim.

The initial GNU suite exposed a pre-existing services-fixture startup race: its
client could run inside `on_parent_ready` before the default serving gate opened.
The fixture now explicitly opens serving before launching clients. A dedicated
pipe acknowledgment requires the first successful response before that callback
returns, deterministically reproducing 503 before the fix and passing afterward.
No library lifecycle behavior changed. The corrected full GNU suite and native
musl suite pass; the red/green logs are retained with the upgrade evidence.

This upgrade adopts cpkt's Lua native-stack guards and heap-backed CRC scratch
storage through its released binaries. Lua remains 5.5.1, and existing provider
ABI majors are unchanged. Core's own public API/ABI is unchanged. The separate
LoneJSON initialization and provisional binding handoffs remain open.
Readback verifies all seven pins against published asset hashes, cached archive
bytes, selected SDK identity and generated dependency records. Formatting and
whitespace checks pass. Ordinary `.clangd` discovery parses a Core implementation,
public header and Lua binding with zero errors using `--tweaks=none`, retaining
the already documented code-action limitation.
Logs and release metadata are under `build/cpkt-upgrade/`. No release, tag or
push is part of this upgrade.

## Constrained glibc Debug test stacks (2026-11-09)

GNU Linux Debug processes under test now select 128 KiB default pthread stacks
before `main`, including child consumer/example applications and both Lua hosts.
CTest disables the pthread stack cache at loader startup through
`GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0`; unrelated inherited tunables
are retained. Direct test execution without that setting exits with a diagnostic.
The bootstrap is linked only into test applications, with no SDK/library runtime
default changes, main-thread limit changes or build-tool worker changes.

All 122 GNU Debug Core checks and all 36 GNU Debug Lua checks pass. The policy
fixtures measure actual default/nested worker stacks after joining a larger
explicit worker, preserve an explicit 1 MiB stack and two-page guard, and prove
guards fault in subprocesses. An uninstrumented parent confirms its defaults and
main-thread limits survive launching the constrained child; a child missing the
cache setting fails as required. The XML and Kore JSON traversal regressions
also assert their actual GNU Debug worker stack is exactly 128 KiB.
The focused policy check also passes with an inherited nonzero cache setting
and an unrelated allocator tunable. Compilation databases contain the bootstrap
only for GNU Debug test executables, with none in GNU Release or musl Debug.

Owned C compilation now rejects VLAs and `alloca` under `-Werror`. A compiler
fixture checks the actual Core compile flags, accepts a small fixed array, and
requires both prohibited forms to fail for the corresponding warning. This
fixture and both traversal regressions also pass on native musl Debug without
the glibc bootstrap. Formatting and whitespace checks pass. Logs use
`build/debug-stack-policy-*`.

## Iterative XML mapping and Kore JSON traversal (2026-11-09)

Mapped XML now suspends reader state in an iterative frame stack, with bounded
inline storage for shallow input and geometric heap growth at greater depth.
Kore patch 0066 removes JSON serialization/destruction recursion without changing
the pristine snapshot. Serialization retains exact bytes and subtree boundaries;
destruction uses a single-pass intrusive worklist with no allocations.

The new default-worker regressions pass on x86_64 musl: 512-level XML mapping,
6,000-level mixed JSON serialization and 65,536-level destruction. They check
XML depth boundaries, incremental chunks, nested-array ownership, inherited
`xml:space`, source/malformed-input failures, new allocation failures and cleanup
followed by reuse. Musl cases assert a measured worker stack below 256 KiB.
Six existing XML/Kore checks pass too. Normal memory and ARM QEMU check
configurations now include the new regressions.

Deterministic cost assertions require no added shallow traversal allocations,
no destruction allocations, and geometric growth cost at depth. The separate
performance comparator verifies that 4% slowdowns pass and 6% slowdowns fail its
5% CPU-time budget. Three paired GNU Release benchmark runs against a saved
pre-change Core archive pass all eight workload assertions. Each run reports a
five-sample process-CPU median; comparison takes the median of the three paired
results. XML ratios are 0.9779, 1.0225 and 1.0044 for depths 0, 3 and 128.
JSON write ratios are 0.8002/0.8053 for widths 8/128 and 0.6964 at depth 128;
destruction ratios are 0.9570 wide and 0.8658 deep. These are focused traversal
measurements on the selected host, not whole-server throughput guarantees.

Both complete native Core suites pass: 119 GNU Release checks and 119 musl
Debug checks, including installed consumers and patch replay/invalidation.
The new correctness/cost checks also pass in the ordinary GNU Debug build.
Core and both traversal regressions compile/link on all seven selected targets;
each of the four ARM Linux targets passes Core HTTP and both stack regressions
under QEMU. Darwin evidence is compile/link only. The two new regressions and
existing mapped-XML test pass the targeted GNU Valgrind gate with no reported
memory errors or definite/indirect leaks. Both Lua XML modes pass on GNU and
musl against freshly installed Debug Core SDKs; the Lua runtime is unchanged.
Formatting and whitespace checks pass.

The ordinary editor database is refreshed by the GNU Debug build. clangd source
and public-header parsing/feature checks pass using normal `.clangd` discovery
with code-action self-tests disabled (`--tweaks=none`). The unrestricted
code-action self-test reports overlapping replacements in existing `isspace`
macro expansions and cross-file `DefineInline` replacements; it reports no
compiler diagnostics. Those code-action limitations are not corrected here.

Logs are `build/traversal-debug-tests.log`,
`build/traversal-performance/results/{baseline,candidate}.tsv` and
`build/traversal-performance.log`; full native, ARM, memory, Lua and editor logs
use `build/traversal-*`. No release/tag/push or full Lua/package/release matrix
was performed. The earlier
[assessment](musl-stack-assessment.md) remains historical. LoneJSON native
initialization and provisional Lua binding CRs are separate open handoffs.

## Scoped musl worker-stack assessment (2026-11-08)

The historical [assessment](musl-stack-assessment.md) records retained O0/O3 Core/Kore frame
measurements for three musl architectures and fresh focused default-worker-stack
observations. Six existing x86_64 musl XML/filesystem/WebDAV checks pass. The
diagnostic probes confirm unresolved XML mapping and LoneJSON initialization
failures on all three targets; Kore tree traversal differs on ARM32 at the tested
depths. Expected-failure diagnostic cases are not product correctness gates.
No product implementation changed, no complete product matrix was repeated,
and Lua runtime work remains with cpkt. See the assessment for ownership,
architecture limits, upstream CR drafts and the preserved local evidence.

## Response timeout clock ordering (2026-11-08)

The response sweep now samples time after body/channel timeout callbacks for each
delivery, preserving a newly queued 408 when scheduling crosses a millisecond
tick. The controlled-clock `response-timeout.c` regression failed before the fix
and passes afterward. It checks callback progress, exact write-idle/deferred
deadline boundaries, disabled timers and incomplete input without sleeps.

GNU and musl each pass that regression and seven affected body/response/channel
wire checks, including `body-rate`. The corrected Core and regression fixture
compile/link on all seven selected toolchains; formatting and whitespace checks
pass. Logs are `build/response-clock-{red,green}.log`,
`build/response-clock-{gnu,musl}-tests.log` and
`build/response-clock-*-build.log`. See I28 in [KORE.md](../KORE.md). The full
release, Lua, QEMU, memory and fuzz gates were not repeated for this focused fix.

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
rules. LuaLS reports no warning/error diagnostics, and ordinary `require("knurl")`
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

Use [Testing](testing.md) for current commands/profile selection and
[Release preparation](releasing.md) for artifact and release identity rules.
The following paragraphs describe earlier verification runs.

Those Core checks used `ctest --test-dir build/debug --output-on-failure
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
