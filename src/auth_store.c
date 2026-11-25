#define _POSIX_C_SOURCE 200809L
#include "auth_json.h"
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void record_path(char out[72], char type, const char *digest)
{
    snprintf(out, 72, "%c-%s.json", type, digest);
}

static int subject_path(char out[72], char type, const char *scope,
                        const char *subject)
{
    char digest[65];
    if (!knurl_auth_digest(NULL, scope, subject, "record", digest))
        return 0;
    record_path(out, type, digest);
    return 1;
}

static knurl_status failed(knurl_error *error)
{
    return knurl_fail(error, KNURL_BACKEND, "auth_store",
                      "cannot derive authentication state");
}

static knurl_status user(knurl_auth_json *json, const char *subject,
                         struct auth_record *record, knurl_error *error)
{
    char path[72];
    if (!subject_path(path, 'u', "", subject))
        return failed(error);
    return knurl_auth_json_read(json, path, record, error);
}

static knurl_status current(knurl_auth_json *json, const char *subject,
                            const char *revision, knurl_error *error)
{
    struct auth_record account;
    knurl_status status;
    if (*revision == 0)
        return KNURL_OK;
    status = user(json, subject, &account, error);
    if (status == KNURL_OK &&
        (account.disabled || strcmp(account.revision, revision) != 0))
        return KNURL_EOF;
    return status;
}

static knurl_status read_user(knurl_auth_json *json, knurl_auth_operation op,
                              knurl_auth_exchange *x, knurl_error *error)
{
    struct auth_record record;
    knurl_status status = user(json, x->subject, &record, error);
    if (status != KNURL_OK && status != KNURL_EOF)
        return status;
    if (op == KNURL_AUTH_PASSWORD &&
        !knurl_auth_password_check(x->secret, record.secret))
        return KNURL_EOF;
    if (status != KNURL_OK || record.disabled ||
        (op == KNURL_AUTH_EMAIL && record.email[0] == 0))
        return KNURL_EOF;
    memcpy(x->identity.subject, record.subject, sizeof(record.subject));
    memcpy(x->revision, record.revision, sizeof(x->revision));
    if (op == KNURL_AUTH_EMAIL)
        memcpy(x->email, record.email, sizeof(record.email));
    return KNURL_OK;
}

static knurl_status throttle(knurl_auth_json *json, char kind,
                             const char *scope, const char *key,
                             lonejson_int64 now, long max, knurl_error *error)
{
    char path[72];
    struct auth_record record;
    knurl_status status;
    if (!subject_path(path, kind, scope, key))
        return failed(error);
    status = knurl_auth_json_read(json, path, &record, error);
    if (status != KNURL_OK && status != KNURL_EOF)
        return status;
    if (now >= record.expires) {
        record.attempts = 0;
        record.expires = now + 60;
    }
    if (record.attempts >= max)
        return KNURL_LIMIT;
    ++record.attempts;
    return knurl_auth_json_write(json, path, &record, error);
}

static knurl_status totp(knurl_auth_json *json, knurl_auth_exchange *x,
                         lonejson_int64 now, knurl_error *error)
{
    char path[72];
    struct auth_record record;
    knurl_status status;
    if (!subject_path(path, 'u', "", x->subject))
        return failed(error);
    status = knurl_auth_json_read(json, path, &record, error);
    if (status != KNURL_OK)
        return status;
    if (record.disabled || record.seed[0] == 0 ||
        (x->revision[0] && strcmp(x->revision, record.revision) != 0))
        return KNURL_EOF;
    record.last_step = knurl_auth_totp_step(json->key, x->subject, record.seed,
                                            x->secret, now, record.last_step);
    if (record.last_step == 0)
        return KNURL_EOF;
    return knurl_auth_json_write(json, path, &record, error);
}

