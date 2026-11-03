# Build availability and backend maintenance

The [README](../README.md) describes the server library's product contract.
The build produces a C89 static/shared library with wide counters, errors, SSE
framing, owned sources and staged reply builders. Reply builders copy text/bytes
and ordered headers, pin regular files, and distinguish live source ownership
from explicitly bounded synchronous materialization. Source registration never
reads or resets a producer. It also compiles the selected private Kore backend
objects under strict C89, applying the 42 original patches, an external C89 port
and the private library lifecycle patch. The selected backend excludes the
standalone CLI configuration parser/assets and uses typed callbacks. Its native
fixture serves HTTP, handles worker-initiated stop, repeats a run, and recovers
from an injected error after bind. Inert server construction copies listeners,
TLS material, callbacks,
trusted-proxy declarations and autoblock rules, and resolves effective settings.
The router owns stable route/group handles, validates declarations and preserves
registration order, prefixes and exact method/path conflicts. Construction and
registration execute no user callbacks or network operations.

Backend objects are not yet linked into the library. Live server execution,
request dispatch and the Lua module remain implementation work. The contract is in the
[server API specification](server-api-spec.md) and
[Vectis integration contract](vectis-integration-contract.md).

## Commands

Prerequisites: CMake >= 3.24, Ninja, Git, Make, Bash >= 4.4, curl, tar, flock,
SHA-256 tools and clang-format. Linux compilation uses the lifecycle's pinned
Bootlin GCC/binutils/sysroot collection, with no host compiler fallback.

```sh
make help
make toolchain             # Explicit provisioning; unnecessary when already prepared.
make build                 # Prepare released SDKs; compile core and private backend.
make test                  # CTest foundation and backend-maintenance checks.
make install               # Install to owned build/install/<target>/Debug by default.
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

The native Debug database is `build/native/Debug/compile_commands.json`, matching
`.clangd`. `make build` prepares the real headers and database used by the editor.
Debug/Release CMake presets use the x86_64 GNU toolchain; prepare SDKs with
`make deps` before direct preset configuration. Linux cross configurations use
`make build TARGET=<target> CONFIG=Release` and separate target build directories.
Darwin toolchain wiring and complete platform/runtime proof remain subsequent
slices. No release artifacts or replacement-readiness claim follow from the
foundation build alone.

## Source ownership

- `vendor/kore/` is an unmodified upstream source export, including its license.
- `vendor/kore.revision` and `vendor/kore.url` identify its exact upstream commit
  and repository. There is no nested Git checkout or submodule.
- `patches/kore/series` orders patches stored outside the source snapshot.
- `build/native/Debug/kore/` is a disposable patched copy; source-only preparation
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
