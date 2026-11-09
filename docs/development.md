# Development

The [extraction contract](server-api-spec.md) defines the product. The
[integration inventory](vectis-integration-contract.md) maps operation coverage and
[verification](verification.md) records the completed development checks.
Actual Vectis cutover and release verification have separate gates.

## Commands

Prerequisites: CMake >= 3.24, Ninja, Git, Make, Bash >= 4.4, curl, tar, flock,
SHA-256 tools and clang-format. Host lifecycle checks also use Python 3 solely to
serve a small local archive fixture; Bash/CMake drive and assert those checks.
Linux compilation uses the lifecycle's pinned
Bootlin GCC/binutils/sysroot collection, with no host compiler fallback.

```sh
make help
make toolchain             # Explicit provisioning; unnecessary when already prepared.
make build                 # Compile the selected x86_64-linux-gnu target in build/debug.
make build-all             # Compile the full seven-target matrix, serially.
make lua                   # Build the separate Lua facade/binding against the installed shared core.
make lua-all               # Compile both products for all seven targets, serially.
make lua-package           # Stage Lua sources, rockspec, source rock and their checksums.
make sdk-package           # Stage the selected Core SDK binary rehearsal under build/.
make sdk-verify            # Inspect that archive and build its relocated consumers.
make build-all CONFIG=release
make test                  # Run checks for the selected toolchain target.
make memcheck              # Native GNU Core/Lua Valgrind gate, reusing ordinary binaries.
make qemu-smoke TARGET=armhf-linux-musl  # Core/Lua and stack checks; no fuzzing.
make fuzz-smoke            # Seed replay and a 10-second native AFL++ campaign.
make fuzz FUZZ_SECONDS=300 # Longer native campaign; same instrumented producer.
make verify-all CONFIG=release # One matrix build followed by all declared gates.
make prerelease            # Release preparation; no clean, signing, tags or publication.
make release-check         # Read-only default-branch/signature/exact-tag validation.
make install               # Install to owned build/install/<target>/debug by default.
make format-check          # Check project sources; excludes pristine/patched Kore.
make prepare-kore          # Source preparation only; no compiler or SDK acquisition.
make clean                 # Remove all local build/, dist/ and .cache/ state.
```

`make build` reuses checksum-verified local archives or acquires missing released
SDKs from c89systems. Once prepared, builds and tests are offline. SDKs have
independent prefixes under `.cache/sdk/<target>/`; archives stay in this
checkout's `.cache/`. Existing global cpkt archives may be read and copied.
Each component SDK prefix must also be owned; symlinking a component to another
checkout is rejected during preparation and Core configuration.
Toolchain discovery is read-only; only explicit provisioning prepares compilers.
One operation owns the checkout; run operations serially.

Normal Make configuration enables the required Core tests, including when reusing
a directory previously configured with `BUILD_TESTING=OFF`. Direct CMake builds
may still select a library-only configuration. The `lifecycle.*` checks cover
owned paths, tracked source inputs, version/release identity and archive-cache
misses, zero-request hits, corruption and failure cleanup. Acquisition fixtures
use a loopback origin and isolated caches under `build/`.

Every build selects a concrete architecture/OS and the lifecycle toolchain for
that target. The default is `x86_64-linux-gnu`, with `build/debug` and
`build/release` directories. Other targets use `build/<target>/debug` or
`build/<target>/release`. The editor database is
`build/debug/compile_commands.json`, matching `.clangd`; the ordinary build
prepares its actual dependencies and generated headers. There is no host-based
product selection.

The [Lua product](lua.md) uses its own `build/debug/lua/compile_commands.json`;
`.clangd` selects it for binding sources, embedding headers and Lua host fixtures.
Run `make lua` once to prepare those installed public SDK inputs before editing
the binding. Core C headers/installations remain independent of Lua.

Lua provider SDKs have one Release producer per target under
`build/<target>/release/lua-deps/` (default GNU: `build/release/lua-deps/`). Both
Core configurations reuse those dependency bytes. Switching Core configuration
does not replace shared SDKs with differently configured Lua providers.