static knurl_status email(knurl_auth_json *json, knurl_auth_operation op,
                          knurl_auth_exchange *x, lonejson_int64 now,
                          knurl_error *error)
{
    char path[72];
    struct auth_record record;
    knurl_status status;
    if (!subject_path(path, 'e', x->scope, x->subject))
        return failed(error);
    status = current(json, x->subject, x->revision, error);
    if (status != KNURL_OK)
        return status;
    status = knurl_auth_json_read(json, path, &record, error);
    if (op == KNURL_AUTH_EMAIL_SAVE) {
        if (status != KNURL_OK && status != KNURL_EOF)
            return status;
        if (status == KNURL_OK && now < record.updated + 60)
            return KNURL_LIMIT;
        memset(&record, 0, sizeof(record));
        record.updated = now;
        record.expires = now + (lonejson_int64)x->lifetime_seconds;
        memcpy(record.secret, x->digest, 65);
        memcpy(record.revision, x->revision, sizeof(record.revision));
        return knurl_auth_json_write(json, path, &record, error);
    }
    if (status != KNURL_OK)
        return status;
    if (record.expires <= now || record.attempts >= 5 ||
        strcmp(record.revision, x->revision) != 0 || !record.secret[0])
        return KNURL_EOF;
    if (CRYPTO_memcmp(record.secret, x->digest, 64) == 0) {
        memset(record.secret, 0, sizeof(record.secret));
        return knurl_auth_json_write(json, path, &record, error);
    }
    ++record.attempts;
    status = knurl_auth_json_write(json, path, &record, error);
    return status == KNURL_OK ? KNURL_EOF : status;
}

static knurl_status session(knurl_auth_json *json, knurl_auth_operation op,
                            knurl_auth_exchange *x, lonejson_int64 now,
                            knurl_error *error)
{
    char path[72];
    struct auth_record record;
    knurl_status status;
    record_path(path, 's', x->digest);
    if (op == KNURL_AUTH_SESSION_REVOKE)
        return knurl_auth_json_delete(json, path, error);
    if (op == KNURL_AUTH_SESSION_SAVE) {
        status = current(json, x->subject, x->revision, error);
        if (status != KNURL_OK)
            return status;
        memset(&record, 0, sizeof(record));
        snprintf(record.subject, sizeof(record.subject), "%s", x->subject);
        memcpy(record.revision, x->revision, sizeof(record.revision));
        record.expires = now + (lonejson_int64)x->lifetime_seconds;
        return knurl_auth_json_write(json, path, &record, error);
    }
    status = knurl_auth_json_read(json, path, &record, error);
    if (status != KNURL_OK)
        return status;
    if (record.expires <= now)
        return KNURL_EOF;
    status = current(json, record.subject, record.revision, error);
    if (status == KNURL_OK)
        memcpy(x->identity.subject, record.subject, sizeof(record.subject));
    return status;
}

static knurl_status machine(knurl_auth_json *json, knurl_auth_exchange *x,
                            knurl_error *error)
{
    struct auth_record record;
    knurl_status status;
    const char *secret;
    char name[129], path[72], digest[65];
    if (!knurl_auth_basic(x->secret, name, &secret))
        return KNURL_EOF;
    if (!subject_path(path, 'c', "", name) ||
        !knurl_auth_digest(json->key, "client", name, secret, digest))
        status = failed(error);
    else {
        status = knurl_auth_json_read(json, path, &record, error);
        if (status == KNURL_OK && CRYPTO_memcmp(record.secret, digest, 64) != 0)
            status = KNURL_EOF;
    }
    if (status == KNURL_OK)
        memcpy(x->identity.subject, record.subject, sizeof(record.subject));
    knurl_auth_basic_clear(secret);
    return status;
}

knurl_status knurl_auth_json_exchange(knurl_auth_operation op,
                                      knurl_auth_exchange *x, void *arg,
                                      knurl_error *error)
{
    knurl_auth_json *json = arg;
    lonejson_int64 now = (lonejson_int64)time(NULL);
    knurl_status status;
    int lock;
    if (now < 0)
        return failed(error);
    if (op == KNURL_AUTH_PASSWORD || op == KNURL_AUTH_EMAIL)
        return read_user(json, op, x, error);
    if (op == KNURL_AUTH_MACHINE_VERIFY)
        return machine(json, x, error);
    lock = knurl_auth_json_lock(json, error);
    if (lock < 0)
        return KNURL_IO;
    switch (op) {
    case KNURL_AUTH_ATTEMPT:
        status = throttle(json, 'p', "", x->peer, now, 60, error);
        if (status == KNURL_OK)
            status = throttle(json, 'a', "", x->subject, now, 12, error);
        break;
    case KNURL_AUTH_TOTP:
        status = totp(json, x, now, error);
        break;
    case KNURL_AUTH_EMAIL_SAVE:
    case KNURL_AUTH_EMAIL_CONSUME:
        status = email(json, op, x, now, error);
        break;
    case KNURL_AUTH_SESSION_SAVE:
    case KNURL_AUTH_SESSION_FIND:
    case KNURL_AUTH_SESSION_REVOKE:
        status = session(json, op, x, now, error);
        break;
    default:
        status = KNURL_INVALID;
        break;
    }
    close(lock);
    return status;
}

