#ifndef ROTMOS_TLS_H
#define ROTMOS_TLS_H

#include <rotmos/io.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rotmos_tls_mode {
    ROTMOS_TLS_DISABLED = 0,
    ROTMOS_TLS_MANUAL,
    ROTMOS_TLS_ACME
} rotmos_tls_mode;

typedef enum rotmos_tls_version {
    ROTMOS_TLS_VERSION_DEFAULT = 0,
    ROTMOS_TLS_VERSION_BOTH,
    ROTMOS_TLS_VERSION_1_2,
    ROTMOS_TLS_VERSION_1_3
} rotmos_tls_version;

typedef enum rotmos_material_kind {
    ROTMOS_MATERIAL_NONE = 0,
    ROTMOS_MATERIAL_FILE,
    ROTMOS_MATERIAL_MEMORY,
    ROTMOS_MATERIAL_CALLBACK
} rotmos_material_kind;

/* A successful open transfers one source. Construction never calls it.
 * validate/run consume bounded material synchronously to EOF; AGAIN is STATE.
 * Manual identities are parsed in memory without temporary key files. */
typedef rotmos_status (*rotmos_material_open_fn)(rotmos_source **out,
                                                 void *userdata,
                                                 rotmos_error *error);

typedef struct rotmos_tls_material {
    rotmos_material_kind kind;
    const char *path;
    rotmos_bytes bytes;
    rotmos_material_open_fn open;
    void *userdata;
} rotmos_tls_material;

typedef struct rotmos_tls_identity {
    rotmos_tls_mode mode;
    const char *const *domains;
    size_t domain_count;
    rotmos_tls_material bundle;
    rotmos_tls_material certificate;
    rotmos_tls_material private_key;
    rotmos_tls_material extra_chain;
    rotmos_tls_material client_ca;
    int require_client_certificate;
} rotmos_tls_identity;

typedef enum rotmos_acme_event {
    ROTMOS_ACME_RESTORE = 0,
    ROTMOS_ACME_PERSIST,
    ROTMOS_ACME_ERROR
} rotmos_acme_event;

typedef enum rotmos_tls_identity_state {
    ROTMOS_TLS_PENDING = 0,
    ROTMOS_TLS_USABLE
} rotmos_tls_identity_state;

/* Usable means every configured HTTPS domain is installed in every HTTP
 * worker. Failure observations retain that state and carry a separate cause. */
typedef struct rotmos_tls_identity_event {
    size_t identity_index;
    rotmos_tls_identity_state state;
    rotmos_bytes domain;
    rotmos_error error;
} rotmos_tls_identity_event;

typedef void (*rotmos_tls_identity_fn)(rotmos_server *server,
                                       const rotmos_tls_identity_event *event,
                                       void *userdata);

typedef rotmos_status (*rotmos_acme_state_fn)(
    rotmos_server *server, const rotmos_process_context *process,
    rotmos_acme_event event, rotmos_bytes domain,
    rotmos_bytes runtime_directory, void *userdata, rotmos_error *error);

typedef struct rotmos_acme_config {
    const char *email;
    const char *directory_url;
    const char *runtime_directory;
    const char *ca_file;
    const char *ca_directory;
    long persist_timeout_ms;
    rotmos_acme_state_fn state;
    void *userdata;
} rotmos_acme_config;

typedef struct rotmos_tls_config {
    rotmos_tls_version version;
    const char *cipher_list;
    size_t max_material_bytes;
    const rotmos_tls_identity *identities;
    size_t identity_count;
    rotmos_acme_config acme;
} rotmos_tls_config;

ROTMOS_API void rotmos_tls_material_init(rotmos_tls_material *material);
ROTMOS_API void rotmos_tls_identity_init(rotmos_tls_identity *identity);
ROTMOS_API void rotmos_tls_config_init(rotmos_tls_config *config);
/* Active-run view; outside a run returns STATE. An identity with no HTTPS
 * listener remains pending. The output borrows no backend structures. */
ROTMOS_API rotmos_status rotmos_server_tls_identity_state(
    const rotmos_server *server, size_t index, rotmos_tls_identity_state *out,
    rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