`make lua-package` needs GNU tar, gzip, zip and touch. It stages only the Lua
facade/binding and their declared build inputs from `lua/source-manifest.txt`.
Checkout inputs must be Git-tracked files reached without symlinks; injected
VERSION is generated. Extracted source archives retain their declared inventory.
The three products and `lua-rotmos-<version>-CHECKSUMS` are written to `dist/`;
no release or upload occurs. [Package instructions](../lua/README.md) describe
the selected-target SDK build and LuaRocks variables. CTest's
`rotmos.lua-package` checks reproducible final/nested bytes and source inventory;
its public `cmake/VerifyLuaPackage.cmake` verifier applies the shared artifact
privacy guard to source files and both standalone/nested rockspecs. The nested
source tar must match the verified standalone bytes. Source-consumer checks use
the same guard for installed Lua files and module binaries. See [SDK privacy
coverage](sdk.md) for recognized content and its limits.
`rotmos.lua-source-consumer` builds the source rock against installed public SDKs,
with LuaRocks cache/temp/tree under its owned build directory. GNU/musl hosts
execute the installed module using the matching target Lua runtime. Other
targets supply compile/link and installed-path evidence.

The [Core SDK archive](sdk.md) has its own selected-target binary rehearsal and
final-byte/consumer verification. It ships public library payload and metadata;
dependency SDKs stay external. Its `BINARY-CHECKSUMS` is explicitly scoped and
does not replace the full release upload inventory.

The full matrix is GNU/musl Linux for x86_64, aarch64 and armhf, plus arm64 Apple
Darwin. `make build TARGET=<target> CONFIG=release` selects one concrete build;
`make build-all` selects all seven. Matching CMake presets use
`<target>-debug` / `<target>-release`, with `debug` / `release` aliases for the
default selected target. Prepare SDKs with `make deps TARGET=<target>` before
direct preset configuration. Linux uses pinned Bootlin collections; Darwin uses
the selected Apple tools on macOS or approved osxcross on Linux. Discovery never
acquires a compiler or falls back to a host compiler. Full runtime suites execute
when the selected target matches the execution host. The four ARM Linux targets
also have an explicit, limited QEMU smoke gate; Darwin cross builds supply
compile/link/artifact evidence. The [contract](server-api-spec.md) defines runtime/artifact
acceptance and the implementation cadence.

## Verification profiles and reuse

The full local matrix requires x86_64 Linux, approved osxcross, Valgrind,
`qemu-arm`, `qemu-aarch64`, and the skill-pinned AFL++ GCC-plugin collection.
Provision missing AFL++ explicitly with `make fuzz-toolchain`; normal discovery
never downloads or substitutes host compilers. Git release-contract fixtures
also require `ssh-keygen`; their temporary keys and refs stay under `build/`.

| Profile | Scope |
| --- | --- |
| Native plain | Full GNU/musl Core and Lua suites, installed consumers and source reconstruction. Release test assertions remain enabled. |
| Native memory | Twelve Core cases and eight installed Lua cases, including both loading modes and custom decoding of spilled bodies, through Valgrind. Invalid memory/uninitialized uses and definite/indirect leaks fail. Logs are per process; no blanket suppressions. |
| ARM QEMU | Three cases per target: wide-count/MIME/basic HTTP lifecycle and Lua preload/dynamic loading with basic JSON and inert routes. No full runtime suite, Valgrind or fuzzing. |
| Native fuzz | Owned proxy HTTP/chunk framing, WebSocket handshake/frame, route/range/ETag parsers. Instrumented C89 sources reuse the installed GNU Release SDK for support code. Seed replay, coverage-map presence, nonzero executions and zero saved crashes/hangs are required. |

`make verify-all` builds each selected configuration once through `lua-all`,
then checks its existing binaries. Host-tool checks run once. Native SDK tests
preserve and verify the final archive separately from their corruption fixtures;
cross SDKs get their own final-byte and relocated-consumer checks. No check invokes
another full matrix or cleans matching Core/Lua products. The default Debug/editor build
and the optimized Release build are distinct inputs, not interchangeable proof.

Installed reference consumers use a CTest preparation fixture and separate
static/shared execution cases, retaining consumer objects and unchanged SDK
timestamps between invocations. The original install prefix is unavailable while
they configure, link and execute against the relocated SDK. Each executed program
has its own bounded deadline and diagnostic; preparation no longer shares one
timeout with the entire execution inventory. JSON cases share this producer.

