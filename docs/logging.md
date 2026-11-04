# Structured and file access logging

> Implementation reference under correction. The [extraction contract](server-api-spec.md) governs required behavior and scope; this document describes current code, not compatibility or an additional product requirement. See the [audit](server-api-audit.md) for known failures.


Include the optional `<rotmos/pslog.h>`. It uses the installed public libpslog
types; the core umbrella does not import them. Consumers of this header link
the matching pslog target explicitly, for example `rotmos::shared` with
`pslog::pslog_shared`, or both static variants. The SDK resolves the same pinned
provider used by the library; Kore's single-header logger implementation is
excluded.

Initialize `rotmos_access_log_config` and call
`rotmos_server_set_pslog(server, root, &access, error)` outside a run.
Registration copies the access rules and borrows both logger pointers. It
creates no logger, opens no file and invokes no sink. Invalid registration
preserves the previous configuration. Logger instances must remain alive
through every run and use sinks capable of native process callbacks. Lua
interop must enforce that capability through its public pslog descriptor.

The root receives backend runtime logs. A NULL root selects a library-owned JSON
logger during the run, preserving the ordinary INFO filter. Structured access
defaults to a `sub=http` child of the root. A non-NULL access logger is used
directly. The library destroys only its own default/derived instances; it never
destroys a borrowed logger. `logger_disabled` disables structured access alone.

The default access levels are TRACE for 100..399, WARN for 400..499 and ERROR
for 500..599. Up to 32 copied exact-status rules override those categories,
including DISABLED. Duplicate statuses and invalid levels/codes fail. Accepted
levels preserve Vectis's TRACE..PANIC and DISABLED declarations. The selected
logger's normal filter still applies, so ordinary successful requests are
filtered by an INFO root unless the caller selects a lower threshold.

The existing Kore/libpslog formatter supplies actual TCP `client_ip`, resolved
`real_ip`, method, path, HTTP version, final status, transmitted body `bytes`,
referer, user agent and nonempty `cid`. It does not log Cookie, Authorization,
other sensitive headers or bodies. Resolver values are retained request copies;
logging does not rerun application policy or truncate a valid 128-byte ID to
Kore's older scratch-buffer size. Final file/source/response completion supplies
the byte count, independently of the selected representation length. Observation
snapshots separately disclose commitment, abort, duration and detailed counts.
The formatter uses the retained TCP address after a downstream disconnect;
it never dereferences a released native connection to reconstruct identity.

`config.http.access_log_path` independently enables the existing file sink.
The parent opens it only during startup, workers queue bounded records, and the
parent drains through Kore's timer and flushes before worker mappings disappear.
An open failure is a returned startup failure. File records continue when
structured access or an individual structured status is disabled. The parent
skips contended worker buffers instead of waiting on an unfinished record; a
worker killed while holding that buffer can lose its unfinished records.

Worker-to-parent runtime messages retain initialized padding and explicit
lengths. Messages are capped at an 8192-byte prefix and malformed/oversized
incoming records are rejected before sink emission. The parent never needs an
arbitrary Lua state to emit them. A fresh run clears backend logger/resolver
pointers and recreates owned logger state.

The [public live fixture](../tests/policy-logs-runtime.c) covers copied rules,
override/disabled sinks, independent file output, client identity and long IDs,
status replacement, startup open failure, logger survival and server reuse.
It also verifies final address/count records after a client resets a paused
response connection, and runs against relocated static/shared SDKs. The
[backend fixture](../tests/log-backend.c) covers sender bounds, short/mismatched/
oversized IPC records and a locked unfinished access buffer without delaying
shutdown. The [policy API](policy.md) supplies admission and observations;
application loggers and metrics aggregation remain application-owned.
