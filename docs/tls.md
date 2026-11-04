# TLS listeners

> Implementation reference under correction. The [extraction contract](server-api-spec.md) governs required behavior and scope; this document describes current code, not compatibility or an additional product requirement. See the [audit](server-api-audit.md) for known failures.


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
prepares fresh inputs; manual certificate replacement uses a complete server restart.
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

An ACME identity declares explicit DNS names and `ROTMOS_TLS_ACME`. Configure
`tls.acme` with the account email, HTTP(S) directory URL, prepared private runtime
root, state callback and persistence budget. Preflight validates the root and CA
inputs without callbacks, directory creation or traffic. Explicit CA settings
take precedence over `SSL_CERT_FILE`/`SSL_CERT_DIR`; file bytes and resolved
directory paths are captured before forks. The selected SDK supplies defaults
when these inputs are absent.

The [generation store](acme-state.md) runs RESTORE in the quiescent parent.
After IPC initialization, the key manager recovers pending state, prepares a
private working generation and loads or generates its keys. Initial complete
key state and each completed certificate pass through PERSIST and acknowledged
publication before certificate distribution. The parent enforces the dedicated
child budget through snapshot preparation, persistence and publication. Returned
failures or timeout reach child observations and the selected death policy.
Manual in-memory identities continue to work when the ACME key manager is active.
Certificate updates reuse prepared protocol, cipher, chain and client-auth
policy; material callbacks are not reopened in HTTP workers.

`rotmos_server_tls_identity_state` returns PENDING or USABLE during an active
run. USABLE means every HTTPS binding for that identity has installed a usable
certificate in every HTTP worker. It does not attest remote CA trust, validity
dates or availability of an external service. Unbound identities remain pending.
`callbacks.on_tls_identity` runs on the parent loop for initial state,
installation changes, worker availability changes and key-manager failures.
Repeated USABLE observations can reflect certificate replacement; they are not
a remote issuance count. Failure events retain the current state, include the
bounded cause, and identify the domain when the interrupted operation knew it.

TLS-ALPN-01 challenge certificates remain pending and are not application
identities. Challenges work while application serving is paused and bypass
client authentication only for the negotiated challenge connection. Ordinary
HTTP remains gated and subject to the identity's client-auth policy.

The [runtime fixture](../tests/acme-runtime.c) exercises actual private
key-manager signing, TLS 1.2/1.3, mTLS, repeated listeners, two HTTP workers,
pending challenge handling, acknowledged replacement, persistence failure and
parent-enforced timeout/recovery. Its [private driver](../tests/acme-backend.c)
submits the real ACME-to-key-manager certificate/challenge messages; it does not
simulate a remote ACME directory, JWS account/order exchange or renewal schedule.
The separate [protocol fixture](acme-protocol.md) drives a local HTTPS CA through
account recovery, signed orders, CSR, TLS-ALPN-01, issuance and renewal selection
after ACME-process restart. It also covers captured trust and transport bounds.
Complete allocation/fatal unwinding and in-flight protocol fault coverage remain.

## Lua callbacks

Lua identities accept `mode='acme'` and callback materials. A material declaration
is `{kind='callback', open=function() ... end}`, with kind optional when open is
supplied. The opener returns an unowned `rotmos.source`, a legacy read/reset/close
table, or nil/error. Callback declarations and their owners remain pinned through
server close; changing the original open field does not change the declaration.
A source already transferred, closed or currently executing cannot be transferred again.
Legacy reset executes once before the first pull, and each accepted source closes
once on success or failure. Consumption reuses the bounded synchronous C material
reader; AGAIN fails with STATE. No TLS transport or PEM parser is implemented in
the binding.

Each explicit validate and run opens fresh material. The same server cannot be
closed, run or recursively validated inside its material callbacks. The complete
opener/read/reset/close boundary is protected, including result conversion and
allocation failures. Cleanup errors do not replace a native operation failure;
a close callback failure makes an otherwise successful validation/run fail. A stop requested
while preparing material prevents subsequent worker forks. Lua callbacks own the
lifetime of any Lua strings they retain; the C core cleanses its own copies.

`tls.acme` accepts email, directory_url, runtime_directory, ca_file, ca_directory,
persist_timeout_ms and `state(server, context)`. Context copies event
(restore/persist/error), domain, runtime_directory and process. ERROR also copies
the public cause as context.error. Nil/true succeeds, false rejects with STATE,
and nil/error propagates the supplied status. Thrown errors and OOM are caught
before returning through the C frame. ERROR return/failure follows the native
notification contract and cannot replace the primary persistence/restore failure.

RESTORE runs in the quiescent parent; PERSIST runs in the key manager after fork,
before acknowledged publication. Clients with threads must be created in the
owning process. The callback owns durable storage selection and hydration; it
receives the candidate directory, with no Vectis/Lockd type or implicit XDG path.
The configured deadline, restart policy, publication and rollback remain in the
public C ACME engine. Construction and validate invoke no ACME state callback or
CA traffic. A successful RESTORE callback which requests stop completes cleanup
without starting children. Effective configuration copies ordinary ACME values.

The [installed Lua fixture](../tests/lua/tls.lua) covers live mutual TLS with
callback certificate/key/chain, copied declarations, source reset/close and
busy/closed ownership, bounded input, callback/OOM failures and startup reentry.
It also exercises actual parent RESTORE and key-manager PERSIST/ERROR delivery,
failed persistence propagation and stop before fork. Full remote issuance and
in-flight fault combinations remain part of the final integration review.
