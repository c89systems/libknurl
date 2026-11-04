# ACME state publication

The backend-internal generation store implements the ACME publication contract.
It is not an installed application API. The [Kore adapter](../src/kore_acme.c)
connects it to public server startup, key-manager recovery, certificate
installation and the parent-enforced child-operation budget. See the
[TLS runtime contract](tls.md) for declarations and identity observations.

The configured runtime root must be prepared, owned by the current user and
accessible only to that user. The store owns its `.rotmos` subdirectory. One
runtime/key-manager owner serializes mutations. The host retains durable storage,
encryption, remote consistency and multi-writer policy.

RESTORE hydrates a fresh complete generation in the quiescent parent. The
callback receives that generation's directory as its `runtime_directory` argument;
use that argument for file operations. A failed restore cannot overwrite the
selected generation. The first restoration can seed the snapshot from an
existing Kore layout in the configured root. Later restores seed from the
acknowledged local generation, then let the host hydrate newer durable state.
No-op hydration therefore preserves acknowledged local state. Creating the
server or declaring the store invokes no callbacks.

The snapshot layout preserves `account-key.pem` and each configured domain's
`certificates/<domain>/key.pem` and `fullchain.pem`. DNS labels are checked before
they become path components. Missing identities permit pending issuance.
Present private keys must be unencrypted RSA keys of at least 2048 bits, matching
Kore's ACME backend. Present chains require a matching key and certificate name.
An extra application storage format or Vectis dependency is not introduced.

Working generations are copied from acknowledged state. Each file has the
declared material bound; copies use a bounded heap buffer. Input files and
directories cannot redirect traversal through symbolic links. Snapshot files
use mode 0600 and generation directories use mode 0700. Partial snapshots are
removed on failure. Key-copy buffers are cleansed before release.

PERSIST receives a complete candidate snapshot, with the current generation
still selected. Its input is read-only to the host. Only a successful callback,
completed within its cooperative budget, permits an acknowledgement marker.
The store rechecks key pairs and bounds after the callback. ERROR receives a
copy of the original cause; its return value or edits cannot replace that cause.
Returned failure removes the candidate selection and preserves active state.
An error observation cannot roll back a remote storage write that already
completed.

Publication selects the complete acknowledged generation with one symlink
replacement. Files and containing directories are synchronized before
unselected old generations are collected. This follows the platform's
[rename contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/rename.2.html)
and [file synchronization contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html).
The host's durable-storage policy remains the persistence acknowledgement.

A pending acknowledged generation can finish activation after interruption.
An interrupted callback with no acknowledgement is retried through PERSIST in
the replacement key manager before activation. Hosts must make that retry safe.
Unselected work and generations are removed during recovery. Closing a store
removes its own working copy and retains acknowledged/pending state for recovery.
The runtime root remains owned by its caller.

The [focused fixture](../tests/acme-state.c) uses actual forked key-manager
contexts and the public callback signature. It covers failure before
acknowledgement, interruption inside PERSIST, interruption after acknowledgement,
ordinary owner restart, new RESTORE hydration, failed RESTORE, late callbacks,
complete multi-domain snapshots, wrong keys, oversized inputs and symlinks.
These are publication-core checks. The separate
[runtime fixture](../tests/acme-runtime.c) proves wire-level certificate
activation, failed PERSIST preservation, key-manager restart, enforced PERSIST
timeout and pending TLS-ALPN-01 challenges through the public server.
The [protocol fixture](acme-protocol.md) adds HTTPS account/order transport,
CSR and live challenge verification, issuance and renewal selection after an
ACME-process restart.
