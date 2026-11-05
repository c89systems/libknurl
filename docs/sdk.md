# Core SDK archives

`make sdk-package TARGET=<target> CONFIG=debug|release` stages the selected
CMake install and writes a binary rehearsal under that build's `packages/`:

- `librotmos-<version>-<target>.tar.gz`
- `librotmos-<version>-<target>-BINARY-CHECKSUMS`

The archive contains the static archive, ABI-versioned shared library and
relative symlinks, public C headers, relocatable CMake/pkg-config metadata,
licenses, provenance/dependency records and the foreground server example.
`share/rotmos/RELEASE_MANIFEST` identifies every other installed file or symlink.
Archive ordering, timestamps, permissions, owner IDs and gzip metadata are
normalized. Static archives and Linux shared libraries omit debug sections;
Darwin debug-map object names are relative at link time; the packager does not
post-mutate installed dylibs.

Dependency SDKs are external: cpkt 0.6.1, LoneJSON 0.46.0 and pslog 0.13.0. The
selected SDK hashes and linked cpkt component versions appear in `DEPENDENCIES`.
Provider libraries are not copied into this archive. Static consumers need their
archive closure; shared consumers need the matching runtime libraries. The
pkg-config metadata declares that closure so selected-target linking resolves it
without relying on host libraries. The Lua facade/binding/source rock is a
separate install product using the same Core/provider SDKs and the host's VM.

`make sdk-verify TARGET=<target> CONFIG=<configuration>` reads the existing
archive, checks its checksum/inventory, metadata, licenses, architecture, ABI
identity, private-path/instrumentation exclusions and public export boundary.
It relocates the extracted SDK and builds the shipped example four ways:
CMake/pkg-config with static/shared linkage. GNU/musl x86_64 consumers execute
`--check`; the other targets supply compile/link and target-format inspection.
The example runs a real foreground HTTP server on 127.0.0.1:8080 without that
option. `rotmos.sdk-package` is the short CTest gate for reproducibility under a
changed staging umask, final-byte/consumer validation, checksum corruption,
unlisted private headers and private paths even with recomputed digests.

These commands do not create a release, tag, upload or complete multi-product
release inventory. A release must still follow the lifecycle's default-branch,
signing, clean-build and full final-artifact gates. Lua products retain their
own checksums; a partial binary inventory cannot stand in for release CHECKSUMS.
