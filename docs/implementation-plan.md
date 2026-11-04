# Extraction implementation plan

The [extraction contract](server-api-spec.md) governs this work. Existing source
and tests are candidates for reuse, not an approved design to preserve wholesale.
The implementation goal remains paused; this document does not resume it.

1. Map the baseline Vectis operations to an installed public reference adapter.
   Show exact config, callback, ownership, timing and error translation. Identify
   missing operations before choosing new interfaces. Keep sibling repos read-only.
2. Retain traceable lifts and necessary C89/backend adaptations. Remove duplicate
   Lua route/codec orchestration and unnecessary public wrappers. Review the new
   response/loop/executor/storage/ACME machinery against actual adapter needs;
   narrow it without removing required behavior or moving transport work to Vectis.
3. Correct known parity failures: XML live input, ordinary JSON materialization
   before after-hooks, one-pass Lua REST output, shared C/Lua typed operations,
   and selected-array caller-owned storage/request-source convenience. Reuse
   released LoneJSON interop; the proposed upstream conversion is not a blocker.
4. Complete remaining required Lua/provider operations and embedding/package
   delivery through the corrected public API. Preserve normal and packed Vectis
   composition and keep application services/providers in Vectis.
5. Run short baseline-derived invariant/failure checks for coherent batches;
   reuse passing evidence. Then complete cross-target, installed-consumer,
   streaming/fault, compatibility and final artifact checks under the lifecycle.
   Do not claim readiness before the reference adapter and consumer gates pass.

Retain the explicit seven-target/toolchain/build-directory rules in the spec.
No host compiler fallback, sibling build input, private binding include, hidden
materialization, new general-purpose SDK or speculative dependency API.

At the rewrite boundary, three unverified selected-array edits remain in
include/rotmos/lonejson.h, src/json_array.c and tests/json.c. They are not an
accepted repair. Resolve them deliberately when implementation resumes; this
specification rewrite neither commits nor verifies them.
