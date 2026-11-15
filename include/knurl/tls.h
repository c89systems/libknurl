#ifndef KNURL_TLS_H
#define KNURL_TLS_H

/** @file tls.h Public tls API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/io.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Borrow the selected system CA bundle path. CURL_CA_BUNDLE then SSL_CERT_FILE
 * override conventional system locations, including when the override is
 * invalid (TLS must fail rather than silently broaden trust). A nonempty
 * SSL_CERT_DIR selects directory trust instead of the fallback bundle when
 * neither bundle override is set. NULL means no file was discovered.
 * Environment must remain stable during use.
 */
KNURL_API const char *knurl_tls_system_ca_file(void);

/** TLS identity source; configured identities require MANUAL or ACME. */
typedef enum knurl_tls_mode {
    /** Unconfigured identity; rejected in a configured identity array. */
    KNURL_TLS_DISABLED = 0,
    /** Host-supplied certificate and private-key material. */
    KNURL_TLS_MANUAL,
    /** Kore-managed ACME issuance and certificate renewal. */
    KNURL_TLS_ACME
} knurl_tls_mode;

/** Accepted downstream TLS protocol versions. */
typedef enum knurl_tls_version {
    /** Default policy: enable TLS 1.2 and TLS 1.3. */
    KNURL_TLS_VERSION_DEFAULT = 0,
    /** Explicitly enable both TLS 1.2 and TLS 1.3. */
    KNURL_TLS_VERSION_BOTH,
    /** Accept TLS 1.2 only. */
    KNURL_TLS_VERSION_1_2,
    /** Accept TLS 1.3 only. */
    KNURL_TLS_VERSION_1_3
} knurl_tls_version;

/** Values for material kind. */
typedef enum knurl_material_kind {
    /** TLS material source: none. */
    KNURL_MATERIAL_NONE = 0,
    /** TLS material source: file. */
    KNURL_MATERIAL_FILE,
    /** TLS material source: memory. */
    KNURL_MATERIAL_MEMORY,
    /** TLS material source: callback. */
    KNURL_MATERIAL_CALLBACK
} knurl_material_kind;

/** A successful open transfers one source. Construction never calls it.
 * validate/run consume bounded material synchronously to EOF; AGAIN is STATE.
 * Manual identities are parsed in memory without temporary key files. */
typedef knurl_status (*knurl_material_open_fn)(knurl_source **out,
                                               void *userdata,
                                               knurl_error *error);

/** TLS PEM input from a file, copied memory or a source opener. Construction is
 * inert; validate/run consume material synchronously.
 */
