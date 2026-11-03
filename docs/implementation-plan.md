# Implementation constraints

The active implementation goal covers the complete
[server API](server-api-spec.md) and [Vectis integration contract](vectis-integration-contract.md).
The end state is a mechanical Vectis cutover through public C and Lua APIs.

- Keep Kore pristine; apply the external patch series only to owned generated copies.
- Compile owned C and the selected backend with strict `-std=c89`.
- Follow the lifecycle skill strictly, including exact released dependency SDKs,
  independent prefixes, toolchain selection, packaging and final-artifact evidence.
- Build the full cpkt matrix: GNU/musl Linux for x86_64, aarch64 and armhf,
  plus arm64 Apple Darwin. Each build selects its concrete architecture/OS and
  lifecycle toolchain. There is no host-selected or native product.
- Use `build/debug` and `build/release` for the default selected
  `x86_64-linux-gnu` target. Other targets use `build/<target>/debug` or
  `build/<target>/release`. Keep the editor database at `build/debug`.
- Prioritize large coherent implementation surfaces. Add short invariant and
  failure-mode tests; avoid lengthy checks, repeated broad suites and detailed
  per-slice audits. Complete comprehensive behavioral, cross-target, installed
  consumer, lifecycle and packaging verification after the larger surfaces land.
- Keep sibling repositories read-only. Commit coherent changes on the feature
  branch without pushing. No replacement-readiness claim until the full contract
  is implemented and verified.
