# Route ACLs

Include `<knurl/acl.h>` or `<knurl/knurl.h>`. The standalone C functions and
receiver shell work without a server. Lua uses the same Core policy engine.

Each ACL has `allow` and `deny` rule sets and one fallback `policy`. Deny matches
win, then allow matches, then the policy applies. Zero/default policy is allow;
`deny` with an empty allow set blocks everyone. Entries within a set are
alternatives. To require both a permitted network and country, use separate
group and route ACLs: every attached ACL must allow the client. A route cannot
weaken its group's restriction, including Lua route-list convenience groups.

Addresses accept numeric IPv4/IPv6, CIDRs including /0, and configured forward-DNS
hostnames. IPv4-mapped IPv6 normalizes to IPv4. Countries are exactly two uppercase
ASCII letters, including XK. `unknown` matches a valid missing country/record;
database and DNS errors never become unknown. The subject is the effective
client unless `use_peer` selects the actual TCP peer.

```lua
local knurl = require('knurl')
local server = assert(knurl.server{port=8080,
  client_ip={trusted_proxies={'10.0.0.0/8'}, max_hops=16}})
local country = assert(knurl.acl{
  policy='deny', country_database='/srv/geo/country.mmdb',
  allow={countries={'SE','NO'}}, deny={unknown=true}})
local group = assert(server:group('/internal', {acl=country}))
assert(group:get('/status', function() return 'ok' end, {
  acl={policy='deny', allow={addresses={'192.0.2.0/24','office.example'}}}}))
assert(country:close()) -- Core retains the group's reference.
assert(server:run())
assert(server:close())
```

Construction and server validation copy/validate declarations without DNS, files
or threads. Startup prepares attached ACLs before serving. Country paths open
owned [MMDB readers](mmdb.md); `max_heap_bytes` bounds that reader's requested
heap (zero selects 512 KiB), not all ACL storage. The immutable inode stays
pinned; attach a new ACL between runs to load a replacement database. C hosts
may instead set `country_provider`, borrowing it through final ACL retirement.

Ordinary, JSON, XML, REST, proxy, WebSocket, static/assets, upload and WebDAV C
declarations expose `acl`; existing initializers set it to NULL. Registration
retains a reference. `knurl_route_set_acl` / `route->set_acl` and group equivalents
replace an attachment while the router is inactive; NULL clears that level.
`knurl_acl_close` releases one owned reference. Declaration views borrow pointers.

For example, a standalone C check uses the same receiver as routes:

```c
knurl_acl_config config;
knurl_acl *acl;
knurl_mmdb_address peer;
knurl_error error;
knurl_status status;
int allowed = 0;
const char *networks[] = {"192.0.2.0/24", "2001:db8::/32"};

knurl_acl_config_init(&config);
config.policy = KNURL_ACL_DENY;
config.allow.addresses = networks;
config.allow.address_count = 2;
if (knurl_acl_new(&config, &acl, &error) != KNURL_OK)
    return 1;
status = acl->refresh(acl, &error);
if (status == KNURL_OK)
    status = knurl_mmdb_address_parse("192.0.2.1", &peer, &error);
if (status == KNURL_OK)
    status = acl->check(acl, &peer, NULL, &allowed, &error);
acl->close(acl, NULL);
/* Only status == KNURL_OK && allowed grants access. */
```

The common route gate precedes preflight, handler hooks, body delivery,
upload/file/provider opening, proxy callbacks/connections and upgrades. Deny
returns 403 without route fallthrough. Unprepared/unavailable ACLs return 503.
Both reject advertised bodies before 100 Continue. Global admission still
precedes routing; host dispatch callbacks bypassing the router own their policy.

## DNS lifecycle and standalone use

Default `dns_refresh_ms=0` resolves once at preparation for that ACL lifetime.
Positive milliseconds opt into background refresh, for example 60000 for at most
one attempt per minute per ACL. Distinct configured names resolve once per
attempt; both lists share their answers. A/AAAA results normalize, deduplicate
and use a binary-search index. Checks never resolve names or schedule work.

HTTP workers share a bounded native executor started after their final fork.
Refreshes cannot overlap for one ACL. Complete snapshots publish atomically;
checks retain the previous snapshot while resolution runs. Failure, queue
pressure or deadline expiry makes the ACL unavailable until resolution succeeds.
Late results cannot publish after timeout. Restarted workers reject overdue
state until refreshed. `dns_timeout_ms=0` selects 10000 milliseconds. System DNS
cancellation is cooperative; executor retirement and worker shutdown deadlines
retain state until work ends or the worker exits.
Expired attempts stop between hostnames, so cancellation does not continue
resolving the rest of a large configured list.

Initial DNS failure prevents startup. `dns_max_addresses=0` selects 64 per name;
the maximum is 1024. Oversized, empty or invalid answers reject the entire update.
Both lists together permit 1024 address/name entries; each country list permits
1024 codes. C hosts may supply a bounded, thread-safe `resolve` callback and
retain its userdata. Background callbacks must not use Lua or live HTTP handles.

Standalone C hosts call `knurl_acl_refresh` before `knurl_acl_check` and schedule
later refreshes themselves. AGAIN means not due or already refreshing. Check
accepts binary peer/effective addresses, returns an explicit status and clears
the admission output on failure. Lua exposes `acl:refresh()` (true when refreshed,
false when not due), `acl:check(peer[, effective])` (boolean or nil/error), and
`acl:close()`. Checks and refresh may run concurrently. Fork and final close
require quiescent users. Startup-only and DNS-free ACLs create no background work.

## Forwarding trust

`client_ip.trusted_proxies` accepts numeric addresses, CIDRs and `*`. Wildcard
trust applies only to the immediate TCP peer. Chains are walked right to left,
skipping only explicitly trusted numeric hops; the nearest untrusted hop wins.
`*` alone cannot skip that nearest hop for an attacker-selected leftmost value.
It relies on controlling access to the listener: any connecting peer may supply
the single-hop identity.

Defaults read X-Forwarded-For, using X-Real-IP only when the chain is absent.
`forwarded_header` / `real_ip_header` customize names; empty disables them.
`forwarded_format='comma'` is the default. `'rfc7239'` parses
[Forwarded `for` parameters](https://www.rfc-editor.org/rfc/rfc7239.html), including
quoted IPv6 and numeric ports; explicitly select `forwarded_header='forwarded'`.
Unusable/obfuscated, malformed, duplicate or oversized chains use the peer
without trying a second identity header. Single-address fields must also be
unique and valid. `max_hops=0` selects 16, with a maximum of 64. Untrusted peers'
headers are ignored. Connection admission always sees the actual peer.

ACLs using effective identity reject unusable forwarding data from trusted peers
with 403; fallback to the proxy's peer address cannot grant client access.
`use_peer` explicitly evaluates that peer, and a successful host `client_ip.resolve`
callback explicitly replaces the normal identity decision.
Zero-length C results and Lua nil preserve the default and its trust validity;
they cannot turn an invalid forwarding chain into an allowed proxy identity.

The [implementation contract](acl-spec.md) defines the verification envelope.
