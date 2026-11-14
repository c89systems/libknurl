# Route ACL implementation contract

ACLs are reusable, reference-counted declarations with public C free functions
and receiver methods, plus a Lua facade. Construction copies and validates
configuration without DNS, files, threads or server initialization. Explicit
refresh prepares standalone ACLs; server startup prepares every attached ACL
before serving. Routes and groups retain attached ACLs. ABI remains 0 while
Knurl is unreleased.

## Rules and identity

Each ACL has `allow` and `deny` rules and one default `policy` (`allow` by
default). Each rule set contains numeric addresses/CIDRs, configured hostnames,
two-letter uppercase country codes and an explicit unknown-country switch.
Within a set, entries are alternatives. Deny matches take precedence over allow
matches; otherwise the default policy applies. Country and address predicates
are alternatives, not implicit intersecting filters. Separate group and route
ACLs compose by intersection: every attached ACL must allow the request.

IPv4, IPv6, /0 and full-length prefixes are supported. IPv4-mapped IPv6 is
normalized to IPv4; malformed addresses, masks and country codes are errors.
Configured hostnames resolve forward to exact address sets; no reverse DNS,
wildcard hostname matching, or DNS resolution of request input is performed.
The subject is the effective client address by default, with an explicit
`use_peer` option for the actual TCP peer. Both identities remain observable.

Country rules use the standalone MMDB reader. A copied database path opens an
owned reader during preparation, using the configured MMDB heap limit (zero
selects 512 KiB). C callers may instead borrow a country provider, retaining it
through ACL retirement. Unknown means a valid absent country/record; database
errors never become unknown. Country data is looked up only when country rules
need it, and country facts remain independent of admission decisions.

## DNS lifecycle

Zero refresh interval resolves once at preparation and never again for that ACL
lifetime. A positive interval opts into refresh. A bounded forward resolver
callback is available to C hosts and deterministic tests; the default uses
system getaddrinfo for A/AAAA results, deduplicating normalized addresses.
The default per-host capacity is 64, with an explicit bound of at most 1024;
overflow fails the whole update rather than silently dropping addresses.
Initial resolution failure prevents startup. Snapshots publish atomically only
after every hostname succeeds; no partially updated allow/deny sets are visible.

Refresh attempts are debounced by monotonic time and only one runs per ACL.
Checks never resolve names, spawn work or wait for a DNS operation's mutex.
During a refresh the last successful snapshot remains usable. Failure or a
background deadline marks the ACL unavailable, so stale allow/deny answers
cannot silently admit clients. Successful resolution restores service. DNS-free
and startup-only ACLs create no background resources.

HTTP workers use one bounded native executor for dynamic ACLs, started only
after their final fork, with one timer per distinct ACL. Busy ACLs cannot queue
overlapping jobs. Capacity pressure fails closed and retries on a bounded timer.
The default background deadline is 10 seconds. System resolver cancellation is
cooperative; existing executor retirement and worker shutdown deadlines retain
all referenced state until work finishes or the worker exits. Standalone users
schedule explicit refresh calls themselves. Fork requires quiescent ACL users;
configuration and preparation start no threads in the parent.

## Forwarding trust

Trusted proxies accept numeric IPv4/IPv6 addresses and CIDRs. `*` trusts only
the immediate TCP peer, never arbitrary addresses inside a supplied chain.
Chains are walked right to left; only explicitly trusted numeric hops are
skipped. The first untrusted hop is the effective client. Thus `*` alone accepts
the nearest supplied hop, not an attacker-selected leftmost address.

Forwarding and single-address header names are configurable. Defaults preserve
X-Forwarded-For followed by X-Real-IP, with an explicit chain-hop bound. A
configurable format also accepts RFC 7239 Forwarded `for` parameters. Duplicate,
malformed or overlong chains cannot select an unvalidated client; they fall
back to the actual peer. Forwarding is ignored for untrusted TCP peers. No
forwarded identity changes connection admission, which always sees the peer.
Effective-identity ACLs reject unusable forwarding data from trusted peers rather
than admitting through the proxy's peer address. Explicit peer ACLs and validated
host identity overrides keep their declared semantics.

## Enforcement and API parity

One common route gate applies after route selection and before route preflight,
body delivery, upload/file opening, proxy target/rewrite callbacks or upstream
allocation, and WebSocket upgrades. It applies to ordinary, raw-header, JSON,
XML, REST, static/assets, uploads, WebDAV, proxy and WebSocket routes. A denied
route returns 403 and never falls through to another matching route. Unavailable
ACLs return 503; neither path consumes an advertised body or sends 100 Continue.
Global application admission remains available and precedes route dispatch.

C route declarations and built-in facades accept ACL attachment, with common
route/group setters for reusable declarations. Mutation is rejected while the
router is active. Lua accepts a reusable ACL object or an inline declaration,
preserves ownership through route/server retirement and exposes standalone
check/refresh/close. All matching and DNS lifecycle behavior belongs to Core;
the binding must not implement a second policy engine.

## Verification

Tests assert precedence, empty/default policies, group intersection, numeric
and CIDR boundaries, mapped addresses, hostname changes and debounce, atomic
refresh failure/recovery, bounded resolver results, country/unknown/error
behavior, ownership and concurrent checks. Wire tests prove every route family
rejects before opening resources, reading bodies, sending 100 Continue or
connecting upstream; verify custom forwarding headers, IPv4/IPv6 trust, wildcard
immediate-peer behavior, malformed/duplicate chains and hop limits. Lua tests
exercise standalone and inline/reused route declarations with the installed SDK.

Run strict C89/C++ header and seven-target builds, GNU/musl native suites,
constrained Debug stacks, ARM execution, Valgrind, SDK/package boundaries,
formatting and documentation checks. Downloads and real MMDB data stay outside
shipped products. No HTTP/2 or HTTP/3 listener work is included.
