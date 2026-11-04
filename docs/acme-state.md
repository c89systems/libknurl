# ACME host state

Librotmos uses Kore's runtime layout directly: `account-key.pem` and each
domain's `certificates/<domain>/key.pem` and `fullchain.pem`. The host prepares
the private runtime directory and owns durable storage, hydration and cleanup.
There is no generation store, journal, selection symlink or second storage format.
The build-time [Kore correction](../patches/kore/0061-direct-acme-host-persistence.patch)
retains Vectis's persistence hook and removes the extra installation/startup hooks.

RESTORE runs once in the quiescent parent before child startup. Its directory
is the canonical configured runtime root. Construction and explicit validation
invoke no state callback and create no runtime files. The key manager enters
that same root and lets Kore load or generate account/domain keys. Startup adds
no PERSIST call.

After writing an issued certificate, Kore invokes PERSIST in the key manager
before distributing it to HTTP workers. The callback receives the runtime root
and domain; a Vectis provider persists the complete tree using its existing
durable-storage client. Clients and their deadlines remain host-owned.
The configured Core persistence budget also bounds the key-manager callback.

Failure prevents that distribution and reaches the process failure/restart
policy. ERROR receives a copy of the original cause; observer edits or failure
cannot replace it. Written runtime files remain after failure, as in Vectis.
A replacement key manager loads those files; librotmos adds no rollback or
remote-storage transaction. The host's durable store remains its authority.

The [state fixture](../tests/acme-state.c) checks directory ownership, no added
layout, unchanged host files, terminal callback results, error preservation and
the cooperative budget. The [runtime fixture](../tests/acme-runtime.c) checks
actual certificate distribution, failed persistence, restart, timeout and
TLS-ALPN-01. The [protocol fixture](../tests/acme-protocol.c) uses the public
[Vectis provider adapter](../tests/vectis-services.c) through real account/order,
issuance and renewal against a local HTTPS CA. No durable-client SDK is imported.
