# Process lifecycle and observations

> Implementation reference under correction. The [extraction contract](server-api-spec.md) governs required behavior and scope; this document describes current code, not compatibility or an additional product requirement. See the [audit](server-api-audit.md) for known failures.


`rotmos_server_run` keeps the runtime parent on its caller thread and forks the
backend's declared children. The parent must remain thread-quiescent while
replacement forks are possible. Initialize process-local bindings in
`on_process_start`; create HTTP worker clients/resources in `on_worker_start`.
The process context distinguishes parent, HTTP, key-manager and ACME roles.
HTTP worker indices remain stable across replacement PIDs.

The parent receives `on_worker_event` for acknowledged start, unexpected exit,
acknowledged restart and deadline escalation. The event is a borrowed value valid
for the callback; copy it to retain it. It includes role/PID/index, restart state,
actual exit status or signal, an independent `expected` flag, abandonment and a
bounded error snapshot. Expected means the parent was stopping; inspect the
error separately. These observations run on the parent thread, including child
startup observations before the parent-ready callback.

RESTART replaces an unexpected HTTP or auxiliary child. TERMINATE stops the
runtime. Unexpected exit zero also follows that policy. Replacement startup
failure stops the run with its diagnostic. A stop already requested by a worker
or parent prevents replacement. Only declared child PIDs are reaped; unrelated
children remain owned by the host.

Returned process/HTTP-start errors preserve their public status and diagnostic.
They prevent readiness and run stopping/exit cleanup for the initialization that
began. Stopping can precede the HTTP start hook after a process-start failure;
hosts must track partial initialization. HTTP work drains while cancellation and
completion delivery remain available. The worker exit callback runs after
backend teardown and returns its child exit status. A returned shutdown/exit
failure remains observable as failed cleanup.

SIGKILL, a crash or a child which fails to finish teardown is reported as
abandonment. Parent escalation does not claim that the child's destructors ran.
The run result's `escalated` flag records forced deadline escalation, including a
recovered operation timeout. `has_failure` guards a copied `last_failure`
snapshot. `recovered_failures` increments after a replacement acknowledges
initialization. A recovered failure followed by an ordinary requested stop
returns that normal reason while retaining the failure/recovery evidence.

The private Kore adapter supplies a dedicated child-operation budget for
auxiliary work. Arming or clearing it wakes the parent through owned IPC; it
does not poll an external process table. The parent constrains its event wait,
escalates an expired child, reaps it and applies death policy. It also enforces
the operation budget during shutdown. The budget uses atomic 32-bit publication
and a wrap-safe monotonic deadline across the supported 32/64-bit targets.
ACME PERSIST will use this primitive; the ACME activation path remains pending.

The [public fixture](../tests/process-runtime.c) covers recovery, unexpected
zero exit, termination, returned startup failures, startup cancellation, failed
exit callbacks and shutdown abandonment. Its
[private extension](../tests/process-backend.c) blocks a child to verify operation
budgets during startup, serving and shutdown, and verifies budget cancellation.
The [auxiliary fixture](../tests/auxiliary-runtime.c) proves loop/teardown hooks
for HTTP, key-manager and ACME roles, key-manager restart/terminate policy, and
existing account-key loading against a local unavailable directory endpoint.
It provides no issuance, persistence or remote-CA evidence.

Arbitrary fatal/allocation failures still need the final failure-unwind checks.
Candidate publication, ACME callbacks/activation, TLS identity observations and
serving-gate acknowledgements remain required by the
[server specification](server-api-spec.md).
