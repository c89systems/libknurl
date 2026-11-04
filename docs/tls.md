# TLS listeners

Declare a `ROTMOS_LISTENER_HTTPS` listener and set `identity_index` to an entry in
`config.tls.identities`. A manual identity has explicit domains and either a
certificate/key PEM bundle or separate certificate and private-key material.
`extra_chain` adds server chain certificates; `client_ca` supplies independent
client-auth trust. These roles are separate. `require_client_certificate` makes
a valid client certificate mandatory. With client trust and that flag clear,
client certificates are optional; a supplied invalid certificate still fails.

Each material accepts a regular file, copied bytes or an opener returning an
unowned public `rotmos_source`. Construction only copies declarations. Explicit
validation and each run open their own inputs and consume them synchronously to
EOF, under `max_material_bytes` per input (zero selects 1 MiB). AGAIN fails with
STATE; no background material reader is implied. Successful opener transfer
receives one close notification on consumption or failure. The source must be
ready for bounded synchronous consumption; a host adapter owns any blocking
client lifetime or cooperative deadline. Validation performs no bind, fork,
RESTORE/PERSIST callback or ACME traffic.

Run preparation follows the parent's prepare hook and reads each identity once.
PEM is parsed directly into reference-counted OpenSSL contexts, with matching-key,
chain, trust and cipher checks. The adapter creates no temporary key files and
requires no external DH parameter file. Temporary input copies and owned copied
configuration material are cleansed when released. Contexts remain owned through
the listener/domain lifecycle and are released on failure or stop. A new run
prepares fresh inputs; certificate replacement uses a complete server restart.
Encrypted private keys require a host-supplied decrypted material source; the
library never prompts for passwords.

Default/BOTH enables TLS 1.2 and 1.3; explicit single-version selections are
available. `cipher_list` preserves Kore's OpenSSL cipher-list behavior, including
its default list. TLS 1.2 ECDSA identities require a matching ECDSA cipher list;
that list does not configure TLS 1.3 suites. Unknown SNI is rejected; a literal
`*` identity is an explicit wildcard choice. Host/SNI mismatch returns 421.
Each HTTPS listener selects its own initial context and domain registry.

`ROTMOS_LISTENER_REDIRECT` uses the referenced identity's allowed domains and
`redirect_https_port` as the presented HTTPS port. It emits 308 with the original
raw path/query, including escaped separators and an empty trailing query, and
omits port 443. It validates the Host authority, handles bracketed IPv6, and
rejects an unapproved or malformed host. Local bind ports and presented ports are
distinct. Redirects use the application serving gate.

The [public TLS fixture](../tests/tls-runtime.c) covers file/memory/callback input,
bundles and extra chains, incorrect keys/trust/ciphers, source AGAIN and file
bounds, SNI/Host checks, required/optional client authentication, TLS 1.2/1.3,
multiple listeners and raw-URI redirects. Its channel echo preserves early bytes,
bounded output and TLS close notifications. Prepared trust stores use OpenSSL's
[verification-store reference contract](https://docs.openssl.org/3.6/man3/SSL_CTX_set1_verify_cert_store/).
Forced WANT_READ/WANT_WRITE faults, slow/disconnected peers and complete failure
unwinding remain integration coverage.

ACME identity activation, candidate persistence/publication, observations and
auxiliary-process deadlines remain required under the
[server specification](server-api-spec.md). Preflight rejects ACME until that
workflow is connected; a successful manual TLS check is not ACME evidence.
