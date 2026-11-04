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

The ACME publication core is in `src/acme_store.c` and `src/acme_files.c`.
The next adapter slice must connect it to the existing public server lifecycle:

- Preflight checks the prepared root and explicit/environment CA inputs without
  callbacks or traffic; snapshot trust before forks.
- RESTORE runs in the quiescent parent against a fresh generation. Key-manager
  recovery/work preparation runs after its IPC initialization, before loading
  keys or distributing certificates.
- Replace the backend's direct live-certificate write with private working-file
  completion, snapshot validation, PERSIST, acknowledgement and publication.
  Keep the dedicated child budget armed through that bounded operation.
- Preserve manual in-memory identities when ACME enables the key manager.
  Apply cached protocol/client-auth policy after native ACME context updates.
- Report pending/usable identity state and preserve TLS-ALPN-01 challenge
  handling while application serving is paused. Connect the callback error to
  parent failure/recovery observations.
- Prove actual certificate activation, failed persistence and restart behavior
  through the public server before allowing ACME runs. The publication fixture
  alone does not prove that integration.