There is one instrumented producer at `build/release/fuzz/`, including its clangd
database, shared by smoke and longer campaigns. The smoke budget is 10 seconds;
`make fuzz` defaults to 300 seconds. AFL's own queue resumes within the selected
campaign directory. Saved findings fail the command and remain available for
reproduction. The harness is non-dumpable; no host core handler, CPU policy or
affinity is changed. Fuzz and memory tools never run through QEMU or on macOS.
Resume refuses unresolved saved crashes/hangs before AFL can rotate their counts.

Some user-mode QEMU builds return `ENOSYS` for `get_robust_list`; that prevents
preparation of the required robust shared autoblock lock. Those emulated runs
can prove log transport but cannot supply autoblock runtime evidence. Use a
Linux execution environment with robust-list support for that check; the
library retains its worker-death recovery requirement.

## Version and release identity

An owned Git checkout uses one exact lightweight `vX.Y.Z` tag on HEAD,
an explicit `CANDIDATE_VERSION=X.Y.Z` preparation override, or `0.0.0`.
Annotated/symbolic and ambiguous release tags are rejected. A checked-in VERSION
is not consulted. Lua source archives receive VERSION and RELEASE_MANIFEST;
extracted builds validate those bytes and ignore an enclosing checkout's Git
identity. Both products and their source/installed consumers share this rule.

`make prerelease` incrementally completes the Release matrix without publishing.
`make release-local` first requires the resolved default branch, a clean committed
tree, a verified signed HEAD, its exact lightweight tag and no candidate override.
Only then does it clean once, run the complete Release gate and copy verified
Core/Lua products to `dist/release-vX.Y.Z/`. Its complete CHECKSUMS inventory and
the manifest's SHA-256 are reported. Existing final output is not overwritten.
These commands never sign, create active release refs, push or publish; branch
integration, signing/tagging and publication retain the lifecycle's operator
authorization boundary.

## Source ownership

- `vendor/kore/` is an unmodified upstream source export, including its license.
- `vendor/kore.revision` and `vendor/kore.url` identify its exact upstream commit
  and repository. There is no nested Git checkout or submodule.
- `patches/kore/series` orders patches stored outside the source snapshot.
- `build/debug/kore/` is a disposable patched copy; source-only preparation
  uses `build/source/kore/`. Preserve changes by editing
  or adding a patch, updating `series`, then rebuilding.

CMake tracks upstream sources, patches, `series`, provenance and the preparation
script. A changed input regenerates the copy from pristine sources; an unchanged
build reuses it. Patch failures name the patch and stop the build without
publishing a partially patched copy.

Backend objects depend directly on successful source preparation. A patch edit
must prepare and recompile its changed source in one build invocation; CTest's
`kore.backend-invalidation` proves that behavior with a compiled fixture.
Preparation also depends on the shared path validator. Source exports and patch
inputs must belong to this checkout and stay separate from both the output and
its staging directory; overlap is rejected before cleanup. Patch files and the
series cannot be symlinks.

## Upgrading Kore

Start with committed, pristine `vendor/kore/` and provenance files:

```sh
make update-kore KORE_REVISION=<full-40-character-upstream-commit>
make test
git diff -- vendor/kore vendor/kore.revision patches/kore
```

When staging an accepted snapshot, use `git add -f -- vendor/kore` so Kore's own
unmodified `.gitignore` does not hide newly introduced public headers. That
directory must contain only the pristine export; generated files belong in
`build/`.

The updater fetches the recorded upstream into owned `build/` scratch, exports
the exact requested commit and tests the current patch stack against that
candidate. Only a successful replay replaces the tracked pristine snapshot and
its revision. A conflict leaves both unchanged and reports the failing patch.
Rebase or repair the patch files against the candidate revision, then repeat the
update. Patch application alone does not prove runtime compatibility: C89,
behavior and installed-consumer checks also apply once the library is built.
The upgrade command does not commit, tag or push changes.

