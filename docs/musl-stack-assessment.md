# Musl worker-stack assessment (2026-11-08)

This document records the historical diagnosis before the iterative mapper and
Kore patch 0066. Both local corrections are now implemented; see
[verification](verification.md) and the [XML manual](xml.md). The separate
LoneJSON initialization and provisional Lua binding handoffs remain open.

The scoped investigation at `c48fff7` confirms a local mapped-XML recursion
failure and separate dependency traversal failures. This is an assessment of
recorded stack frames, known recursive call paths and selected worker-thread
behavior. It does not certify arbitrary callbacks or external library internals.
No product implementation changed during the continuation.

## Historical local XML result

[`xml_mapping.c`](../src/xml_mapping.c) recursively alternates
`rotmos_xml_stream_object` and `rotmos_xml_stream_field_value`. The default
`max_depth` is 64. A finite, heap-allocated map with probe depth 62 parses and
cleans up on all three musl architectures tested. Probe depth 512 with the default
configuration returns `XML nesting exceeds max_depth` without terminating the
worker. These checks do not establish every default-configuration input.

Raising `max_depth` to 1,024 lets the same mapper enter enough native recursion
to exhaust a default worker stack: the depth-512 fixture fails on x86_64,
AArch64 and ARM32. The retained x86_64 debugger trace identifies mapper depth
327. The equivalent retained glibc test passes on its measured 8 MiB worker
stack. The failing mapper belongs to librotmos; this evidence does not identify
a libxml2 defect.

The delivered local correction uses an iterative reader-frame stack, preserving
incremental reads, configured depth semantics, field ordering, ownership and
error cleanup. Required regressions exercise unchanged default musl worker
stacks rather than increasing their size.

## Retained compiler measurements

The earlier investigation compiled Core and patched Kore with GCC 15.3.0 at
`-O0` and `-O3`, using `-fstack-usage` and `-fcallgraph-info=su`. The continuation
read those artifacts rather than repeating compilation of the entire product.

| Musl target | Largest frame, O0 | Largest frame, O3 | Largest estimated direct chain, O0 / O3 |
| --- | ---: | ---: | ---: |
| x86_64 | 13,440 B | 13,504 B | 37,200 / 38,128 B |
| AArch64 | 13,424 B | 13,488 B | 37,072 / 38,000 B |
| ARM32 hard-float | 13,424 B | 13,448 B | 36,712 / 37,760 B |

The largest frames belong to filesystem path operations, led by
`rotmos_fs_delete_conditional`; the largest estimated chains also traverse
filesystem operations. XML text commit/output and Kore file serving have
additional frames around 8 KiB. These are candidates for moving multi-KiB
scratch storage to the heap, but the selected filesystem tests did not reproduce
an overflow.

The x86_64 librotmos Lua C binding was also measured: largest frames are 1,776 B
at O0 and 1,792 B at O3. ARM Lua bindings were not included in these saved frame
measurements. Lua runtime work remains assigned separately to cpkt.

No frame in the recorded Core/Kore and x86_64 binding `.su` files is classified
as unbounded dynamic allocation. This is a compiler classification of individual
frames, not a bound on recursion. The chain estimates count a recursive cycle
once, omit external/indirect callees, and may combine mutually exclusive branches;
they are ranking evidence, not measured high-water marks or complete stack bounds.

## Known direct cycles checked

| Recorded cycle | Assessment |
| --- | --- |
| XML object/field mapping | Confirmed at this baseline; subsequently corrected with iterative reader frames. |
| WebDAV finish/error | `finalized` is set before hooks/submission. An error callback cannot repeatedly re-enter finish. The existing WebDAV runtime check passes. |
| Lua configuration `settings` / `settings_push` | Recursion enters the fixed deadline descriptor, which contains only scalar fields. Input cannot add recursively nested descriptor types. |
| Kore logging/message send | The send-to-log edge requires `worker == NULL`; that log path does not send another worker message. The direct graph cycle does not imply repeated recursion. |
| Kore HTTP response/header receive | Error responses set `CONN_CLOSE_EMPTY`, preventing another receive. Public adapter domains install the prebody route without native Kore redirect entries; managed requests yield through that handoff. No unbounded public-adapter cycle was established. Standalone Kore configurations are outside this conclusion. |
| Kore JSON lookup | The public lookup token array is bounded by `KORE_JSON_DEPTH_MAX`, which is 10 in this pin. This differs from unconstrained programmatic tree traversal below. |
| Filesystem directory traversal and XML generation | The inspected walkers already keep traversal frames on the heap. The existence of large individual scratch frames elsewhere is a separate budget concern. |

