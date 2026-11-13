# Read-only assets and static serving

[`<knurl/asset.h>`](https://github.com/c89systems/libknurl/blob/trunk/include/knurl/asset.h) supplies read-only opened assets,
providers and bounded source adapters. [`<knurl/static.h>`](https://github.com/c89systems/libknurl/blob/trunk/include/knurl/static.h)
registers files, directory roots and custom providers through the ordinary router.
Both headers are included by the umbrella and expose no filesystem, Kore or
Vectis layouts. Vectis retains pack manifest/payload ownership and supplies an
adapter through the public provider interface.

`knurl_asset_provider_new` copies a callback declaration without invoking it.
`knurl_asset_provider_open` accepts a normalized absolute resource path and
synchronously returns one new owned asset, or EOF for absence. Opening must
return promptly. An opened asset supplies copied metadata, explicit-offset reads
for a file, or an incremental immediate-child iterator for a directory.
`knurl_asset_stat` borrows the opened object's metadata. `knurl_asset_read`
never skips/collects preceding content. Listing names are literal directory
components; URL punctuation is encoded by the HTTP facade. Dot/traversal names,
slashes and control bytes are rejected. Request-path validation is separate.
`knurl_asset_list_next` returns one child
or EOF; a call returning AGAIN supplies no child. ETags/content types/dates and list
names have explicit finite bounds. Reads validate counts and treat premature EOF
as I/O failure. Terminal failures latch; close remains available.

Provider ownership persists while opened assets remain alive. Closing the provider
prevents new opens; its terminal callback waits for its last asset to close.
`knurl_asset_new` takes one close notification only on success.
`knurl_asset_memory` borrows bytes without making a full copy. Its byte owner must
remain valid through the asset's close notification. Provider/asset operations
are caller-serialized, and callbacks must not longjmp or recursively mutate their
own handle. Metadata strings are borrowed until close; a list result is borrowed
until the next list call or close. Returned source ownership prevents manual asset
read/close until source retirement.

`knurl_asset_source_take` takes an opened file only on success. It emits an exact
wide-counter range using bounded reads and does not read during construction.
Reset restarts that selected range without reopening the path. Source close,
including staged replacement or transport cancellation, closes the asset once.
An asset read returning AGAIN follows the normal source contract: HTTP output
pauses until an owner-loop response resume; application wake logic uses the
existing request/response loop APIs. It never starts a hidden blocking thread.
The separate asynchronous storage-operation/native-executor contract remains
required for WebDAV and long-running provider jobs.

`knurl_asset_filesystem_new` is inert. First open pins its root by walking each
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

`knurl_static_config_init` defaults to GET/HEAD and disables inferred directory
indexes. Configure `path` and `file_path` for `knurl_router_static_file`, `path`
and `root_dir` for `knurl_router_static_directory`, or `path` and a borrowed
`provider` for `knurl_router_static_assets`. Enabling indexes selects `index.html`
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

| Vectis declaration | Public knurl declaration |
| --- | --- |
| static file `path`, `file_path`, type and methods | `knurl_router_static_file` with the same `knurl_static_config` fields |
| static directory `path_prefix`, root, index and methods | `knurl_router_static_directory`, mapping `path_prefix` to `path` |
| borrowed embedded filesystem mount | `knurl_router_static_assets`; provider open delegates to packed lookup and creates a memory/directory asset |
| embedded root/index or not-found lookup fallback | Provider-owned lookup policy; a root file alias retains canonical slash redirects |

A single trailing slash is accepted only when the first ordinary route match is
a directory/asset mount. This exception does not admit dot segments, encoded
separators, controls, repeated decoding or ordinary regex routes. Filesystem and
provider path walks independently reject ambiguous paths. Slashless directory
URLs redirect with 308 after an index is found; Location encodes the decoded path
once and preserves the raw query. Files with an added slash return 404. Indexes
are resolved under the same confined root. No directory listing is exposed by a
static route.

Content type uses the same extension resolver for filesystem and provider mounts.
Defaults cover audio/video (MP3, MP4/M4A, FLAC, AAC, Ogg/Opus, WebM, QuickTime),
HLS/DASH playlists and segments, subtitles, modern images, fonts, archives,
office documents, ebooks and web/data formats. `.md`/`.markdown` remain
`text/markdown; charset=utf-8`; `.mp3` is `audio/mpeg` and `.mp4` is `video/mp4`.
The checked-in [catalogue](https://github.com/c89systems/libknurl/blob/trunk/src/mime.c) uses registered
[media types](https://www.iana.org/assignments/media-types/) where available and
conventional types otherwise. Extension inference never reads or sniffs content.
Compression suffixes alone do not set Content-Encoding or decompress payloads.
Negotiated sidecars follow the policy below.

Resolution precedence, for GET/HEAD and ranged responses alike:

1. Explicit route `content_type`, which forces one type for the entire mount.
2. Route `mime_types` extension overrides.
3. Instance `http.mime_types` extension overrides.
4. Nonempty asset/provider metadata (including explicit application/octet-stream).
5. Built-in filename-extension inference, then application/octet-stream.

Untyped packed entries now receive extension inference; explicit manifest types
remain authoritative unless configuration overrides them. Previously a packed
entry's type took precedence over the route's `content_type`; that field is now
an explicit override consistently for all static mounts.

C declarations use arrays of `knurl_mime_type`:

```c
static const knurl_mime_type instance_types[] = {
    {"custom", "application/vnd.example.resource"}
};
static const knurl_mime_type route_types[] = {
    {"ts", "text/plain; charset=utf-8"}
};
/* After the ordinary configuration initializers: */
server_config.http.mime_types = instance_types;
server_config.http.mime_type_count = 1;
static_config.mime_types = route_types;
static_config.mime_type_count = 1;
```

`knurl_server_content_type(server, filename)` resolves instance mappings and
built-ins without filesystem access; its result is borrowed until destruction.
`knurl_static_content_type(filename)` remains the built-in-only helper.
The latter, and a NULL-server lookup, return strings with static lifetime.

Lua uses dictionaries, forwarded to those same public C declarations:

```lua
local server = assert(knurl.server{
  port = 8080,
  http = {mime_types = {custom = 'application/vnd.example.resource'}},
})
assert(server:static_directory{
  path = '/files', root_dir = './public',
  mime_types = {ts = 'text/plain; charset=utf-8'},
})
assert(server:content_type('item.custom') == 'application/vnd.example.resource')
```

Strings and mapping arrays are copied at server creation or route registration;
there is no process-global registry or live mutation. Zero entries inherit the
next layer. Extensions are ASCII letters/digits or `+`, `-`, `_`, optionally
prefixed by one dot; matching is case-insensitive against the final filename
extension. `.MP3` and `mp3` are the same key. Duplicates within one declaration,
paths/compound suffixes, empty types and header controls fail with INVALID.
Lookups take filenames, not URLs with query/fragment components. Ambiguous
extensions use one default: `.ts` means MPEG transport stream; a source-code
mount can override it as above. Directory/group mounts support the same options.

Content-Type values follow [RFC 9110 media-type syntax](https://www.rfc-editor.org/rfc/rfc9110.html#section-8.3.1),
including its token and quoted-string parameter rules. The complete value is
validated: malformed/missing parameters, unclosed quotes, whitespace around
`=`, bare non-ASCII tokens, control characters, CR/LF/folding and comma-separated
type lists fail before registration or reply replacement. Wildcard media ranges
are rejected as representation types. Valid values are copied unchanged;
rejection does not attempt to repair malformed input by stripping characters.
Empty parameter separators are allowed by HTTP's grammar; a nonempty parameter
must contain both a token name and a token or quoted value with no whitespace
around `=`. Validation checks syntax, not whether a vendor type is registered.
Quoted parameter values allow escaped quotes/backslashes, delimiters and the
legacy opaque bytes permitted by HTTP; HTAB is accepted only in allowed whitespace
or quoted positions. C strings are NUL-terminated; Lua also rejects embedded NUL
using the string's explicit length. The same Core validator protects mappings,
explicit static/404 types, asset metadata, DAV types and staged reply/header types.

Assets default to no-cache and a `not found\n` 404 with
no-store; filesystem 404 replies are empty unless configured otherwise. Cache
and not-found policies are explicit. ETag/If-Match/If-None-Match checks validate
complete lists, including malformed tails, and distinguish weak/strong comparison.
Last-Modified uses the same opened metadata, formats English HTTP dates
independently of locale, and returns 304 for If-Modified-Since dates at or after
Last-Modified when If-None-Match is absent. Live HTTP handling uses
libcurl's public [date parser](https://curl.se/libcurl/c/curl_getdate.html); asset
construction does not initialize curl or perform network work. If-Unmodified-Since
returns 412 when the opened representation is newer, unless If-Match is present.
It runs before If-None-Match, If-Modified-Since and ranges; invalid dates,
repeated date fields and unavailable modification metadata are ignored.
All three HTTP-date forms are accepted. Invalid or repeated If-Range falls back
to the complete response. Only GET applies Range. Up to 16 `bytes` ranges support
suffix/open-ended forms. Unsatisfiable
members are dropped when another member is satisfiable. One selected member
uses 206 Content-Range; several use streamed multipart/byteranges with per-part
types/ranges and a checked complete length. Request order and overlaps are
preserved. Malformed, excessive or wholly unsatisfiable lists return 416;
unknown units are ignored. If-Range accepts strong ETags or exact matching dates
when the provider explicitly sets `last_modified_strong`. Filesystem timestamps
alone do not assert that strength.
HEAD retains representation length without reading content. Counters remain wide
on armhf, including ranges beyond 4 GiB.

Static configuration enables negotiated compression by default. Set
`compression_disabled=true` to disable it, or `precompressed_disabled=true` to
disable sidecar lookup while retaining dynamic gzip. The same fields exist in C
and Lua. Missing Accept-Encoding selects identity. Up to 32 empty list members
are ignored across all Accept-Encoding fields; a larger count is rejected.
Explicit weights, wildcard and identity exclusions determine selection; equal
quality prefers Brotli sidecar, gzip sidecar, dynamic gzip, then identity.
Malformed qvalues fail; no acceptable available representation returns 406.
Vary includes Accept-Encoding without discarding other Vary fields during
submission, including on HEAD and 304. Negotiated Content-Encoding describes
successful asset replies and 304 metadata; plain error replies and pretty HTML
errors do not inherit it. Range selects identity, returning 406 if identity
is excluded; Cache-Control no-transform suppresses transformations.

Sidecars `original.br` and `original.gz` are opened through the same confined
provider. They must be regular files at least as fresh as the original. Filesystem
providers supply precise modification metadata. Custom providers can set
`has_modification_time`, `modification_seconds` and `modification_nanoseconds`;
otherwise both Last-Modified values must explicitly assert strength. Missing or
stale sidecars fall back to another acceptable representation. All unused opened
assets close; the selected asset stays pinned through delivery. MIME inference
uses the original path/type, encoding has a distinct ETag, and sidecar length is
known. A sidecar extension served directly still does not imply Content-Encoding.

Dynamic gzip uses bounded input/output chunks from the pinned zlib dependency,
with deterministic headers, reset and normal source ownership. It never reads
the full asset, compresses to determine its length, or stages a temporary file.
Eligible types are text/*, JSON/+json, XML/+xml, JavaScript and WASM. Empty assets
and already compressed binary types skip dynamic compression. Its validator is
distinct and weak; compressed Content-Length is unknown. HEAD selects normally
but never reads/compresses content. Brotli generation is not provided. Replacing
a static payload in an after-hook retires its source and drops automatic
representation encoding/validator/range metadata.

The [asset fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/asset.c) covers ownership transfer, provider retention,
source pause/reset, invalid counts, premature EOF, reentry, path/symlink/special-
file confinement, opened-file/root replacement and incremental listing. It runs
on a default-stack pthread. The [wire fixture](https://github.com/c89systems/libknurl/blob/trunk/tests/static-runtime.c) covers
filesystem and packed responses, conditions/ranges, HEAD, redirects/query encoding,
trailing-slash precedence, method denial before 100 Continue, group registration,
source replacement and a range beyond 4 GiB. Both also run against relocated
static/shared SDKs.