The current source pin is `c5fe59f521bdc1f2c486b5d8cc90227d72fcc628`; the
original extraction used `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b`. All 62
patches replay unchanged across that upgrade. The original patches came from
Vectis commit
`6b1e263e0436687f68a7768ca55f4361a6e59e19`; that provenance is recorded
separately in `patches/kore/vectis.revision`. Fixes belong in this repository's
external patch stack.

The [contract](server-api-spec.md), [integration inventory](vectis-integration-contract.md)
and [verification record](verification.md) define scope and evidence limits.
Imported Kore sources retain their notices in [vendor/kore/LICENSE](../vendor/kore/LICENSE).

## Public API editor comments

Public C headers carry Doxygen comments recognized by clangd, including callbacks,
configuration members, enum values and ownership/error rules. Build the selected
Core/Lua products once to populate their compilation databases before using
`.clangd`. Keep declaration comments with the public API when behavior changes.

The Lua facade's annotations reference the shipped `rotmos.meta.lua` definitions,
including methods implemented dynamically in C. LuaLS discovers both files in the
source tree; installed consumers add their `share/lua/5.5` prefix to the language
server's workspace library. Metadata contains no runtime implementation and
requires no extra module at runtime. CMake and LuaRocks both install it.

## Debug test thread stacks

GNU Linux Debug test executables set the process's default pthread stack size to
128 KiB before `main`. This includes the Lua test hosts, installed-consumer
fixtures and SDK example applications launched by the tests. The setting belongs
to these test applications; Core/Lua libraries and exported SDK targets do not
install it. Main-thread stack limits and build-tool worker defaults are unchanged.
Explicitly requested pthread stack sizes remain effective, and pthread-owned
stacks retain nonzero guard regions.

CTest disables glibc's pthread stack cache at process startup by appending
`glibc.pthread.stack_cache_size=0` to `GLIBC_TUNABLES`, preserving unrelated
tunables and passing the setting to child applications. This prevents reuse of
a larger cached stack from masking the 128 KiB constraint. To run a Debug test
executable directly, supply the same startup environment, for example:

```sh
GLIBC_TUNABLES="${GLIBC_TUNABLES:+${GLIBC_TUNABLES}:}glibc.pthread.stack_cache_size=0" \
  build/debug/rotmos_xml_stack
```

An instrumented test application rejects startup without that cache setting.
The policy tests check actual default/nested worker sizes, a larger explicit
stack, guard faults and child-process behavior. Native musl checks independently
exercise musl's natural worker defaults; Release tests keep their normal defaults.

Owned C sources compile with `-Werror -Wvla -Walloca`. Use the project's heap
allocator for dynamic or large temporary storage, with explicit allocation-failure
handling and cleanup. Small fixed chunk buffers still need to fit the worker's
stack budget. A compiler-policy check accepts a bounded fixed buffer and requires
VLA and `alloca` fixtures to fail compilation.

## Traversal performance checks

The normal `rotmos.xml-stack` and `rotmos.kore-json-stack` checks assert
allocation-cost bounds as well as worker-stack correctness. The
`rotmos.traversal-performance-contract` check proves that the timing comparator
accepts a 4% increase and rejects a 6% increase against its 5% budget. These
deterministic checks run without timing the machine.

For a measured before/after comparison, build `rotmos_traversal_benchmark` in
Release and save its executable under owned `build/` before changing production
code. It links Core statically. Keep the selected compiler, dependency SDKs and
benchmark source/configuration identical for the candidate, and record the
baseline revision. Build the candidate Release executable, then run:

```sh
cmake -DBASELINE="$PWD/build/traversal-performance/baseline-benchmark" \
  -DCANDIDATE="$PWD/build/release/rotmos_traversal_benchmark" \
  -DOUTPUT_DIR="$PWD/build/traversal-performance/results" \
  -P tests/TraversalPerformance.cmake
```

The script alternates three baseline/candidate runs. Each reports the median
of five process-CPU samples of at least 50 ms per workload; the comparator
asserts that the median of the three reported values does not regress by more
than 5%. XML covers a scalar record and depths 3/128; Kore covers narrow/wide
serialization, nested serialization and wide/nested destruction. Destruction
timing excludes tree construction. Logs retain both raw sets and the ratios.
These focused checks support traversal changes; server throughput and other
hardware/dependency configurations need their own measurements.