These conclusions apply to the inspected direct paths. Application callbacks
can add stack use or call back into APIs; direct compiler graphs do not prove
their behavior.

## Dependency results and handoff

LoneJSON 0.47.0 initialization of a finite 4,097-map chain fails on all three
musl targets before parsing. The continuation adds an `init` mode that directly
calls `lonejson_init`, bypassing librotmos schema conversion and XML. A 65-map
control initializes and cleans up normally. Parser depth limits do not constrain
this initialization path. The retained debugger trace identifies repeated
`lonejson.init_map_with_allocator.part` frames.

Kore's pristine snapshot
`c5fe59f521bdc1f2c486b5d8cc90227d72fcc628` contains recursive JSON serialization
and destruction. The continuation uses the patched Core artifacts and obtains:

| Programmatic tree operation | x86_64 Debug | AArch64 Release under QEMU | ARM32 Release under QEMU |
| --- | --- | --- | --- |
| Serialize depth 10, then free | Completes | Completes | Completes |
| Serialize depth 2,048 | Terminates during serialization | Terminates during serialization | Completes, 12,290 output bytes, then frees |
| Free depth 16,384 without serialization | Terminates | Terminates | Completes |

The ARM32 results do not establish safe traversal at arbitrary depth. Exact
failure thresholds were not measured. HTTP reachability of these tree depths
was not established. See the [Kore defect record](../KORE.md#json-tree-traversal-on-musl-workers).

Three local upstream CR drafts remain under ignored `stash/`:

- `CR-lonejson-schema-init-musl-stack.md`: confirmed native initialization failure.
- `CR-kore-json-tree-musl-stack.md`: confirmed traversal failures, with architecture limits.
- `CR-lonejson-lua-json-stack-reservation.md`: earlier decode/encode crash;
  binding cause remains provisional. No additional Lua runtime work was performed.

The reports include versions, reproduction details, expected behavior and
acceptance evidence. They have not been filed upstream. Other dependency Lua
schema/record recursion has not received a complete runtime characterization;
the retained provider measurements are not a provider-wide stack guarantee.

## Verification and evidence limits

Fresh x86_64 musl CTest execution passes six existing checks: `rotmos.xml`,
`rotmos.xml-mapped`, `rotmos.vectis-filesystem`, `rotmos.storage-filesystem`,
`rotmos.storage-filesystem-managed` and `rotmos.webdav-runtime`. The XML mapped
and standalone filesystem fixtures explicitly use default-attribute worker
threads. No thread stack was enlarged for the diagnostic probes either.

Nine small diagnostic cases were run per architecture against existing Core
archives. x86_64 and AArch64 match all nine expected observations. ARM32 matches
seven; its other two cases complete successfully where the initial test
expectations assumed termination. Those expectation mismatches are retained,
not reported as two additional product failures. The diagnostic CTest project
contains expected-failure observations and is not a product correctness gate.
Full raw output was inspected; a textual success marker alone would not prove
that subsequent cleanup completed.

Measured worker stack sizes are 133,120 B on x86_64, 133,104 B on AArch64 under
QEMU, and 133,840 B on ARM32 under QEMU. ARM execution is emulated evidence,
not physical-hardware validation. Native x86_64 used the existing Debug Core;
ARM used existing Release Core artifacts. Not every optimization/architecture
combination was executed.

The source harness, CMake characterization project, six frame reports, `.su`
classification summary, earlier backtraces, six-check Core log and three new
diagnostic logs are preserved in `stash/musl-stack-evidence/`. Existing build
outputs are under `build/stack-continuation/`. Further failure reproductions
were stopped after the production executive reported another platform notice;
two drafted deeper ARM32 cases were removed without execution.

This records the scoped assessment of the baseline frames and known direct
cycles. Local XML mapping and Kore traversal have since been corrected;
LoneJSON initialization and binding handoffs remain open. External library internals, arbitrary callback
chains, physical ARM hardware, and the separately owned Lua runtime remain
outside the established evidence. No release or complete product matrix was run.
