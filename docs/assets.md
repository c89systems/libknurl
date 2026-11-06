# Read-only assets and static serving

[`<rotmos/asset.h>`](../include/rotmos/asset.h) supplies read-only opened assets,
providers and bounded source adapters. [`<rotmos/static.h>`](../include/rotmos/static.h)
registers files, directory roots and custom providers through the ordinary router.
Both headers are included by the umbrella and expose no filesystem, Kore or
Vectis layouts. Vectis retains pack manifest/payload ownership and supplies an
adapter through the public provider interface.

`rotmos_asset_provider_new` copies a callback declaration without invoking it.
`rotmos_asset_provider_open` accepts a normalized absolute resource path and
synchronously returns one new owned asset, or EOF for absence. Opening must
return promptly. An opened asset supplies copied metadata, explicit-offset reads
for a file, or an incremental immediate-child iterator for a directory.
`rotmos_asset_stat` borrows the opened object's metadata. `rotmos_asset_read`
never skips/collects preceding content. Listing names are literal directory
components; URL punctuation is encoded by the HTTP facade. Dot/traversal names,
slashes and control bytes are rejected. Request-path validation is separate.
`rotmos_asset_list_next` returns one child
or EOF; a call returning AGAIN supplies no child. ETags/content types/dates and list
names have explicit finite bounds. Reads validate counts and treat premature EOF
as I/O failure. Terminal failures latch; close remains available.

Provider ownership persists while opened assets remain alive. Closing the provider
prevents new opens; its terminal callback waits for its last asset to close.
`rotmos_asset_new` takes one close notification only on success.
`rotmos_asset_memory` borrows bytes without making a full copy. Its byte owner must
remain valid through the asset's close notification. Provider/asset operations
are caller-serialized, and callbacks must not longjmp or recursively mutate their
own handle. Metadata strings are borrowed until close; a list result is borrowed
until the next list call or close. Returned source ownership prevents manual asset
read/close until source retirement.

`rotmos_asset_source_take` takes an opened file only on success. It emits an exact
wide-counter range using bounded reads and does not read during construction.
Reset restarts that selected range without reopening the path. Source close,
including staged replacement or transport cancellation, closes the asset once.
An asset read returning AGAIN follows the normal source contract: HTTP output
pauses until an owner-loop response resume; application wake logic uses the
existing request/response loop APIs. It never starts a hidden blocking thread.
The separate asynchronous storage-operation/native-executor contract remains
required for WebDAV and long-running provider jobs.

`rotmos_asset_filesystem_new` is inert. First open pins its root by walking each
component with `O_NOFOLLOW`; later requests resolve relative to that descriptor.
Every served entry is opened with no symlink following and inspected with fstat.
Special files are rejected, with nonblocking open preventing FIFO waits. Metadata
and delivered bytes refer to the same opened file, so replacing its pathname does
not switch an active response to another object. The file's contents are not an
OS snapshot: uncoordinated in-place writes may change them, and truncation fails
the read. Metadata ETags are explicitly weak. Strong content validators may be
provided by an immutable packed provider. Relative roots resolve at first open.
Directory listing retains an iterator rather than a whole child list and yields
AGAIN after a bounded run of skipped entries. Regular filesystem syscalls can
still block in the OS; this surface does not promise real-time disk latency.

`rotmos_static_config_init` defaults to GET/HEAD and disables inferred directory
indexes. Configure `path` and `file_path` for `rotmos_router_static_file`, `path`
and `root_dir` for `rotmos_router_static_directory`, or `path` and a borrowed
`provider` for `rotmos_router_static_assets`. Enabling indexes selects `index.html`
unless `index_file` is supplied. Strings/options are copied during registration.
Custom providers and hook userdata remain borrowed through router destruction.
The Lua facade accepts host providers through public Lua interop values for
`static_assets` and `webdav` declarations. Each value pins its optional Lua owner,
and copied route declarations retain it through server close. The binding does
not close borrowed providers. Packed lookup stays with the host, while these
mounts use the same C static/storage/WebDAV implementation. See the
[host-provider workflow](lua.md#host-providers).
Literal route groups provide the corresponding convenience registrations.
Filesystem single-file registration preserves Vectis's configured GET/HEAD masks,
so a separate POST route can use the same literal path. Directory/asset mounts
preserve registration order and return 405/Allow for denied methods before body
consumption or 100 Continue. Ordinary auth/before/after hooks apply to serving.

The Vectis adapter maps declarations directly:

| Vectis declaration | Public rotmos declaration |
| --- | --- |
| static file `path`, `file_path`, type and methods | `rotmos_router_static_file` with the same `rotmos_static_config` fields |
| static directory `path_prefix`, root, index and methods | `rotmos_router_static_directory`, mapping `path_prefix` to `path` |
| borrowed embedded filesystem mount | `rotmos_router_static_assets`; provider open delegates to packed lookup and creates a memory/directory asset |
| embedded root/index or not-found lookup fallback | Provider-owned lookup policy; a root file alias retains canonical slash redirects |

A single trailing slash is accepted only when the first ordinary route match is
a directory/asset mount. This exception does not admit dot segments, encoded
separators, controls, repeated decoding or ordinary regex routes. Filesystem and
provider path walks independently reject ambiguous paths. Slashless directory
URLs redirect with 308 after an index is found; Location encodes the decoded path
once and preserves the raw query. Files with an added slash return 404. Indexes
are resolved under the same confined root. No directory listing is exposed by a
static route.

Content type follows Vectis's extension mapping, with explicit overrides for
filesystem routes. Packed entries retain manifest content types before their
configured fallback, defaulting to application/octet-stream without an inferred
extension type. Assets default to no-cache and a `not found\n` 404 with
no-store; filesystem 404 replies are empty unless configured otherwise. Cache
and not-found policies are explicit. ETag/If-Match/If-None-Match checks validate
complete lists, including malformed tails, and distinguish weak/strong comparison.
Last-Modified uses the same opened metadata, formats English HTTP dates
independently of locale, and preserves the existing equal-timestamp
If-Modified-Since response when If-None-Match is absent. Live HTTP handling uses
libcurl's public [date parser](https://curl.se/libcurl/c/curl_getdate.html); asset
construction does not initialize curl or perform network work. Only GET applies
Range. Preserve one `bytes` range, suffix/open-ended forms,
If-Range's strong validator rule, 206 Content-Range and 416 unsatisfiable results.
Multiple/malformed byte ranges are unsatisfiable; unknown range units are ignored.
HEAD retains representation length without reading content. Counters remain wide
on armhf, including ranges beyond 4 GiB.

The [asset fixture](../tests/asset.c) covers ownership transfer, provider retention,
source pause/reset, invalid counts, premature EOF, reentry, path/symlink/special-
file confinement, opened-file/root replacement and incremental listing. It runs
on a default-stack pthread. The [wire fixture](../tests/static-runtime.c) covers
filesystem and packed responses, conditions/ranges, HEAD, redirects/query encoding,
trailing-slash precedence, method denial before 100 Continue, group registration,
source replacement and a range beyond 4 GiB. Both also run against relocated
static/shared SDKs.
