# Development

The [extraction contract](server-api-spec.md) defines the product. The
[integration inventory](vectis-integration-contract.md) maps operation coverage and
[verification](verification.md) records the completed development checks.
Actual Vectis cutover and release verification have separate gates.

See [Testing](testing.md), [Editor setup](editors.md) and
[Release preparation](releasing.md) for their dedicated workflows.

## Planned extensions

The [SOAP 1.2 server specification](soap12-spec.md) records the future C/Lua
endpoint contract, namespace-aware XML requirements, streaming and ownership
rules, fault behavior and acceptance tests. It describes planned work rather
than an implemented or released capability.

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
that target. The default is `x86_64-linux-gnu`, using `build/debug` and
`build/release`; other targets use `build/<target>/debug` or `release`.
There is no host-based product selection. [Editor setup](editors.md) describes
how these ordinary builds supply clangd's Core and Lua compilation databases.

Lua provider SDKs have one Release producer per target under
`build/<target>/release/lua-deps/` (default GNU: `build/release/lua-deps/`). Both
Core configurations reuse those dependency bytes. Switching Core configuration
does not replace shared SDKs with differently configured Lua providers.

Lua source-package commands and the complete artifact inventory are described in
[Release preparation](releasing.md). The [Lua package README](../lua/README.md)
contains the build prerequisites shipped with that product.

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

The current source pin is recorded in [vendor/kore.revision](../vendor/kore.revision).
The original extraction used `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b`. The 62
patches present at that upgrade replayed unchanged; the current ordered list is
[patches/kore/series](../patches/kore/series). The original patches came from
Vectis commit
`6b1e263e0436687f68a7768ca55f4361a6e59e19`; that provenance is recorded
separately in `patches/kore/vectis.revision`. Fixes belong in this repository's
external patch stack.

The [contract](server-api-spec.md), [integration inventory](vectis-integration-contract.md)
and [verification record](verification.md) define scope and evidence limits.
Imported Kore sources retain their notices in [vendor/kore/LICENSE](../vendor/kore/LICENSE).
