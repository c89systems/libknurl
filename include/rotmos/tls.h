#ifndef ROTMOS_TLS_H
#define ROTMOS_TLS_H

/** @file tls.h Public tls API. Expected failures return rotmos_status and
 * optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/io.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for tls mode. */
typedef enum rotmos_tls_mode {
    /** TLS mode/availability: disabled. */
    ROTMOS_TLS_DISABLED = 0,
    /** TLS mode/availability: manual. */
    ROTMOS_TLS_MANUAL,
    /** TLS mode/availability: acme. */
    ROTMOS_TLS_ACME
} rotmos_tls_mode;

/** Values for tls version. */
typedef enum rotmos_tls_version {
    /** TLS protocol selector: default. */
    ROTMOS_TLS_VERSION_DEFAULT = 0,
    /** TLS protocol selector: both. */
    ROTMOS_TLS_VERSION_BOTH,
    /** TLS protocol selector: 1 2. */
    ROTMOS_TLS_VERSION_1_2,
    /** TLS protocol selector: 1 3. */
    ROTMOS_TLS_VERSION_1_3
} rotmos_tls_version;

/** Values for material kind. */
typedef enum rotmos_material_kind {
    /** TLS material source: none. */
    ROTMOS_MATERIAL_NONE = 0,
    /** TLS material source: file. */
    ROTMOS_MATERIAL_FILE,
    /** TLS material source: memory. */
    ROTMOS_MATERIAL_MEMORY,
    /** TLS material source: callback. */
    ROTMOS_MATERIAL_CALLBACK
} rotmos_material_kind;

/** A successful open transfers one source. Construction never calls it.
 * validate/run consume bounded material synchronously to EOF; AGAIN is STATE.
 * Manual identities are parsed in memory without temporary key files. */
typedef rotmos_status (*rotmos_material_open_fn)(rotmos_source **out,
                                                 void *userdata,
                                                 rotmos_error *error);

/** TLS PEM input from a file, copied memory or a source opener. Construction is
 * inert; validate/run consume material synchronously.
 */
typedef struct rotmos_tls_material {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    rotmos_material_kind kind;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Copied bounded operation payload, or TLS bytes copied by the server
     * declaration.
     */
    rotmos_bytes bytes;
    /** Resource/state opener; failed construction must clean its own partial
     * state.
     */
    rotmos_material_open_fn open;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_tls_material;

/** Manual or ACME domain identity. Strings/byte declarations are copied with
 * the server; opener userdata remains borrowed.
 */
typedef struct rotmos_tls_identity {
    /** Selected policy mode; use the enclosing initializer for defaults. */
    rotmos_tls_mode mode;
    /** Identity domain names copied with the server declaration. */
    const char *const *domains;
    /** Number of configured identity domain names. */
    size_t domain_count;
    /** Combined PEM certificate/key material for a manual TLS identity. */
    rotmos_tls_material bundle;
    /** PEM certificate/chain material for a manual TLS identity. */
    rotmos_tls_material certificate;
    /** PEM private key material paired with the certificate. */
    rotmos_tls_material private_key;
    /** Optional additional PEM chain certificates. */
    rotmos_tls_material extra_chain;
    /** Optional trusted client-CA material for mutual TLS. */
    rotmos_tls_material client_ca;
    /** Nonzero requires verified downstream client certificates. */
    int require_client_certificate;
} rotmos_tls_identity;

/** Values for acme event. */
typedef enum rotmos_acme_event {
    /** Host ACME state notification: restore. */
    ROTMOS_ACME_RESTORE = 0,
    /** Host ACME state notification: persist. */
    ROTMOS_ACME_PERSIST,
    /** Host ACME state notification: error. */
    ROTMOS_ACME_ERROR
} rotmos_acme_event;

/** Values for tls identity state. */
typedef enum rotmos_tls_identity_state {
    /** TLS mode/availability: pending. */
    ROTMOS_TLS_PENDING = 0,
    /** TLS mode/availability: usable. */
    ROTMOS_TLS_USABLE
} rotmos_tls_identity_state;

/** Identity availability/failure snapshot. USABLE means installed in every HTTP
 * worker; a separate error never rewrites that availability state.
 */
typedef struct rotmos_tls_identity_event {
    /** Zero-based TLS identity index for an HTTPS listener. */
    size_t identity_index;
    /** State callback or identity availability, according to the containing
     * type.
     */
    rotmos_tls_identity_state state;
    /** Borrowed identity domain during the TLS/ACME callback. */
    rotmos_bytes domain;
    /** Copied bounded diagnostic; OK/NONE when no error is attached. */
    rotmos_error error;
} rotmos_tls_identity_event;

/** Parent-loop identity availability/failure observation with a borrowed event
 * snapshot.
 */
typedef void (*rotmos_tls_identity_fn)(rotmos_server *server,
                                       const rotmos_tls_identity_event *event,
                                       void *userdata);

/** Host durability hook: RESTORE in the quiescent parent, PERSIST in key
 * manager after certificate write and before distribution, ERROR with the
 * preserved cause. Runtime path/domain are borrowed for the call.
 */
typedef rotmos_status (*rotmos_acme_state_fn)(
    rotmos_server *server, const rotmos_process_context *process,
    rotmos_acme_event event, rotmos_bytes domain,
    rotmos_bytes runtime_directory, void *userdata, rotmos_error *error);

/** Configuration for acme. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_acme_config {
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
    /** State callback or identity availability, according to the containing
     * type.
     */
    rotmos_acme_state_fn state;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_acme_config;

/** Configuration for tls. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_tls_config {
    /** Accepted downstream TLS protocol versions; DEFAULT resolves to the
     * backend policy.
     */
    rotmos_tls_version version;
    /** Copied TLS cipher-list declaration. */
    const char *cipher_list;
    /** Maximum bytes accepted per TLS material source. */
    size_t max_material_bytes;
    /** Copied TLS identity declarations; callback userdata remains borrowed. */
    const rotmos_tls_identity *identities;
    /** Number of identities. */
    size_t identity_count;
    /** ACME working-state configuration and host durability hooks. */
    rotmos_acme_config acme;
} rotmos_tls_config;

/** Initialize tls material with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_tls_material_init(rotmos_tls_material *material);
/** Initialize tls identity with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_tls_identity_init(rotmos_tls_identity *identity);
/** Initialize tls config with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_tls_config_init(rotmos_tls_config *config);
/** Active-run view; outside a run returns STATE. An identity with no HTTPS
 * listener remains pending. The output borrows no backend structures. */
ROTMOS_API rotmos_status rotmos_server_tls_identity_state(
    const rotmos_server *server, size_t index, rotmos_tls_identity_state *out,
    rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
