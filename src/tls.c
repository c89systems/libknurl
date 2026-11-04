#include "tls_internal.h"

#include <openssl/pem.h>
#include <openssl/err.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int no_password(char *buffer, int size, int writing, void *userdata)
{
    (void)buffer;
    (void)size;
    (void)writing;
    (void)userdata;
    return 0;
}

static int certificates(SSL_CTX *ctx, const unsigned char *pem, size_t length,
                        int trust, int first)
{
    BIO *input;
    X509 *certificate;
    const char *cursor;
    const char *end;
    const char *limit;
    int count;
    int okay;
    int bytes;
    /* Parse every PEM block; trailing junk and malformed chains are errors. */
    cursor = (const char *)pem;
    limit = cursor + length;
    count = 0;
    while (cursor < limit) {
        while (cursor < limit && (*cursor == ' ' || *cursor == '\t' ||
                                  *cursor == '\r' || *cursor == '\n'))
            ++cursor;
        if (cursor == limit)
            break;
        if (limit - cursor < 27 ||
            memcmp(cursor, "-----BEGIN CERTIFICATE-----", 27))
            return 0;
        end = strstr(cursor, "-----END CERTIFICATE-----");
        if (end == NULL)
            return 0;
        end += 25;
        if (end > limit || end - cursor > INT_MAX)
            return 0;
        bytes = (int)(end - cursor);
        input = BIO_new_mem_buf(cursor, bytes);
        if (input == NULL)
            return 0;
        certificate = PEM_read_bio_X509(input, NULL, NULL, NULL);
        BIO_free(input);
        if (certificate == NULL)
            return 0;
        if (trust) {
            okay =
                X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), certificate);
            if (okay)
                okay = SSL_CTX_add_client_CA(ctx, certificate);
        } else if (first && count == 0) {
            okay = SSL_CTX_use_certificate(ctx, certificate);
        } else {
            okay = SSL_CTX_add1_chain_cert(ctx, certificate);
        }
        X509_free(certificate);
        if (!okay)
            return 0;
        ++count;
        cursor = end;
    }
    return count != 0;
}

