# Input validation and escaping

Include `<knurl/input.h>` (also included by `<knurl/knurl.h>`). These C helpers
validate explicit byte spans without trimming, repairing or normalizing them.
Embedded NUL is invalid. Application rules remain explicit; there is no universal
sanitized string.

| Operation | Accepted input |
| --- | --- |
| `knurl_input_long` | Optional minus and decimal digits, within native C long and inclusive supplied bounds. |
| `knurl_input_u64` | Unsigned decimal with exact portable bounds. |
| `knurl_input_boolean` | Exactly `true`, `false`, `1`, `0`. |
| `knurl_input_text` | Valid UTF-8 within inclusive byte lengths; optional rejection of C0/C1 controls. |
| `knurl_input_allowlist` | Every byte belongs to the supplied allowed-byte string. |
| `knurl_input_choice` | Exact membership in an array of byte spans. |
| `knurl_input_email` | ASCII dot-atom mailbox and DNS-label domain. Local part at most 64 bytes, domain 253, total 254. No display names, quoted local parts, Unicode/IDNA conversion or deliverability check. |
| `knurl_input_http_url` | Explicit HTTP/HTTPS URL with host and valid port, without credentials, whitespace or controls. Uses libcurl's URL parser; performs no network/DNS operation. |
| `knurl_input_filename` | One 1..255-byte ASCII component using letters, digits, dot, underscore and hyphen, excluding `.` and `..`. Filesystem confinement remains separate. |
| `knurl_input_html_escape` | UTF-8 escaped for HTML text or quoted attributes: ampersand, angle brackets, double and single quotes. |

Operations return status and optional bounded diagnostics. Numeric failures leave
the output unchanged. INVALID means syntax or declared bounds failed; OVERFLOW
means the numeric representation cannot hold the value. HTML escaping requires
an initialized empty `knurl_owned_bytes`; release successful output with
`knurl_owned_bytes_cleanup`. Its zero output limit selects 1 MiB. No helper
provides SQL, shell, JavaScript, CSS or URL escaping. JSON/XML codecs escape their
own output contexts.

Requests expose `query_long`, `query_u64`, `query_boolean` and matching `param_*`
receivers. Absent input returns EOF, invalid input returns INVALID, and duplicate
query names return INVALID even when their values agree. Defaults are explicit
application decisions. Ordinary `query_first` retains first-value semantics;
choose the typed helper when duplicates must fail. See [requests](requests.md)
for decoding and body lifetimes. Archived Lua wrappers are deferred until
Luthrum; see [runtime direction](runtime-direction.md).
