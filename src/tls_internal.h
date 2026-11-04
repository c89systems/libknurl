#ifndef ROTMOS_TLS_INTERNAL_H
#define ROTMOS_TLS_INTERNAL_H

#include "server_internal.h"
#include <openssl/ssl.h>

struct rotmos_tls_prepared {
    SSL_CTX **contexts;
    size_t count;
};

rotmos_status rotmos_tls_material_read(const rotmos_tls_material *material,
                                       size_t maximum, unsigned char **out,
                                       size_t *length, rotmos_error *error);
void rotmos_tls_material_clear(unsigned char *data, size_t length);
rotmos_status rotmos_tls_prepare(const rotmos_tls_config *config,
                                 const char *default_ciphers,
                                 struct rotmos_tls_prepared *out,
                                 rotmos_error *error);
void rotmos_tls_prepared_clear(struct rotmos_tls_prepared *prepared);

#endif
