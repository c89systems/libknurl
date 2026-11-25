#include "auth_internal.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int knurl_auth_basic(const char *header, char name[129], const char **secret)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char decoded[1537];
    size_t length, plain, padding, id;
    char *colon;
    int out, ok = 0;
    *secret = NULL;
    if (strncasecmp(header, "Basic ", 6) != 0)
        return 0;
    header += 6 + strspn(header + 6, " ");
    length = strspn(header, alphabet);
    padding = strspn(header + length, "=");
    if (padding > 2 || header[length + padding] != 0 ||
        length + padding > 2048 || length == 0 || (length + padding) % 4 != 0)
        return 0;
    out = EVP_DecodeBlock(decoded, (const unsigned char *)header,
                          (int)(length + padding));
    if (out > (int)padding) {
        plain = (size_t)out - padding;
        decoded[plain] = 0;
        colon = memchr(decoded, ':', plain);
        if (colon != NULL && memchr(decoded, 0, plain) == NULL) {
            id = (size_t)(colon - (char *)decoded);
            if (id != 0 && id <= 128 && plain > id + 1 &&
                plain - id - 1 <= 512) {
                memcpy(name, decoded, id);
                name[id] = 0;
                if (knurl_auth_name(name)) {
                    *secret = knurl_copy_text(colon + 1);
                    ok = *secret != NULL;
                }
            }
        }
    }
    OPENSSL_cleanse(decoded, sizeof(decoded));
    return ok;
}

void knurl_auth_basic_clear(const char *secret)
{
    OPENSSL_cleanse((void *)secret, strlen(secret));
    free((void *)secret);
}

static int password_hash(const char *password, const unsigned char *salt,
                         unsigned char result[32])
{
    return PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, 16, 600000,
                             EVP_sha256(), 32, result) == 1;
}

int knurl_auth_password_make(const char *password, char out[97])
{
    unsigned char record[48];
    int ok = RAND_bytes(record, 16) == 1 &&
             password_hash(password, record, record + 16);
    if (ok)
        knurl_auth_hex(record, sizeof(record), out);
    OPENSSL_cleanse(record, sizeof(record));
    return ok;
}

int knurl_auth_password_check(const char *password, const char *encoded)
{
    unsigned char record[48] = {0}, result[32];
    int valid, equal;
    valid = knurl_auth_unhex(encoded, record, sizeof(record));
    equal = password_hash(password, record, result) &&
            CRYPTO_memcmp(record + 16, result, sizeof(result)) == 0;
    OPENSSL_cleanse(result, sizeof(result));
    OPENSSL_cleanse(record, sizeof(record));
    return valid && equal;
}

static size_t base32(const char *text, unsigned char out[32])
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    unsigned long bits = 0;
    unsigned int count = 0;
    size_t size = 0;
    const char *digit;
    unsigned char ch;
    while (*text != 0 && *text != '=') {
        ch = (unsigned char)*text++;
        if (ch >= 'a' && ch <= 'z')
            ch = (unsigned char)(ch - ('a' - 'A'));
        digit = strchr(alphabet, ch);
        if (digit == NULL)
            return 0;
        bits = (bits << 5) | (unsigned long)(digit - alphabet);
        count += 5;
        if (count >= 8) {
            if (size == 32)
                return 0;
            count -= 8;
            out[size++] = (unsigned char)(bits >> count);
            bits &= (1UL << count) - 1;
        }
    }
    return size >= 16 && bits == 0 && count < 5 && text[strspn(text, "=")] == 0
               ? size
               : 0;
}

/* Record format is hex(nonce || tag || seed length || ciphertext).
 * The account identifier is authenticated as associated data. */
int knurl_auth_seed_seal(const unsigned char key[32], const char *user,
                         const char *text, char out[123])
{
    unsigned char seed[32], record[61], tail_buffer[16];
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    size_t size = base32(text, seed);
    int length, tail, ok;
    record[28] = (unsigned char)size;
    ok = size != 0 && ctx != NULL && RAND_bytes(record, 12) == 1 &&
         EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, record) == 1 &&
         EVP_EncryptUpdate(ctx, NULL, &length, (const unsigned char *)user,
                           (int)strlen(user)) == 1 &&
         EVP_EncryptUpdate(ctx, record + 29, &length, seed, (int)size) == 1 &&
         length == (int)size &&
         EVP_EncryptFinal_ex(ctx, tail_buffer, &tail) == 1 && tail == 0 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, record + 12) == 1;
    if (ok)
        knurl_auth_hex(record, 29 + size, out);
    OPENSSL_cleanse(seed, sizeof(seed));
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static size_t seed_open(const unsigned char key[32], const char *user,
                        const char *text, unsigned char seed[32])
{
    unsigned char record[61], tail_buffer[16];
    EVP_CIPHER_CTX *ctx;
    size_t size = strlen(text) / 2;
    int length, tail, ok;
    if (size < 45 || size > sizeof(record) ||
        !knurl_auth_unhex(text, record, size) || record[28] != size - 29)
        return 0;
    size -= 29;
    ctx = EVP_CIPHER_CTX_new();
    ok = ctx != NULL &&
         EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, record) == 1 &&
         EVP_DecryptUpdate(ctx, NULL, &length, (const unsigned char *)user,
                           (int)strlen(user)) == 1 &&
         EVP_DecryptUpdate(ctx, seed, &length, record + 29, (int)size) == 1 &&
         length == (int)size &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, record + 12) == 1 &&
         EVP_DecryptFinal_ex(ctx, tail_buffer, &tail) == 1 && tail == 0;
    EVP_CIPHER_CTX_free(ctx);
    return ok ? size : 0;
}

static int otp(const unsigned char *seed, size_t size, lonejson_int64 step,
               const char *expected)
{
    unsigned char counter[8], code[20];
    unsigned int length;
    unsigned long value;
    size_t off, i;
    char result[7];
    for (i = 8; i != 0; --i) {
        counter[i - 1] = (unsigned char)step;
        step >>= 8;
    }
    if (HMAC(EVP_sha1(), seed, (int)size, counter, sizeof(counter), code,
             &length) == NULL ||
        length != 20)
        return 0;
    off = code[19] & 15;
    value = ((unsigned long)(code[off] & 127) << 24) |
            ((unsigned long)code[off + 1] << 16) |
            ((unsigned long)code[off + 2] << 8) | code[off + 3];
    snprintf(result, sizeof(result), "%06lu", value % 1000000UL);
    return CRYPTO_memcmp(expected, result, 6) == 0;
}

lonejson_int64 knurl_auth_totp_step(const unsigned char key[32],
                                    const char *user, const char *sealed,
                                    const char *code, lonejson_int64 now,
                                    lonejson_int64 last)
{
    unsigned char seed[32];
    lonejson_int64 step = now / 30, accepted = 0;
    size_t length;
    int offset;
    if (strlen(code) != 6)
        return 0;
    length = seed_open(key, user, sealed, seed);
    if (length != 0) {
        for (offset = -1; offset <= 1; ++offset)
            if (step + offset > last && otp(seed, length, step + offset, code))
                accepted = step + offset;
    }
    OPENSSL_cleanse(seed, sizeof(seed));
    return accepted;
}
