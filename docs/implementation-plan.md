# Extraction implementation plan

The [extraction contract](server-api-spec.md) governs this work. Existing source
and tests are candidates for reuse, not an approved design to preserve wholesale.
The revised implementation goal is active following the user's explicit restart.

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

The unverified selected-array ownership patch was discarded. Selected-array
request/source operations now follow Vectis's pull loop with caller-owned storage
and index callbacks. Lua uses its own record plus the existing public conversion;
the unused owned-record push-parser API is removed.
The first reference-adapter case forwards the baseline typed JSON callback shape
and asserts completed JSON bytes in an after-hook. Ordinary C JSON route output
now materializes before that hook; explicit live typed replies remain separate.
Lua REST replies now forward one encoder output directly, preserving bytes and
optional newline without a second parse/emission or default output-size cap.
C XML routes now reuse live upload-reader parsing with owner-loop completion;
the forwarding fixture checks progress before EOF and parser cancellation. See
the XML implementation note for the baseline reader-thread/join transition.
Lua typed routes now use the public C registrations with an optional record-owner
adapter. The facade only adapts request arguments; parsing, dispatch, ordinary
serialization and hook ordering share the C path. Acquire/release retain Lua
records through response cleanup or native acknowledgement on cancellation.
The adapter is partial: the remaining operation families and known gaps above
still need their corresponding forwarding/parity cases.
The unused standalone framer has been removed. Native executor/job declarations
are private, with export/installed-header checks; the proxy framer and native
I/O/filesystem mechanisms retain their internal callers and invariant coverage.
Lua server logging now forwards public pslog owner references and access policy
through the C logging registration. The released pslog Lua provider shares the
host VM/C SDK; application seed/category policy stays outside librotmos.
Host asset/storage providers now enter Lua through public borrowed-provider
interop values. Static-assets and WebDAV registrations retain the host owner and
use the existing C engines. The host-module fixture builds against installed
C/Lua SDKs on all seven targets; GNU/musl preload/dynamic workflows are exercised.
The server reference now maps all 34 baseline configuration fields, including
explicit body-limit inheritance, copied rules/strings and application defaults.
Its worker case covers foreground and host-controlled paused readiness, callback
ordering, stop results and successful control-FD transfer. Baseline second-based
timeouts round up and saturate; mapped JSON records have no invented size cap.
The remaining operation/error/type forwarding cases are still incomplete.
Lua source delivery now has an explicit minimal input manifest, deterministic
source archive, public-URL release rockspec and source rock. The source consumer
uses only installed public SDKs and the included concrete toolchain selector;
LuaRocks cache/temp/tree stay under build. Final/nested byte and reconstruction
checks complement the existing binding workflow tests. Full release artifact
validation and the complete Vectis adapter remain open.
Ordinary-route forwarding now includes baseline status/errors, request string
conversion and host-source callback/ownership adaptation. Delayed live reset is
an explicit public C option shared by Lua. Its parity case also exposed and
corrected cleanup after request-close on reset failure. Body policy/file
forwarding now covers threshold spill and disabled-spill/I/O failure timing,
intrinsic/server-limit registration checks, borrowed readers and deferred file
opening after hooks. Remaining route/provider, proxy/TLS and lifecycle operation
forwarding and the broader surface audit remain open.
Managed and ordinary Kore responses now share the backend head codec through an
external patch. Configured Server/HSTS, keep-alive metadata/request budgets,
native cookies/headers and optional error pages are preserved. Default response
headers have no added size ceiling; pending heads drain within the output queue.
The public reference fixture checks large heads, manual backpressure/replacement,
HEAD/304 and configured error output. TLS/native backend cases check HSTS and
cookie/header merging. The combined request/reply batch adds independent body
materialization/copy/spill/result-reader forwarding, baseline header replacement
and transport fields, and shared Kore file metadata/conditional replies. It fixes
default-directory spill execution and caller-buffer edge cases. Lua status/hook
validation and REST body defaults now follow the baseline. New cases cover these
operations together with registration order, masks and conflicts.
The service adapter now forwards proxy callbacks/edits, WebDAV storage/auth,
upload stream/reader/file, WebSocket and resolved TLS/redirect declarations.
One combined runtime case checks proxy, DAV, uploads, WebSockets and SSE. Native
reader ownership now preserves host request facades through cancellation/ACK;
successful unselected uploads return the baseline 204. WebSocket send/input/close
byte behavior follows Kore, including outgoing continuation and empty peer-close
replies. Live TLS/mTLS/ACME provider forwarding and remaining service options
still need baseline adapter evidence. Then finish the
remaining surface audit and consolidated consumer/artifact gates. The complete
Vectis adapter and final compatibility claim remain open.
