# Core SDK archives

`make sdk-package TARGET=<target> CONFIG=debug|release` stages the selected
CMake install and writes a binary rehearsal under that build's `packages/`:

- `libknurl-<version>-<target>.tar.gz`
- `libknurl-<version>-<target>-BINARY-CHECKSUMS`

The archive contains the static archive, ABI-versioned shared library and
relative symlinks, public C headers, relocatable CMake/pkg-config metadata,
licenses, provenance/dependency records and the foreground server example.
Start at `share/knurl/README.md` for the C and Lua quick starts. The public
guides are under `share/knurl/docs/`, Lua setup is in `share/knurl/lua/README.md`,
and the SDK example guide is in `share/knurl/examples/README.md`.
`share/knurl/RELEASE_MANIFEST` identifies every other installed file or symlink.
Archive ordering, timestamps, permissions, owner IDs and gzip metadata are
normalized. Static archives and Linux shared libraries omit debug sections;
Darwin debug-map object names are relative at link time; the packager does not
post-mutate installed dylibs.

The HTTP completeness work uses shared-library ABI 1, advanced once from the
released ABI 0. Linux uses SONAME `libknurl.so.1`; Darwin uses install name
`@rpath/libknurl.1.dylib` and compatibility/current versions `1.0.0`. Those Mach-O
versions track ABI independently of the repository's release or development
version. Package verification checks the installed library metadata.

Dependency SDKs are external: cpkt 0.7.0, LoneJSON 0.50.0 and pslog 0.13.0. The
selected SDK hashes and linked cpkt component versions appear in `DEPENDENCIES`.
LoneJSON 0.50.0 retains 0.49.0's shared-library ABI 29. Rebuild Core, its Lua
provider and downstream consumers together against these headers;
0.47.0/0.48.0 binaries are not a compatible runtime substitute.
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

Lua source archives, standalone rockspecs and the source rock's metadata use
the same guard. Its nested source tar must exactly match the verified standalone
tar. Installed LuaRocks facade/module files are checked too. Existing Lua
package/source-consumer CTest gates enforce this verification. To inspect staged
products directly, use `cmake -DPRODUCT_DIR=<absolute-owned-directory>
-P cmake/VerifyLuaPackage.cmake`; scratch stays under `build/`.

These commands do not create a release, tag, upload or complete multi-product
release inventory. [Release preparation](https://github.com/c89systems/libknurl/blob/trunk/docs/releasing.md) defines that inventory
and the default-branch, signing, clean-build and final-artifact gates. Lua products
retain their own checksums; a partial binary inventory cannot stand in for release
CHECKSUMS.
