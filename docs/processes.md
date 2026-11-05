# Process lifecycle and observations

> Implementation reference. The [extraction contract](server-api-spec.md) governs required behavior and scope. See the [audit](server-api-audit.md) and [verification](verification.md) for evidence and limits.


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
backend and logger teardown, with a detached stopped/failed server and a borrowed
process snapshot. It may destroy that fork's server and host state; Core performs
no later access to either. The original application process never receives this
terminal worker callback. The parent fork's host cleanup belongs after run returns.
Terminal cleanup must not start new work.
The callback returns its child exit status. A returned shutdown/exit
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
ACME persistence uses this primitive for its existing activation/failure ordering.

The [public fixture](../tests/process-runtime.c) covers recovery, unexpected
zero exit, termination, returned startup failures, startup cancellation, failed
exit callbacks and shutdown abandonment. Its
[private extension](../tests/process-backend.c) blocks a child to verify operation
budgets during startup, serving and shutdown, and verifies budget cancellation.
The [auxiliary fixture](../tests/auxiliary-runtime.c) proves loop/teardown hooks
for HTTP, key-manager and ACME roles, key-manager restart/terminate policy, and
existing account-key loading against a local unavailable directory endpoint.
Local-CA issuance/persistence is exercised separately by the ACME runtime and Lua
fixtures; those are not claims about a remote CA.

The [host adapter](../tests/vectis-host.c) maps shared readiness and existing
READY/control providers, the baseline 100 ms readiness/60 s auth-maintenance
timers and fork-exit forwarding. Its [runtime case](../tests/vectis-host-runtime.c)
keeps outer supervision and control framing in the host and covers readiness,
STOP/malformed/EOF shutdown, app/server reclamation and failed cleanup after
reclamation. The accumulated failure-unwind gate remains tracked in the
[audit](server-api-audit.md).

The worker completion marker follows the terminal callback's return. A callback
that hangs after reclaiming its app is still escalated and reported as abandoned;
native teardown alone does not acknowledge completion of host cleanup.