typedef struct knurl_tls_material {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    knurl_material_kind kind;
    /** Copied PEM file path, used only for KNURL_MATERIAL_FILE. */
    const char *path;
    /** PEM bytes copied with the declaration, for KNURL_MATERIAL_MEMORY. */
    knurl_bytes bytes;
    /** KNURL_MATERIAL_CALLBACK opener; transfers one source only on success. */
    knurl_material_open_fn open;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_tls_material;

/** Manual or ACME domain identity. Strings/byte declarations are copied with
 * the server; opener userdata remains borrowed.
 */
typedef struct knurl_tls_identity {
    /** Selected policy mode; use the enclosing initializer for defaults. */
    knurl_tls_mode mode;
    /** Identity domain names copied with the server declaration. */
    const char *const *domains;
    /** Number of configured identity domain names. */
    size_t domain_count;
    /** Combined PEM certificate/key material for a manual TLS identity. */
    knurl_tls_material bundle;
    /** PEM certificate/chain material for a manual TLS identity. */
    knurl_tls_material certificate;
    /** PEM private key material paired with the certificate. */
    knurl_tls_material private_key;
    /** Optional additional PEM chain certificates. */
    knurl_tls_material extra_chain;
    /** Optional trusted client-CA material for mutual TLS. */
    knurl_tls_material client_ca;
    /** Nonzero requires verified downstream client certificates. */
    int require_client_certificate;
} knurl_tls_identity;

/** Values for acme event. */
typedef enum knurl_acme_event {
    /** Host ACME state notification: restore. */
    KNURL_ACME_RESTORE = 0,
    /** Host ACME state notification: persist. */
    KNURL_ACME_PERSIST,
    /** Host ACME state notification: error. */
    KNURL_ACME_ERROR
} knurl_acme_event;

/** Values for tls identity state. */
typedef enum knurl_tls_identity_state {
    /** TLS mode/availability: pending. */
    KNURL_TLS_PENDING = 0,
    /** TLS mode/availability: usable. */
    KNURL_TLS_USABLE
} knurl_tls_identity_state;

/** Identity availability/failure snapshot. USABLE means installed in every HTTP
 * worker; a separate error never rewrites that availability state.
 */
typedef struct knurl_tls_identity_event {
    /** Zero-based TLS identity index for an HTTPS listener. */
    size_t identity_index;
    /** Current installation availability: PENDING or USABLE. */
    knurl_tls_identity_state state;
    /** Borrowed identity domain during the TLS/ACME callback. */
    knurl_bytes domain;
    /** Copied bounded diagnostic; OK/NONE when no error is attached. */
    knurl_error error;
} knurl_tls_identity_event;

/** Parent-loop identity availability/failure observation with a borrowed event
 * snapshot.
 */
typedef void (*knurl_tls_identity_fn)(knurl_server *server,
                                      const knurl_tls_identity_event *event,
                                      void *userdata);

/** Host durability hook: RESTORE in the quiescent parent, PERSIST in key
 * manager after certificate write and before distribution, ERROR with the
 * preserved cause. Runtime path/domain are borrowed for the call.
 */
typedef knurl_status (*knurl_acme_state_fn)(
    knurl_server *server, const knurl_process_context *process,
    knurl_acme_event event, knurl_bytes domain, knurl_bytes runtime_directory,
    void *userdata, knurl_error *error);

/** File-backed ACME durability policy for an explicitly configured private
 * runtime_directory on durable storage. RESTORE uses files in place; PERSIST
 * fsyncs account/domain keys, certificate and containing directories before
 * publication. Rejects symlinks, foreign owners and nonprivate
 * files/directories. userdata is unused. This synchronous key-manager callback
 * can report I/O failure; the configured persistence budget applies. */
KNURL_API knurl_acme_state_fn knurl_acme_file_state(void);

/** Configuration for acme. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_acme_config {
    /** ACME account contact email. */
    const char *email;
    /** ACME CA directory URL. */
    const char *directory_url;
    /** Kore ACME working directory; host restore/persist owns durable storage
     * policy.
     */
    const char *runtime_directory;
    /** Optional CA trust-bundle file for ACME directory HTTPS. */
    const char *ca_file;
    /** Optional CA trust directory for ACME directory HTTPS. */
    const char *ca_directory;
    /** Deadline for host ACME persistence acknowledgement, in milliseconds. */
    long persist_timeout_ms;
    /** Host restore/persist/error callback; invoked during run, not validation.
     */
    knurl_acme_state_fn state;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_acme_config;

/** Configuration for tls. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_tls_config {
    /** Accepted downstream TLS versions; DEFAULT enables TLS 1.2 and TLS 1.3.
     */
    knurl_tls_version version;
    /** Copied TLS cipher-list declaration. */
    const char *cipher_list;
    /** Maximum bytes accepted per TLS material source. */
    size_t max_material_bytes;
    /** Copied TLS identity declarations; callback userdata remains borrowed. */
    const knurl_tls_identity *identities;
    /** Number of identities. */
    size_t identity_count;
    /** ACME working-state configuration and host durability hooks. */
    knurl_acme_config acme;
} knurl_tls_config;

/** Initialize tls material with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_tls_material_init(knurl_tls_material *material);
/** Initialize tls identity with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_tls_identity_init(knurl_tls_identity *identity);
/** Initialize tls config with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_tls_config_init(knurl_tls_config *config);
/** Active-run view; outside a run returns STATE. An identity with no HTTPS
 * listener remains pending. The output borrows no backend structures. */
KNURL_API knurl_status knurl_server_tls_identity_state(
    const knurl_server *server, size_t index, knurl_tls_identity_state *out,
    knurl_error *error);

#ifdef __cplusplus
}
#endif
#endif
