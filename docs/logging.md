# Structured and file access logging

`<knurl/log.h>` exposes `knurl_server_set_log_callback()` for a native callback
registered outside `run()`. Kore runtime diagnostics and structured HTTP access
records reach the callback synchronously on the originating process's owner
thread. The callback borrows the record and its fields for the duration of the
call. Access records carry typed request metadata, and handler and worker
failures can carry a `knurl_error`. Sink latency is part of the emitting thread's
work, including the Kore event loop for HTTP access records.

Register a NULL callback to select the libpslog adapter. A native callback also
honors configured access levels, per-status rules, and the structured access
enablement policy.

## libpslog adapter

Include the optional `<knurl/pslog.h>`. It uses the installed public libpslog
types; the core umbrella does not import them. Consumers of this header link
the matching pslog target explicitly, for example `knurl::shared` with
`pslog::pslog_shared`, or both static variants. The SDK resolves the same pinned
provider used by the library; Kore's single-header logger implementation is
excluded.

Initialize `knurl_access_log_config` and call
`knurl_server_set_pslog(server, root, &access, error)` outside a run.
Registration copies the access rules and borrows both logger pointers. It
creates no logger, opens no file and invokes no sink. Invalid registration
preserves the previous configuration. Logger instances must remain alive
through every run. Each worker inherits the logger configuration. Keep the
logger idle at fork and choose a sink suitable for use by the emitting
processes. The libpslog adapter passes borrowed fields to the regular logging
API so libpslog controls formatting and output chunking.

A host can create one root pslog instance and pass it to this registration and
its other native components. Derived context loggers share its sink. Retain the
host-owned root until the server and all borrowing native work have retired;
component shutdown does not destroy it. Lua programs can register a native-sink
pslog logger with their server. See [Lua configuration](lua-config.md).

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

Structured records supply actual TCP `client_ip`, resolved
`real_ip`, method, path, HTTP version, final status, transmitted body `bytes`,
referer, user agent and nonempty `cid`. It does not log Cookie, Authorization,
other sensitive headers or bodies. Resolver values are retained request copies;
logging does not rerun application policy or truncate a valid 128-byte ID to
Kore's older scratch-buffer size. Final file/source/response completion supplies
the byte count, independently of the selected representation length. Observation
snapshots separately disclose commitment, abort, duration and detailed counts.
The formatter uses the retained TCP address after a downstream disconnect;
it never dereferences a released native connection to reconstruct identity.

`config.http.access_log_path` independently enables combined-format file access
logs. One parent-owned writer handles all listeners and workers. Workers queue
bounded records; the parent drains them through Kore's timer and once more before
worker mappings disappear. File records continue when structured access or an
individual structured status is disabled. The parent skips contended worker
buffers held by a worker. Workers drop the file record when their buffer is full
or contended.

File rotation is enabled by default, with these `config.http` settings:

| Setting | Default | Meaning |
| --- | --- | --- |
| `access_log_max_bytes` | 10485760 (10 MiB) | Rotate before appending a record that would exceed this threshold to a nonempty file. Zero inherits the default. |
| `access_log_file_count` | 3 | Total files, including the active file. Zero inherits three; one truncates the active file at rotation boundaries. |
| `access_log_rotation_disabled` | false / 0 | Explicitly select unbounded append without rotation. |

With three files, rotation removes `access.log.2`, moves `.1` to `.2`, moves
`access.log` to `.1` and opens a fresh active file. Records are never split across
files: an oversized record is accepted into an empty file, which rotates before
the next record. Existing logs are appended across runs, with their size counted
toward the threshold. New files use mode 0644; rotation retains the active file's
permission bits, subject to the process umask. The threshold must fit a positive
64-bit signed file offset, including on 32-bit targets.

Each instance must exclusively own its configured regular-file path and numbered
backups. Final-component symlinks and nonregular files are rejected without
waiting on a FIFO. External writers or external rotation while the instance runs
are unsupported. Lowering the retention count does not remove backups outside the
newly configured range.

Rotation and append failures, including ENOSPC, leave HTTP serving. The parent
reports an error through the root logger, drops the file record it cannot accept
and retries on the next record. A repeated identical error is suppressed until
output succeeds or the error changes. A failed rename chain resumes at its failed
step, without repeatedly evicting history; it does not bypass rotation to grow the
active file. A partial failed append is rolled back when the filesystem permits.
There is no replay queue or durability/fsync guarantee. Startup ENOSPC is also
reported and retried when records arrive; other startup open failures remain
returned errors. Diagnostics require a functioning root log sink, so an independent
sink is useful when the access-log filesystem is full.

Runtime diagnostics use their originating process's logger. The parent reports
failures it observes during supervision, such as unexpected worker exit or a
missed deadline. A fresh run clears backend callbacks and recreates owned logger
state.

The [public live fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/policy-logs-runtime.c) covers copied rules,
override/disabled sinks, independent file output, client identity and long IDs,
status replacement, startup open failure, logger survival and server reuse.
It also verifies final address/count records after a client resets a paused
response connection, and runs against relocated static/shared SDKs. The
[backend fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/log-backend.c) covers complete long diagnostics, a locked unfinished file access buffer,
origin-thread callback delivery and borrowed access fields. The [policy API](policy.md) supplies admission and observations;
application loggers and metrics aggregation remain application-owned.
The [file fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/access-log-file.c) checks retention, exact thresholds,
oversized records, restart append, permissions, truncate/disable options, failed
rotation recovery, partial writes, ENOSPC and nonregular paths. Its two-worker,
two-listener wire case verifies continued HTTP delivery, error diagnostics,
recovery and parent descriptor cleanup, including startup ENOSPC.