static rotmos_status identity_prepare(const rotmos_tls_config *config,
                                      const rotmos_tls_identity *identity,
                                      const char *ciphers, SSL_CTX **out,
                                      rotmos_error *error)
{
    unsigned char *material[5];
    size_t lengths[5];
    const rotmos_tls_material *inputs[5];
    const unsigned char *key_data;
    const unsigned char *chain_data;
    unsigned char *split;
    size_t key_length;
    size_t chain_length;
    size_t i;
    SSL_CTX *ctx;
    BIO *key_input;
    EVP_PKEY *key;
    rotmos_status status;
    int minimum;
    int maximum;
    static const unsigned char session[] = "rotmos_tls_session";
    *out = NULL;
    inputs[0] = &identity->bundle;
    inputs[1] = &identity->certificate;
    inputs[2] = &identity->private_key;
    inputs[3] = &identity->extra_chain;
    inputs[4] = &identity->client_ca;
    for (i = 0; i < 5; ++i) {
        material[i] = NULL;
        lengths[i] = 0;
    }
    ctx = NULL;
    key = NULL;
    status = ROTMOS_OK;
    for (i = 0; i < 5 && status == ROTMOS_OK; ++i)
        status = rotmos_tls_material_read(inputs[i], config->max_material_bytes,
                                          &material[i], &lengths[i], error);
    if (status != ROTMOS_OK)
        goto done;
    chain_data = material[1];
    chain_length = lengths[1];
    key_data = material[2];
    key_length = lengths[2];
    if (material[0] != NULL) {
        split = (unsigned char *)strstr((char *)material[0],
                                        "-----BEGIN PRIVATE KEY-----");
        if (split == NULL)
            split = (unsigned char *)strstr((char *)material[0],
                                            "-----BEGIN RSA PRIVATE KEY-----");
        if (split == NULL)
            split = (unsigned char *)strstr((char *)material[0],
                                            "-----BEGIN EC PRIVATE KEY-----");
        if (split == NULL || split == material[0]) {
            status = ROTMOS_INVALID;
            goto done;
        }
        chain_data = material[0];
        chain_length = (size_t)(split - material[0]);
        key_data = split;
        key_length = lengths[0] - chain_length;
    }
    ERR_clear_error();
    ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
        status = ROTMOS_NOMEM;
        goto done;
    }
    minimum = config->version == ROTMOS_TLS_VERSION_1_3 ? TLS1_3_VERSION
                                                        : TLS1_2_VERSION;
    maximum = config->version == ROTMOS_TLS_VERSION_1_2 ? TLS1_2_VERSION
                                                        : TLS1_3_VERSION;
    if (!SSL_CTX_set_min_proto_version(ctx, minimum) ||
        !SSL_CTX_set_max_proto_version(ctx, maximum) ||
        !SSL_CTX_set_cipher_list(ctx, ciphers) ||
        !SSL_CTX_set_session_id_context(ctx, session, sizeof(session) - 1) ||
        !certificates(ctx, chain_data, chain_length, 0, 1) ||
        (lengths[3] != 0 &&
         !certificates(ctx, material[3], lengths[3], 0, 0))) {
        status = ROTMOS_INVALID;
        goto done;
    }
    key_input = BIO_new_mem_buf(key_data, (int)key_length);
    if (key_input != NULL) {
        key = PEM_read_bio_PrivateKey(key_input, NULL, no_password, NULL);
        BIO_free(key_input);
    }
    if (key == NULL || !SSL_CTX_use_PrivateKey(ctx, key) ||
        !SSL_CTX_check_private_key(ctx)) {
        status = ROTMOS_INVALID;
        goto done;
    }
    if (lengths[4] != 0) {
        if (!certificates(ctx, material[4], lengths[4], 1, 0)) {
            status = ROTMOS_INVALID;
            goto done;
        }
        SSL_CTX_set_verify(ctx,
                           SSL_VERIFY_PEER |
                               (identity->require_client_certificate
                                    ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT
                                    : 0),
                           NULL);
        SSL_CTX_set_verify_depth(ctx, 1);
    }
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION |
                                 SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
    *out = ctx;
    ctx = NULL;
done:
    EVP_PKEY_free(key);
    SSL_CTX_free(ctx);
    for (i = 0; i < 5; ++i)
        rotmos_tls_material_clear(material[i], lengths[i]);
    ERR_clear_error();
    if (status != ROTMOS_OK && (error == NULL || error->status != status))
        return rotmos_fail(error, status, "tls_identity",
                           "invalid TLS certificate, chain, private key, trust "
                           "or cipher list");
    return status;
}

rotmos_status rotmos_tls_prepare(const rotmos_tls_config *config,
                                 const char *default_ciphers,
                                 struct rotmos_tls_prepared *out,
                                 rotmos_error *error)
{
    size_t i;
    rotmos_status status;
    out->contexts = NULL;
    out->count = 0;
    if (config->identity_count == 0)
        return ROTMOS_OK;
    out->contexts = calloc(config->identity_count, sizeof(*out->contexts));
    if (out->contexts == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "tls_prepare",
                           "cannot allocate TLS identity contexts");
    out->count = config->identity_count;
    for (i = 0; i < out->count; ++i) {
        if (config->identities[i].mode != ROTMOS_TLS_MANUAL) {
            rotmos_tls_prepared_clear(out);
            return rotmos_fail(error, ROTMOS_NOT_SUPPORTED, "tls_prepare",
                               "ACME identity activation is not connected yet");
        }
        status = identity_prepare(
            config, &config->identities[i],
            config->cipher_list != NULL ? config->cipher_list : default_ciphers,
            &out->contexts[i], error);
        if (status != ROTMOS_OK) {
            rotmos_tls_prepared_clear(out);
            return status;
        }
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_tls_prepared_clear(struct rotmos_tls_prepared *prepared)
{
    size_t i;
    for (i = 0; i < prepared->count; ++i)
        SSL_CTX_free(prepared->contexts[i]);
    free(prepared->contexts);
    prepared->contexts = NULL;
    prepared->count = 0;
}
