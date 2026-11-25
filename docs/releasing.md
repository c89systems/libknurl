# Release preparation and artifacts

The release delivers the C SDK and the standalone `knurl` runner as separate
products. The [Lua runner and loadable module](lua-config.md) have separate
source build and installation components. The loadable module is available
from source. See [runtime direction](runtime-direction.md).

`make prerelease JOBS=1` runs the Release matrix from the development branch:
native C suites, installed consumers, memory checks, parser seed replay and
bounded fuzzing, ARM QEMU, seven-target SDK verification, three runner packages and formatting. It
preserves matching build products and does not perform a clean tagged release.
See [Testing](testing.md) for profile scope. Historical [verification](verification.md)
records retain their original scope, including now-archived products.

Complete GNU Debug C and editor checks during preparation. Debug and Release
are distinct inputs. Reproduce failures in their smallest meaningful scope,
repair them and rerun the affected gate. After a tag/ref transition, reconfigure
with the matching candidate version before reusing products.

## Version and release identity

An owned Git checkout uses one exact lightweight `vX.Y.Z` tag on HEAD, an
explicit `CANDIDATE_VERSION=X.Y.Z` preparation override, or `0.0.0`.
Annotated/symbolic and ambiguous release tags are rejected. A checked-in VERSION
is not consulted. Shared-library ABI is independent of the release version.
The 0.2.0 release retains ABI 0 by explicit product decision. Artifact
verification checks the installed Linux and Darwin metadata against that value.

`make release-local` first requires the resolved default branch, a clean committed
tree, a verified signed HEAD, its exact lightweight tag and no candidate override.
It then cleans once, runs the complete Release gate and copies verified C SDK and runner tarballs
to `dist/release-vX.Y.Z/`. Its CHECKSUMS inventory and SHA-256 are reported.
Existing final output is not overwritten. These commands never sign, create
release refs, push or publish. Those actions retain their operator authorization
boundary. A failed final gate stops publication; preserve evidence and repair on
the development branch before a separately authorized release attempt.

## Artifact inventory

A complete release has **ten payloads and one CHECKSUMS manifest**.

| Product | Filename | Count |
| --- | --- | ---: |
| C SDK | `libknurl-<version>-<target>.tar.gz` | 7 |
| Runner | `knurl-<version>-<target>.tar.gz` | 3 |

Targets are GNU/musl Linux on x86_64, aarch64 and armhf, and arm64 Apple Darwin.
Each SDK carries static/shared libraries, public C headers, relocatable consumer
metadata, C examples, documentation and notices. External dependency SDKs stay
separate.

The runner ships for `x86_64-linux-musl`, `aarch64-linux-musl`, and
`arm64-apple-darwin`. Linux packages contain a fully static ELF. The macOS
runner links project and third-party libraries statically and uses Apple
system libraries and frameworks. Its deployment target is macOS 15.

Use `make runner-package TARGET=<runner-target> CONFIG=release` to produce
and verify a selected runner tarball under `build/<target>/release/packages/`.
The gate checks the extracted architecture, dependency metadata, and notices.
Linux executes the relocated binary natively or via AArch64 QEMU, including
worker lifecycle and native-route responsiveness during Lua I/O. The QEMU
fixture disables autoblock because user-mode emulation lacks robust shared
mutex support. osxcross
verifies the extracted Mach-O on Linux. Run the same command on Apple Silicon
to exercise the relocated macOS executable.

[SDK delivery](sdk.md) describes selected-target binary rehearsals under
`build/<target>/release/packages/` (default GNU: `build/release/packages/`). A
rehearsal may use `0.0.0`. The final collector uses exact expected paths and
checksums. The lifecycle fixture proves the ten-file inventory, stale-product exclusion,
checksum corruption rejection and refusal to overwrite a frozen final directory.
