# Release preparation and artifacts

`make prerelease JOBS=1` runs the Release matrix from the current development
branch, including native Core/Lua, memory, seed replay and bounded parser smoke,
ARM QEMU, seven-target SDK verification and formatting. It preserves matching
build products and does not perform the final clean tagged release.
See [Testing](testing.md) for profile scope and [Verification](verification.md)
for the latest rehearsal evidence.

Complete the GNU Debug Core/Lua and ordinary editor checks on the development
branch during preparation. Debug and Release are distinct verification inputs;
an earlier matrix pass does not establish that asynchronous tests are free of
startup races. Reproduce any failure in its smallest meaningful scope before
repairing it and rerunning the affected gate.

After returning from a tagged commit to development, reconfigure Core and Lua
with the same candidate version before reusing their products. Ref changes can
change the resolved version even when the source tree is identical.

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

Use the declared `make release-local` procedure for the final signed/tagged
gate. Additional diagnostic work belongs in preparation. A failed final gate
stops publication; preserve its evidence and return to the development branch
for a separately authorized repair rather than retrying inside that release.

## Artifact inventory

The complete release has ten payloads plus one `CHECKSUMS` manifest:

| Product | Filename | Count |
| --- | --- | ---: |
| Core SDK | `libknurl-<version>-<target>.tar.gz` | 7 |
| Lua source | `lua-knurl-<version>.tar.gz` | 1 |
| LuaRocks specification | `lua-knurl-<version>-1.rockspec` | 1 |
| Lua source rock | `lua-knurl-<version>-1.src.rock` | 1 |

The SDK targets are GNU/musl Linux on x86_64, aarch64 and armhf, and arm64 Apple
Darwin. Each SDK carries static/shared libraries, public headers, consumer
metadata, examples and notices; external dependency SDKs stay separate.
[SDK delivery](sdk.md) describes the selected-target binary rehearsals under
`build/<target>/release/packages/` (default GNU: `build/release/packages/`).
Lua package tests retain their verified products under
`build/release/lua/tests/package/products/`.

A rehearsal may use development version `0.0.0`. These files are preparation
outputs; the final collector requires the signed default-branch commit and exact
lightweight release tag before it copies bytes into `dist/release-vX.Y.Z/`.
Use the complete manifest for that release rather than a binary-only or
Lua-only checksum inventory.

## Lua source packages

`make lua-package` needs GNU tar, gzip, zip and touch. It stages only the Lua
facade/binding and their declared build inputs from `lua/source-manifest.txt`.
Checkout inputs must be Git-tracked files reached without symlinks; injected
VERSION is generated. Extracted source archives retain their declared inventory.
The three products and `lua-knurl-<version>-CHECKSUMS` are written to `dist/`;
no release or upload occurs. [Package instructions](../lua/README.md) describe
the selected-target SDK build and LuaRocks variables. CTest's
`knurl.lua-package` checks reproducible final/nested bytes and source inventory;
its public `cmake/VerifyLuaPackage.cmake` verifier applies the shared artifact
privacy guard to source files and both standalone/nested rockspecs. The nested
source tar must match the verified standalone bytes. Source-consumer checks use
the same guard for installed Lua files and module binaries. See [SDK privacy
coverage](sdk.md) for recognized content and its limits.
`knurl.lua-source-consumer` builds the source rock against installed public SDKs,
with LuaRocks cache/temp/tree under its owned build directory. GNU/musl hosts
execute the installed module using the matching target Lua runtime. Other
targets supply compile/link and installed-path evidence.
