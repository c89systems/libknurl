# Build availability and backend maintenance

The [README](../README.md) describes the server library's product contract.
The current build contains the pristine Kore snapshot, all 42 Vectis patches,
a build-time patch preparation target, an upgrade command and vendoring
regression tests. It does not yet compile the server library or Lua module.
The public API, C89 backend port and server facade are specified in the
[server API specification](server-api-spec.md) and
[Vectis integration contract](vectis-integration-contract.md).

## Commands

Prerequisites: CMake >= 3.21, Git, Make and Bash >= 4.4. Upgrades also use `tar`
and `mktemp`. The server dependency SDKs are not required for the current
vendoring tests, because no C library is compiled by those targets.

```sh
make help
make build                 # Prepare build/native/kore; does not compile a library.
make test                  # Replay, failure, upgrade and invalidation tests.
make clean                 # Remove all local build/, dist/ and .cache/ state.
```

Normal builds and tests are offline. One operation owns the checkout; run build,
test and upgrade operations serially.

## Source ownership

- `vendor/kore/` is an unmodified upstream source export, including its license.
- `vendor/kore.revision` and `vendor/kore.url` identify its exact upstream commit
  and repository. There is no nested Git checkout or submodule.
- `patches/kore/series` orders patches stored outside the source snapshot.
- `build/native/kore/` is a disposable patched copy. Preserve changes by editing
  or adding a patch, updating `series`, then rebuilding.

CMake tracks upstream sources, patches, `series`, provenance and the preparation
script. A changed input regenerates the copy from pristine sources; an unchanged
build reuses it. Patch failures name the patch and stop the build without
publishing a partially patched copy.

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