knurl_status knurl_auth_json_admin(knurl_auth_json *json, knurl_auth_admin op,
                                   const char *subject, const char *value,
                                   knurl_error *error)
{
    struct auth_record record;
    char path[72], encoded[123];
    size_t length = value != NULL ? strlen(value) : 0;
    knurl_status status;
    int lock,
        client = op == KNURL_AUTH_CLIENT_SET || op == KNURL_AUTH_CLIENT_REVOKE;
    if (json == NULL || !knurl_auth_name(subject) ||
        op < KNURL_AUTH_USER_PASSWORD || op > KNURL_AUTH_CLIENT_REVOKE ||
        (op == KNURL_AUTH_USER_PASSWORD && (length < 15 || length > 1024)) ||
        (op == KNURL_AUTH_USER_EMAIL && !knurl_auth_email_address(value)) ||
        (op == KNURL_AUTH_USER_TOTP && (length == 0 || length > 64)) ||
        (op == KNURL_AUTH_CLIENT_SET && (length < 32 || length > 512)) ||
        ((op == KNURL_AUTH_USER_ENABLE || op == KNURL_AUTH_USER_DISABLE ||
          op == KNURL_AUTH_CLIENT_REVOKE) &&
         value != NULL))
        return knurl_fail(error, KNURL_INVALID, "auth_admin",
                          "invalid account, credential or operation");
    if (!subject_path(path, client ? 'c' : 'u', "", subject))
        return failed(error);
    if (op == KNURL_AUTH_USER_PASSWORD &&
        !knurl_auth_password_make(value, encoded))
        return failed(error);
    if (op == KNURL_AUTH_USER_TOTP &&
        !knurl_auth_seed_seal(json->key, subject, value, encoded))
        return knurl_fail(error, KNURL_INVALID, "auth_admin",
                          "expected a 16 to 32 byte base32 TOTP seed");
    if (op == KNURL_AUTH_CLIENT_SET &&
        !knurl_auth_digest(json->key, "client", subject, value, encoded))
        return failed(error);
    memset(&record, 0, sizeof(record));
    lock = knurl_auth_json_lock(json, error);
    if (lock < 0)
        return KNURL_IO;
    if (!client) {
        status = knurl_auth_json_read(json, path, &record, error);
        if (status == KNURL_EOF &&
            (op == KNURL_AUTH_USER_DISABLE || op == KNURL_AUTH_USER_ENABLE))
            status = knurl_fail(error, KNURL_INVALID, "auth_admin",
                                "account does not exist");
        if (status != KNURL_OK && status != KNURL_EOF)
            goto done;
        if (!knurl_auth_random_hex(record.revision, 16)) {
            status = failed(error);
            goto done;
        }
    }
    snprintf(record.subject, sizeof(record.subject), "%s", subject);
    switch (op) {
    case KNURL_AUTH_USER_PASSWORD:
    case KNURL_AUTH_CLIENT_SET:
        snprintf(record.secret, sizeof(record.secret), "%s", encoded);
        break;
    case KNURL_AUTH_USER_EMAIL:
        snprintf(record.email, sizeof(record.email), "%s", value);
        break;
    case KNURL_AUTH_USER_TOTP:
        /* Reapplying the same seed must never reset replay protection. */
        snprintf(record.seed, sizeof(record.seed), "%s", encoded);
        break;
    case KNURL_AUTH_USER_DISABLE:
    case KNURL_AUTH_USER_ENABLE:
        record.disabled = op == KNURL_AUTH_USER_DISABLE;
        break;
    case KNURL_AUTH_CLIENT_REVOKE:
        status = knurl_auth_json_delete(json, path, error);
        goto done;
    }
    status = knurl_auth_json_write(json, path, &record, error);
done:
    close(lock);
    OPENSSL_cleanse(encoded, sizeof(encoded));
    if (status == KNURL_OK)
        knurl_error_clear(error);
    return status;
}

knurl_status knurl_auth_json_issue(knurl_auth_json *json, const char *subject,
                                   char out[65], knurl_error *error)
{
    knurl_status status;
    if (out == NULL)
        return knurl_fail(error, KNURL_INVALID, "auth_admin",
                          "secret output is required");
    if (!knurl_auth_random_hex(out, 32))
        status = failed(error);
    else
        status = knurl_auth_json_admin(json, KNURL_AUTH_CLIENT_SET, subject,
                                       out, error);
    if (status != KNURL_OK)
        OPENSSL_cleanse(out, 65);
    return status;
}
