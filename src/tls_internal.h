#ifndef KNURL_TLS_INTERNAL_H
#define KNURL_TLS_INTERNAL_H

#include "server_internal.h"
#include <openssl/ssl.h>

struct knurl_tls_prepared {
    SSL_CTX **contexts;
    size_t count;
};

knurl_status knurl_tls_material_read(const knurl_tls_material *material,
                                     size_t maximum, unsigned char **out,
                                     size_t *length, knurl_error *error);
void knurl_tls_material_clear(unsigned char *data, size_t length);
knurl_status knurl_tls_identity_prepare(const knurl_tls_config *config,
                                        const knurl_tls_identity *identity,
                                        const char *ciphers, SSL_CTX **out,
                                        knurl_error *error);
knurl_status knurl_tls_prepare(const knurl_tls_config *config,
                               const char *default_ciphers,
                               struct knurl_tls_prepared *out,
                               knurl_error *error);
void knurl_tls_prepared_clear(struct knurl_tls_prepared *prepared);
knurl_status knurl_tls_policy_apply(SSL_CTX *policy, SSL_CTX *context,
                                    const char *ciphers, knurl_error *error);

#endif
