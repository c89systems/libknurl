# WebDAV mounts

[`<knurl/webdav.h>`](../include/knurl/webdav.h) registers filesystem, read-only
and custom storage mounts through `knurl_router_webdav` and
`knurl_route_group_webdav`. The same [storage operation contract](storage.md)
provides conditional mutations, transactions, quotas, cancellation and recovery.
No Vectis, credential client or Lua runtime is required by this C facade.

Initialize `knurl_webdav_config`, set `path_prefix` and `auth`, then supply either
a borrowed `provider` or the filesystem `storage` configuration. A filesystem
mount uses its server router; declaring it starts no threads or filesystem work.
Strings and declarations are copied. A custom provider and callback userdata
remain borrowed through router destruction. Group prefixes are literal; the
registry owns one mount route, including its trailing-slash form.

Zero `auth_disabled` requires an auth callback. Zero `conceal_disabled` conceals a
denial as 404; disabling concealment selects 403. Auth decisions can allow with a
copied principal, deny, challenge (default 401) or redirect (default 302), with
bounded copied response fields and body. `knurl_webdav_request_principal` borrows
the root authorization's copied principal through the request lifetime. Visibility
and mutation-scope checks do not replace that identity. Application request
userdata remains available to the callbacks and later middleware.

The facade runs group/local auth and before hooks before DAV admission, then local/
group after hooks against the staged result before transport submission. Early
hook replies and auth/method rejection precede request-body delivery and automatic
100 Continue. Host auth callbacks run synchronously on the HTTP owner loop and
must return promptly without yielding or retaining borrowed callback fields.
Credentials, Lockd policy and packed-content ownership remain host concerns.

## Methods and bodies

OPTIONS advertises DAV class 1 and the provider's allowed methods. GET and HEAD
serve opened file snapshots with entity tags, conditional/range handling and
configured media/cache headers, sharing the [static reply policy](assets.md).
Collections do not become implicit GET indexes. Read-only mounts return 405/Allow
for mutations before receiving the body. LOCK/UNLOCK are not implemented or
advertised as protocol locks.

PUT begins a storage transaction at admission. Each callback offers at most one
bounded chunk; the transport retains it until the provider acknowledges a positive
prefix. The next delivery consumes that prefix once, then offers only remaining
bytes. End-of-body commits through PUT_FINISH. Request close revokes pending work
and closes/aborts the retained write resource after provider acknowledgement.
Filesystem transaction files are declared storage output, not hidden transport
spools. A zero-byte upload also follows begin/finish. New destinations produce 201;
replacement produces 204, using the provider's locked `created` outcome.

The current managed HTTP transport uses Content-Length for PUT and delivers those
bytes incrementally. Chunked managed PUT is unsupported; chunked body delivery
requires the separate raw takeover path described in [requests](requests.md). Non-PUT request payloads are discarded incrementally when admitted.
MKCOL rejects a nonempty body with 415. DELETE returns 204; MKCOL returns 201.
Storage quota failures preserve Vectis's 413 mapping, conditions/existing overwrite
conflicts return 412, and missing/tombstoned reads return 404.

COPY/MOVE decode Destination once, confine it to the same mount, validate an
optional HTTP(S) authority against Host (including default ports), and preserve
Overwrite T/F plus COPY depth 0/infinity. MOVE requires infinity when Depth is
explicit. Destination query/fragment text does not become a resource path. Encoded
separators, dot traversal, userinfo, foreign authorities and foreign mounts fail
before mutation. Successful new/replaced destinations produce 201/204. The
provider's explicit partial commit/recovery outcomes remain available even if
HTTP delivery is cancelled; cancellation does not imply rollback.

`operation_timeout_ms` zero selects 30000 ms, including queue/lock/policy wait.
`operation_timeout_disabled` explicitly removes that deadline. The Vectis
reference translator and Lua facade disable this additional deadline by default;
an explicit Lua timeout enables it unless explicitly disabled. The facade reports
an interrupted live operation as 504 for timeout, while storage retirement still
waits for native acknowledgement. Worker shutdown retains its bounded escalation
contract. Core `max_body_bytes` zero inherits the server's body policy.
Vectis translation and Lua's omitted body limit forward `max_file_bytes`; an
explicit Lua body limit overrides that translation, including zero inheritance.
WebDAV caps the selected body allowance at the server hard limit, admitting
smaller valid requests and returning 413 above the resulting allowance.

## Authorization inside mutations

Protected mutable providers must declare `KNURL_STORAGE_CAN_SCOPE_CHECK`.
`knurl_storage_operation_config.check_scope` is accepted only for DELETE/COPY/MOVE
and only with that capability. The provider checks every affected physical source
descendant, mapped destination, and overwritten destination descendant inside the
same mutation scope; source conditions and these decisions precede mutation.
A root-only preflight is insufficient for recursive changes.

`knurl_storage_operation_check_scope` invokes policy on the owner loop and denies
access after cancellation. Retained native work can query whether a checker exists,
but must marshal decisions to that loop. The filesystem provider retains its
mutation lock, publishes one bounded copied path pair through a waker, and waits
cooperatively on its native thread. It never passes the request into native work
or calls Lua there. The owner loop stays available for unrelated HTTP work.
Its traversal retains one frame per path depth rather than collecting a tree.

A denial returns `KNURL_STORAGE_DENIED` without mutating content. The DAV facade
stages the host's denial/challenge/redirect response and sends it after the native
operation acknowledges. A policy error also prevents mutation. Scope wakers and
operation context remain retained until native acknowledgement and registration
retirement, including request close during a decision.

## PROPFIND output

Depth 0 returns the resource; depth 1 also advances its immediate-child iterator.
Omitted/infinite depth produces the existing 403 `propfind-finite-depth` XML error;
invalid finite depth produces 400. Each visible child gets its own authorization
check. Denied/challenged/redirected children are omitted without applying their
response fields to the collection response; policy errors fail the exchange.

The XML source emits the root, one child at a time, and the closing multistatus.
It retains one bounded item's metadata and serializes URI-encoded hrefs, collection
kind and wide content lengths directly into caller output spans. It pauses while
a listing operation is pending and resumes through the public response producer.
No complete listing/document or undisclosed staging file is constructed. The
opened-file asset path uses bounded ordinary pread calls, which may block in the
filesystem. The generic [source API](responses.md) separately supports
native-reader execution; WebDAV file delivery does not select that mode.

The [HTTP fixture](../tests/webdav-runtime.c) drives real sockets with 128-byte
transport chunks. It covers auth/challenge/redirect, middleware and principal/
userdata ownership, read-only/packed mounts, validators/ranges/HEAD, incremental
PUT before end-of-body, empty/conditional uploads, disconnect cleanup, MKCOL,
finite-depth XML, hidden children, recursive/mapped/overwritten authorization,
request abort during native policy handoff, destination confinement and COPY/MOVE.
It also runs against relocated installed static/shared consumers. The [verification record](verification.md) covers the accumulated transport and
installed-consumer gate; it does not establish exhaustive fault combinations or
a slow-peer memory bound.
