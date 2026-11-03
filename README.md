# librotmos

librotmos is being extracted from Vectis's enhanced Kore HTTP runtime. Its intended
public surface is a librotmos C API, with Kore as the backend and an explicit
backend escape hatch. Vectis will retain its application and Lua interfaces.

**Current status:** this repository contains the pristine Kore snapshot, all 42
Vectis patches, a build-time patch preparation target, an upgrade command and
vendoring regression tests. It does not yet build a server library. The public
API, C89 backend port and Vectis cutover remain to be implemented. See the
[server API specification](docs/server-api-spec.md) for the proposed contract and
the [extraction boundary](docs/extraction.md) for the bootstrap context.

## Source ownership

- `vendor/kore/` is an unmodified upstream source export, including its license.
- `vendor/kore.revision` and `vendor/kore.url` identify its exact upstream commit
  and repository. There is no nested Git checkout or submodule.
- `patches/kore/series` orders patches stored outside the source snapshot.
- `build/native/kore/` is a disposable patched copy. Never edit it to preserve a
  change: edit or add a patch, update `series`, then rebuild.

Normal builds and tests are offline. CMake tracks upstream sources, patches,
`series`, provenance and the preparation script. A changed input regenerates the
copy from pristine sources; an unchanged build reuses it. Patch failures name the
patch and stop the build without publishing a partially patched copy. One
operation owns the checkout; run build, test and upgrade operations serially.

## Commands

Prerequisites for this bootstrap: CMake >= 3.21, Git, Make and Bash >= 4.4. Upgrades
also use `tar` and `mktemp`. The server dependency SDKs are not required for these
vendoring tests, because no C library is compiled yet.

```sh
make help
make build                 # Prepare build/native/kore; does not compile a library.
make test                  # Replay, failure, upgrade and invalidation tests.
make clean                 # Remove all local build/, dist/ and .cache/ state.
```

To upgrade, start with committed, pristine `vendor/kore/` and provenance files:

```sh
make update-kore KORE_REVISION=<full-40-character-upstream-commit>
make test
git diff -- vendor/kore vendor/kore.revision patches/kore
```

When staging an accepted snapshot, use `git add -f -- vendor/kore` so Kore's own
unmodified `.gitignore` does not hide newly introduced public headers. That
directory must contain only the pristine export; generated files belong in
`build/`.

The updater fetches the recorded upstream into owned `build/` scratch, exports the
exact requested commit and tests the current patch stack against that candidate.
Only a successful replay replaces the tracked pristine snapshot and its revision.
A conflict leaves both unchanged and reports the failing patch. Rebase or repair
the patch files against the candidate revision, then repeat the update. Patch
application alone does not prove a compatible runtime upgrade: the future C89,
behavior and consumer checks must also pass before accepting one. No command
commits, tags or pushes an upgrade automatically.

The initial source pin is `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b`. The original
patches were imported from Vectis commit
`6b1e263e0436687f68a7768ca55f4361a6e59e19`; that provenance is recorded separately
in `patches/kore/vectis.revision`. Future fixes belong in this repository's stack.

librotmos-owned code is MIT licensed. Imported Kore sources retain their upstream
copyright and permission notices in [vendor/kore/LICENSE](vendor/kore/LICENSE).
