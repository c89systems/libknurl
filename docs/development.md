# Building and maintaining the extraction

The [extraction contract](server-api-spec.md) defines the product. Current code is under correction; the [audit](server-api-audit.md) records compatibility failures. Build availability and passing existing tests do not establish Vectis replacement readiness.

## Commands

Prerequisites: CMake >= 3.24, Ninja, Git, Make, Bash >= 4.4, curl, tar, flock,
SHA-256 tools and clang-format. Linux compilation uses the lifecycle's pinned
Bootlin GCC/binutils/sysroot collection, with no host compiler fallback.

```sh
make help
make toolchain             # Explicit provisioning; unnecessary when already prepared.
make build                 # Compile the selected x86_64-linux-gnu target in build/debug.
make build-all             # Compile the full seven-target matrix, serially.
make lua                   # Build the separate Lua facade/binding against the installed shared core.
make lua-all               # Compile both products for all seven targets, serially.
make lua-package           # Stage Lua sources, rockspec, source rock and their checksums.
make build-all CONFIG=release
make test                  # Run checks for the selected toolchain target.
make install               # Install to owned build/install/<target>/debug by default.
make format-check          # Check project sources; excludes pristine/patched Kore.
make prepare-kore          # Source preparation only; no compiler or SDK acquisition.
make clean                 # Remove all local build/, dist/ and .cache/ state.
```

`make build` reuses checksum-verified local archives or acquires missing released
SDKs from c89systems. Once prepared, builds and tests are offline. SDKs have
independent prefixes under `.cache/sdk/<target>/`; archives stay in this
checkout's `.cache/`. Existing global cpkt archives may be read and copied.
Toolchain discovery is read-only; only explicit provisioning prepares compilers.
One operation owns the checkout; run operations serially.

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

`make lua-package` needs GNU tar, gzip, zip and touch. It stages only the Lua
facade/binding and their declared build inputs from `lua/source-manifest.txt`.
The three products and `lua-rotmos-<version>-CHECKSUMS` are written to `dist/`;
no release or upload occurs. [Package instructions](../lua/README.md) describe
the selected-target SDK build and LuaRocks variables. CTest's
`rotmos.lua-package` checks reproducible final/nested bytes and source inventory;
`rotmos.lua-source-consumer` builds the source rock against installed public SDKs,
with LuaRocks cache/temp/tree under its owned build directory. GNU/musl hosts
execute the installed module using the matching target Lua runtime. Other
targets supply compile/link and installed-path evidence.

The full matrix is GNU/musl Linux for x86_64, aarch64 and armhf, plus arm64 Apple
Darwin. `make build TARGET=<target> CONFIG=release` selects one concrete build;
`make build-all` selects all seven. Matching CMake presets use
`<target>-debug` / `<target>-release`, with `debug` / `release` aliases for the
default selected target. Prepare SDKs with `make deps TARGET=<target>` before
direct preset configuration. Linux uses pinned Bootlin collections; Darwin uses
the selected Apple tools on macOS or approved osxcross on Linux. Discovery never
acquires a compiler or falls back to a host compiler. Runtime checks execute only
when the selected target matches the execution host; other configurations supply
compile/link evidence. Full runtime and artifact validation follows the larger
implementation surfaces. The [implementation constraints](implementation-plan.md)
record this scope and cadence.

Some user-mode QEMU builds return `ENOSYS` for `get_robust_list`; that prevents
preparation of the required robust shared autoblock lock. Those emulated runs
can prove log transport but cannot supply autoblock runtime evidence. Use a
Linux execution environment with robust-list support for that check; the
library retains its worker-death recovery requirement.

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

The initial source pin is `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b`. The
original patches were imported from Vectis commit
`6b1e263e0436687f68a7768ca55f4361a6e59e19`; that provenance is recorded
separately in `patches/kore/vectis.revision`. Fixes belong in this repository's
external patch stack.

The [ownership boundary](extraction.md) and [specification audit](server-api-audit.md)
provide implementation context and evidence limits. Imported Kore sources retain
their notices in [vendor/kore/LICENSE](../vendor/kore/LICENSE).
