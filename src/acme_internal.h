#ifndef ROTMOS_ACME_INTERNAL_H
#define ROTMOS_ACME_INTERNAL_H

#include "tls_internal.h"

struct rotmos_acme_store {
    const rotmos_tls_config *config;
    const char *default_ciphers;
    const char **domains;
    size_t domain_count;
    char *root;
    char *directory;
    char *work;
    char *candidate;
    int descriptor;
};

int rotmos_acme_domain_valid(const char *domain);
rotmos_status rotmos_acme_store_new(const rotmos_tls_config *config,
                                    const char *default_ciphers,
                                    struct rotmos_acme_store **out,
                                    rotmos_error *error);
void rotmos_acme_store_free(struct rotmos_acme_store *store);
rotmos_status rotmos_acme_store_restore(struct rotmos_acme_store *store,
                                        rotmos_server *server,
                                        const rotmos_process_context *process,
                                        rotmos_error *error);
rotmos_status rotmos_acme_store_begin(struct rotmos_acme_store *store,
                                      rotmos_error *error);
rotmos_status rotmos_acme_store_stage(struct rotmos_acme_store *store,
                                      rotmos_error *error);
rotmos_status rotmos_acme_store_persist(struct rotmos_acme_store *store,
                                        rotmos_server *server,
                                        const rotmos_process_context *process,
                                        const char *domain,
                                        rotmos_error *error);
rotmos_status rotmos_acme_store_publish(struct rotmos_acme_store *store,
                                        rotmos_error *error);
rotmos_status rotmos_acme_store_recover(struct rotmos_acme_store *store,
                                        rotmos_server *server,
                                        const rotmos_process_context *process,
                                        rotmos_error *error);
rotmos_status rotmos_acme_store_active(struct rotmos_acme_store *store,
                                       char **out, rotmos_error *error);

/* Filesystem operations are confined to the declared root/private directory. */
char *rotmos_acme_path(const char *directory, const char *leaf);
rotmos_status rotmos_acme_directory(const char *path, int create,
                                    int private_mode, rotmos_error *error);
rotmos_status rotmos_acme_snapshot(struct rotmos_acme_store *store,
                                   const char *source, const char *prefix,
                                   char **out, rotmos_error *error);
rotmos_status rotmos_acme_check(struct rotmos_acme_store *store,
                                const char *directory, rotmos_error *error);
rotmos_status rotmos_acme_select(struct rotmos_acme_store *store,
                                 const char *name, const char *generation,
                                 rotmos_error *error);
rotmos_status rotmos_acme_selected(struct rotmos_acme_store *store,
                                   const char *name, char **out,
                                   rotmos_error *error);
rotmos_status rotmos_acme_sweep(struct rotmos_acme_store *store,
                                rotmos_error *error);
int rotmos_acme_remove(struct rotmos_acme_store *store, const char *path);
rotmos_status rotmos_acme_sync(int descriptor, rotmos_error *error);

#endif
