# Core SDK archives

`make sdk-package TARGET=<target> CONFIG=debug|release` stages the selected
CMake install and writes a binary rehearsal under that build's `packages/`:

- `libknurl-<version>-<target>.tar.gz`
- `libknurl-<version>-<target>-BINARY-CHECKSUMS`

The archive contains the static archive, ABI-versioned shared library and
relative symlinks, public C headers, relocatable CMake/pkg-config metadata,
licenses, provenance/dependency records and the foreground server example.
Start at `share/knurl/README.md` for the C quick start. The public guides are under
`share/knurl/docs/`, and the SDK example guide is in
`share/knurl/examples/README.md`. Archived products are not installed.
`share/knurl/RELEASE_MANIFEST` identifies every other installed file or symlink.
Archive ordering, timestamps, permissions, owner IDs and gzip metadata are
normalized. Static archives and Linux shared libraries omit debug sections;
Darwin debug-map object names are relative at link time; the packager does not
post-mutate installed dylibs.

libknurl 0.2.0 retains shared-library ABI 0 under the project's release policy.
Rebuild consumers against the current public headers and matching dependency
SDKs. Linux uses SONAME `libknurl.so.0`; Darwin uses install name
`@rpath/libknurl.0.dylib` and compatibility/current versions `0.0.0`.
Those Mach-O versions track ABI
independently of the repository's release or development version. Package
verification checks the installed library metadata.

Dependency SDKs are external: cpkt 0.8.0, LoneJSON 0.50.0 and pslog 0.13.0. The
selected SDK hashes and linked cpkt component versions appear in `DEPENDENCIES`.
LoneJSON 0.50.0 retains 0.49.0's shared-library ABI 29. Rebuild Core and downstream
consumers together against these headers;
0.47.0/0.48.0 binaries are not a compatible runtime substitute.
Provider libraries are not copied into this archive. Static consumers need their
archive closure; shared consumers need the matching runtime libraries. The
pkg-config metadata declares that closure so selected-target linking resolves it
without relying on host libraries. The C product does not link a Lua runtime or
acquire Lua provider sources or a libpid0 SDK. The external cpkt SDK may carry
additional unused components. See [runtime direction](runtime-direction.md).

`make sdk-verify TARGET=<target> CONFIG=<configuration>` reads the existing
archive, checks its checksum/inventory, metadata, licenses, architecture, ABI
identity, private-path/instrumentation exclusions and public export boundary.
It relocates the extracted SDK and builds the shipped example four ways:
CMake/pkg-config with static/shared linkage. GNU/musl x86_64 consumers execute
`--check`; the other targets supply compile/link and target-format inspection.
The example runs a real foreground HTTP server on 127.0.0.1:8080 without that
option. `knurl.sdk-package` is the short CTest gate for reproducibility under a
changed staging umask, final-byte/consumer validation, checksum corruption,
unlisted private headers and private paths even with recomputed digests.

The shared artifact privacy guard inspects declared text and binary payload for
known workstation paths, the current checkout path, sanitizer/AFL markers,
complete private-key PEM blocks (including SSH and encrypted-key forms), and
long GitHub token forms. Diagnostics report the artifact and violation class
without printing matched content. Public certificates, isolated PEM labels,
API names and short redacted examples remain valid. Explicit inventories exclude
unintended files; content matching cannot identify arbitrary opaque secrets.
The selected target archiver also verifies zero member UID/GID and epoch
timestamps inside the static library. Linux stripping uses deterministic-archive
mode, so nested archive metadata neither discloses host IDs nor changes on repack.

These commands do not create a release, tag, upload or complete multi-product
release inventory. [Release preparation](https://github.com/c89systems/libknurl/blob/trunk/docs/releasing.md) defines that inventory
and the default-branch, signing, clean-build and final-artifact gates. A selected-
target binary inventory cannot stand in for the seven-target release CHECKSUMS.
