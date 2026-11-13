# Input validation and escaping

Include `<knurl/input.h>` (also included by `<knurl/knurl.h>`) or use
`knurl.input` in Lua. These helpers validate explicit byte spans without trimming,
repairing or normalizing them. Embedded NUL is invalid. Application rules remain
explicit; there is no universal sanitized string.

| C operation | Lua operation | Accepted input |
| --- | --- | --- |
| `knurl_input_long` | `input.parse_long(value, min, max)` | Optional minus and decimal digits, within native C long and inclusive supplied bounds. |
| `knurl_input_u64` | `input.parse_u64(value, min, max)` | Unsigned decimal with exact portable bounds; Lua uses integers or `knurl.count` without rounding. |
| `knurl_input_boolean` | `input.boolean(value)` | Exactly `true`, `false`, `1`, `0`. Lua returns a boolean, including successful false. |
| `knurl_input_text` | `input.text(value, min, max, reject_controls)` | Valid UTF-8 within inclusive byte lengths; optional rejection of C0/C1 controls. |
| `knurl_input_allowlist` | `input.allowlist(value, allowed, min, max)` | Every byte belongs to the supplied allowed-byte string. |
| `knurl_input_choice` | `input.choice(value, choices)` | Exact membership in an array of spans/strings. |
| `knurl_input_email` | `input.email(value)` | ASCII dot-atom mailbox and DNS-label domain. Local part at most 64 bytes, domain 253, total 254. No display names, quoted local parts, Unicode/IDNA conversion or deliverability check. |
| `knurl_input_http_url` | `input.http_url(value)` | Explicit HTTP/HTTPS URL with host and valid port, without credentials, whitespace or controls. Uses libcurl's URL parser; performs no network/DNS operation. |
| `knurl_input_filename` | `input.filename(value)` | One 1..255-byte ASCII component using letters, digits, dot, underscore and hyphen, excluding `.` and `..`. Filesystem confinement remains separate. |
| `knurl_input_html_escape` | `input.html_escape(value, max_bytes)` | UTF-8 escaped for HTML text or quoted attributes: `&`, `<`, `>`, double and single quotes. |

C operations return status and optional bounded diagnostics. Numeric failures
leave the output unchanged. INVALID means syntax or declared bounds failed;
OVERFLOW means the numeric representation cannot hold the value. HTML escaping
requires an initialized empty `knurl_owned_bytes`; release successful output with
`knurl_owned_bytes_cleanup`. The zero HTML output limit selects 1 MiB. No helper
provides SQL, shell, JavaScript, CSS or URL escaping. JSON/XML codecs escape their
own output contexts.

Lua validators return true or nil/error, parsers return the parsed value or
nil/error, and escaping returns a string or nil/error. Omitted numeric bounds
select the C long/u64 range. Omitted text/allowlist byte bounds select 0..1 MiB;
an explicit maximum of zero permits only empty input. Omitted control rejection
is false. Strings are required; numbers are not silently converted to input text.

Requests expose `query_long`, `query_u64`, `query_boolean` and matching `param_*`
receivers in C and Lua. Absent input returns EOF, invalid input returns INVALID,
and duplicate query names return INVALID even when their values agree. Defaults
are explicit application decisions. Ordinary `query_first`/Lua `query` retains
its first-value semantics; choose the typed helper when duplicates must fail.

```lua
local limit, err = req.query_long('limit', 1, 100)
if limit == nil then
  if err.status == knurl.status.EOF then limit = 20
  else return {status=400, body='invalid limit'} end
end
local enabled, why = req.query_boolean('enabled')
if enabled == nil and why.status ~= knurl.status.EOF then
  return {status=400, body='invalid enabled value'}
end
local label, failure = knurl.input.html_escape(req.query('label') or '', 4096)
if not label then return {status=400, body=failure.message} end
```

These operations are shared by the C and Lua surfaces; Lua adds no independent
validation engine. See [requests](requests.md) for decoding and body lifetimes.
