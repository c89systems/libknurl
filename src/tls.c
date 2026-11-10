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

knurl_status knurl_tls_identity_prepare(const knurl_tls_config *config,
                                        const knurl_tls_identity *identity,
                                        const char *ciphers, SSL_CTX **out,
                                        knurl_error *error)
{
    unsigned char *material[5];
    size_t lengths[5];
    const knurl_tls_material *inputs[5];
    const unsigned char *key_data;
    const unsigned char *chain_data;
    unsigned char *split;
    size_t key_length;
    size_t chain_length;
    size_t i;
    SSL_CTX *ctx;
    BIO *key_input;
    EVP_PKEY *key;
    knurl_status status;
    int minimum;
    int maximum;
    static const unsigned char session[] = "knurl_tls_session";
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
    status = KNURL_OK;
    for (i = identity->mode == KNURL_TLS_ACME ? 3 : 0;
         i < 5 && status == KNURL_OK; ++i)
        status = knurl_tls_material_read(inputs[i], config->max_material_bytes,
                                         &material[i], &lengths[i], error);
    if (status != KNURL_OK)
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
            status = KNURL_INVALID;
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
        status = KNURL_NOMEM;
        goto done;
    }
    minimum = config->version == KNURL_TLS_VERSION_1_3 ? TLS1_3_VERSION
                                                       : TLS1_2_VERSION;
    maximum = config->version == KNURL_TLS_VERSION_1_2 ? TLS1_2_VERSION
                                                       : TLS1_3_VERSION;
    if (!SSL_CTX_set_min_proto_version(ctx, minimum) ||
        !SSL_CTX_set_max_proto_version(ctx, maximum) ||
        !SSL_CTX_set_cipher_list(ctx, ciphers) ||
        !SSL_CTX_set_session_id_context(ctx, session, sizeof(session) - 1) ||
        (identity->mode != KNURL_TLS_ACME &&
         !certificates(ctx, chain_data, chain_length, 0, 1)) ||
        (lengths[3] != 0 &&
         !certificates(ctx, material[3], lengths[3], 0, 0))) {
        status = KNURL_INVALID;
        goto done;
    }
    if (identity->mode != KNURL_TLS_ACME) {
        key_input = BIO_new_mem_buf(key_data, (int)key_length);
        if (key_input != NULL) {
            key = PEM_read_bio_PrivateKey(key_input, NULL, no_password, NULL);
            BIO_free(key_input);
        }
        if (key == NULL || !SSL_CTX_use_PrivateKey(ctx, key) ||
            !SSL_CTX_check_private_key(ctx)) {
            status = KNURL_INVALID;
            goto done;
        }
    }
    if (lengths[4] != 0) {
        if (!certificates(ctx, material[4], lengths[4], 1, 0)) {
            status = KNURL_INVALID;
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
        knurl_tls_material_clear(material[i], lengths[i]);
    ERR_clear_error();
    if (status != KNURL_OK && (error == NULL || error->status != status))
        return knurl_fail(error, status, "tls_identity",
                          "invalid TLS certificate, chain, private key, trust "
                          "or cipher list");
    return status;
}

knurl_status knurl_tls_prepare(const knurl_tls_config *config,
                               const char *default_ciphers,
                               struct knurl_tls_prepared *out,
                               knurl_error *error)
{
    size_t i;
    knurl_status status;
    out->contexts = NULL;
    out->count = 0;
    if (config->identity_count == 0)
        return KNURL_OK;
    out->contexts = calloc(config->identity_count, sizeof(*out->contexts));
    if (out->contexts == NULL)
        return knurl_fail(error, KNURL_NOMEM, "tls_prepare",
                          "cannot allocate TLS identity contexts");
    out->count = config->identity_count;
    for (i = 0; i < out->count; ++i) {
        status = knurl_tls_identity_prepare(
            config, &config->identities[i],
            config->cipher_list != NULL ? config->cipher_list : default_ciphers,
            &out->contexts[i], error);
        if (status != KNURL_OK) {
            knurl_tls_prepared_clear(out);
            return status;
        }
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_tls_policy_apply(SSL_CTX *policy, SSL_CTX *context,
                                    const char *ciphers, knurl_error *error)
{
    STACK_OF(X509) * chain;
    STACK_OF(X509_NAME) * names;
    int i;
    chain = NULL;
    if (!SSL_CTX_set_min_proto_version(context,
                                       SSL_CTX_get_min_proto_version(policy)) ||
        !SSL_CTX_set_max_proto_version(context,
                                       SSL_CTX_get_max_proto_version(policy)) ||
        !SSL_CTX_set_cipher_list(context, ciphers))
        return knurl_fail(error, KNURL_INVALID, "tls_policy",
                          "cannot apply prepared TLS protocol/cipher policy");
    SSL_CTX_set1_cert_store(context, SSL_CTX_get_cert_store(policy));
    SSL_CTX_set_verify(context, SSL_CTX_get_verify_mode(policy), NULL);
    SSL_CTX_set_verify_depth(context, SSL_CTX_get_verify_depth(policy));
    names = SSL_CTX_get_client_CA_list(policy);
    if (names != NULL) {
        names = SSL_dup_CA_list(names);
        if (names == NULL)
            return knurl_fail(error, KNURL_NOMEM, "tls_policy",
                              "cannot retain prepared client CA names");
        SSL_CTX_set_client_CA_list(context, names);
    }
    (void)SSL_CTX_get0_chain_certs(policy, &chain);
    for (i = 0; chain != NULL && i < sk_X509_num(chain); ++i)
        if (!SSL_CTX_add1_chain_cert(context, sk_X509_value(chain, i)))
            return knurl_fail(error, KNURL_NOMEM, "tls_policy",
                              "cannot retain prepared extra chain");
    return KNURL_OK;
}

void knurl_tls_prepared_clear(struct knurl_tls_prepared *prepared)
{
    size_t i;
    for (i = 0; i < prepared->count; ++i)
        SSL_CTX_free(prepared->contexts[i]);
    free(prepared->contexts);
    prepared->contexts = NULL;
    prepared->count = 0;
}
