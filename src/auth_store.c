#define _POSIX_C_SOURCE 200809L
#include "auth_json.h"
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void record_path(char out[67], char type, const char *digest)
{
    snprintf(out, 67, "%c-%s", type, digest);
}

static int subject_path(char out[67], char type, const char *scope,
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

static knurl_status user(struct auth_transaction *tx, const char *subject,
                         struct auth_record *record, knurl_error *error)
{
    char path[67];
    if (!subject_path(path, 'u', "", subject))
        return failed(error);
    return knurl_auth_json_read(tx, path, record, error);
}

static knurl_status current(struct auth_transaction *tx, const char *subject,
                            const char *revision, knurl_error *error)
{
    struct auth_record account;
    knurl_status status;
    if (*revision == 0)
        return KNURL_OK;
    status = user(tx, subject, &account, error);
    if (status == KNURL_OK &&
        (account.disabled || strcmp(account.revision, revision) != 0))
        return KNURL_EOF;
    return status;
}

static knurl_status read_user(struct auth_transaction *tx,
                              knurl_auth_operation op, knurl_auth_exchange *x,
                              knurl_error *error)
{
    struct auth_record record;
    knurl_status status = user(tx, x->subject, &record, error);
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

static knurl_status throttle(struct auth_transaction *tx, char kind,
                             const char *scope, const char *key,
                             lonejson_int64 now, long max, knurl_error *error)
{
    char path[67];
    struct auth_record record;
    knurl_status status;
    if (!subject_path(path, kind, scope, key))
        return failed(error);
    status = knurl_auth_json_read(tx, path, &record, error);
    if (status != KNURL_OK && status != KNURL_EOF)
        return status;
    if (now >= record.expires) {
        record.attempts = 0;
        record.expires = now + 60;
    }
    if (record.attempts >= max)
        return KNURL_LIMIT;
    ++record.attempts;
    return knurl_auth_json_write(tx, path, &record, error);
}

static knurl_status totp(struct auth_transaction *tx, knurl_auth_exchange *x,
                         lonejson_int64 now, knurl_error *error)
{
    char path[67];
    struct auth_record account, record;
    knurl_status status;
    status = user(tx, x->subject, &account, error);
    if (status != KNURL_OK)
        return status;
    if (account.disabled || account.seed[0] == 0 ||
        account.enrollment[0] == 0 ||
        (x->revision[0] && strcmp(x->revision, account.revision) != 0))
        return KNURL_EOF;
    if (!subject_path(path, 't', "", x->subject))
        return failed(error);
    status = knurl_auth_json_read(tx, path, &record, error);
    if (status != KNURL_OK && status != KNURL_EOF)
        return status;
    if (strcmp(record.enrollment, account.enrollment) != 0)
        record.last_step = 0;
    record.last_step =
        knurl_auth_totp_step(tx->store->key, x->subject, account.seed,
                             x->secret, now, record.last_step);
    if (record.last_step == 0)
        return KNURL_EOF;
    memcpy(record.enrollment, account.enrollment, sizeof(record.enrollment));
    return knurl_auth_json_write(tx, path, &record, error);
}

static knurl_status email(struct auth_transaction *tx, knurl_auth_operation op,
                          knurl_auth_exchange *x, lonejson_int64 now,
                          knurl_error *error)
{
    char path[67];
    struct auth_record record;
    knurl_status status;
    if (!subject_path(path, 'e', x->scope, x->subject))
        return failed(error);
    status = current(tx, x->subject, x->revision, error);
    if (status != KNURL_OK)
        return status;
    status = knurl_auth_json_read(tx, path, &record, error);
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
        return knurl_auth_json_write(tx, path, &record, error);
    }
    if (status != KNURL_OK)
        return status;
    if (record.expires <= now || record.attempts >= 5 ||
        strcmp(record.revision, x->revision) != 0 || !record.secret[0])
        return KNURL_EOF;
    if (CRYPTO_memcmp(record.secret, x->digest, 64) == 0) {
        memset(record.secret, 0, sizeof(record.secret));
        return knurl_auth_json_write(tx, path, &record, error);
    }
    ++record.attempts;
    status = knurl_auth_json_write(tx, path, &record, error);
    return status == KNURL_OK ? KNURL_EOF : status;
}

static knurl_status session(struct auth_transaction *tx,
                            knurl_auth_operation op, knurl_auth_exchange *x,
                            lonejson_int64 now, knurl_error *error)
{
    char path[67];
    struct auth_record record;
    knurl_status status;
    record_path(path, 's', x->digest);
    if (op == KNURL_AUTH_SESSION_REVOKE)
        return knurl_auth_json_delete(tx, path, error);
    if (op == KNURL_AUTH_SESSION_SAVE) {
        status = current(tx, x->subject, x->revision, error);
        if (status != KNURL_OK)
            return status;
        memset(&record, 0, sizeof(record));
        snprintf(record.subject, sizeof(record.subject), "%s", x->subject);
        memcpy(record.revision, x->revision, sizeof(record.revision));
        record.expires = now + (lonejson_int64)x->lifetime_seconds;
        return knurl_auth_json_write(tx, path, &record, error);
    }
    status = knurl_auth_json_read(tx, path, &record, error);
    if (status != KNURL_OK)
        return status;
    if (record.expires <= now)
        return KNURL_EOF;
    status = current(tx, record.subject, record.revision, error);
    if (status == KNURL_OK)
        memcpy(x->identity.subject, record.subject, sizeof(record.subject));
    return status;
}

static knurl_status machine(struct auth_transaction *tx, knurl_auth_exchange *x,
                            knurl_error *error)
{
    struct auth_record record;
    knurl_status status;
    const char *secret;
    char name[129], path[67], digest[65];
    if (!knurl_auth_basic(x->secret, name, &secret))
        return KNURL_EOF;
    if (!subject_path(path, 'c', x->policy, name) ||
        !knurl_auth_digest(tx->store->key, x->policy, name, secret, digest))
        status = failed(error);
    else {
        status = knurl_auth_json_read(tx, path, &record, error);
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
    struct auth_transaction tx;
    lonejson_int64 now = (lonejson_int64)time(NULL);
    knurl_status status;
    int locked = op != KNURL_AUTH_PASSWORD && op != KNURL_AUTH_EMAIL &&
                 op != KNURL_AUTH_MACHINE_VERIFY;
    if (now < 0)
        return failed(error);
    status = knurl_auth_json_begin(json, locked, &tx, error);
    if (status != KNURL_OK)
        return status;
    switch (op) {
    case KNURL_AUTH_PASSWORD:
    case KNURL_AUTH_EMAIL:
        status = read_user(&tx, op, x, error);
        break;
    case KNURL_AUTH_MACHINE_VERIFY:
        status = machine(&tx, x, error);
        break;
    case KNURL_AUTH_ATTEMPT:
        status = throttle(&tx, 'p', "", x->peer, now, 60, error);
        if (status == KNURL_OK)
            status = throttle(&tx, 'a', "", x->subject, now, 12, error);
        break;
    case KNURL_AUTH_TOTP:
        status = totp(&tx, x, now, error);
        break;
    case KNURL_AUTH_EMAIL_SAVE:
    case KNURL_AUTH_EMAIL_CONSUME:
        status = email(&tx, op, x, now, error);
        break;
    case KNURL_AUTH_SESSION_SAVE:
    case KNURL_AUTH_SESSION_FIND:
    case KNURL_AUTH_SESSION_REVOKE:
        status = session(&tx, op, x, now, error);
        break;
    default:
        status = KNURL_INVALID;
        break;
    }
    return knurl_auth_json_end(&tx, status, error);
}

knurl_status knurl_auth_json_admin(knurl_auth_json *json, knurl_auth_admin op,
                                   const char *subject, const char *value,
                                   knurl_error *error)
{
    struct auth_transaction tx;
    struct auth_record record;
    char path[67], encoded[97];
    size_t length = value != NULL ? strlen(value) : 0;
    knurl_status status;
    if (json == NULL || !knurl_auth_name(subject) ||
        op < KNURL_AUTH_USER_PASSWORD || op > KNURL_AUTH_USER_DISABLE ||
        (op == KNURL_AUTH_USER_PASSWORD && (length < 15 || length > 1024)) ||
        (op == KNURL_AUTH_USER_EMAIL && !knurl_auth_email_address(value)) ||
        ((op == KNURL_AUTH_USER_ENABLE || op == KNURL_AUTH_USER_DISABLE) &&
         value != NULL))
        return knurl_fail(error, KNURL_INVALID, "auth_admin",
                          "invalid account, credential or operation");
    if (!subject_path(path, 'u', "", subject) ||
        (op == KNURL_AUTH_USER_PASSWORD &&
         !knurl_auth_password_make(value, encoded)))
        return failed(error);
    status = knurl_auth_json_begin(json, 1, &tx, error);
    if (status == KNURL_OK)
        status = knurl_auth_json_read(&tx, path, &record, error);
    if (status == KNURL_EOF &&
        (op == KNURL_AUTH_USER_DISABLE || op == KNURL_AUTH_USER_ENABLE))
        status = knurl_fail(error, KNURL_INVALID, "auth_admin",
                            "account does not exist");
    if (status == KNURL_OK || status == KNURL_EOF) {
        if (!knurl_auth_random_hex(record.revision, 16)) {
            status = failed(error);
        } else {
            snprintf(record.subject, sizeof(record.subject), "%s", subject);
            switch (op) {
            case KNURL_AUTH_USER_PASSWORD:
                memcpy(record.secret, encoded, sizeof(encoded));
                break;
            case KNURL_AUTH_USER_EMAIL:
                snprintf(record.email, sizeof(record.email), "%s", value);
                break;
            case KNURL_AUTH_USER_DISABLE:
            case KNURL_AUTH_USER_ENABLE:
                record.disabled = op == KNURL_AUTH_USER_DISABLE;
                break;
            }
            status = knurl_auth_json_write(&tx, path, &record, error);
        }
    }
    OPENSSL_cleanse(encoded, sizeof(encoded));
    status = knurl_auth_json_end(&tx, status, error);
    if (status == KNURL_OK)
        knurl_error_clear(error);
    return status;
}

knurl_status knurl_auth_json_issue(knurl_auth_json *json, const char *policy,
                                   const char *subject, char out[65],
                                   knurl_error *error)
{
    struct auth_transaction tx;
    struct auth_record record;
    char path[67];
    knurl_status status;
    if (out != NULL)
        out[0] = 0;
    if (json == NULL || !knurl_auth_policy_name(policy) ||
        !knurl_auth_name(subject) || out == NULL)
        return knurl_fail(
            error, KNURL_INVALID, "auth_issue",
            "store, policy, client identifier and secret output are required");
    memset(&record, 0, sizeof(record));
    if (!subject_path(path, 'c', policy, subject) ||
        !knurl_auth_random_hex(out, 32) ||
        !knurl_auth_digest(json->key, policy, subject, out, record.secret)) {
        OPENSSL_cleanse(out, 65);
        return failed(error);
    }
    snprintf(record.subject, sizeof(record.subject), "%s", subject);
    snprintf(record.policy, sizeof(record.policy), "%s", policy);
    status = knurl_auth_json_begin(json, 1, &tx, error);
    if (status == KNURL_OK)
        status = knurl_auth_json_write(&tx, path, &record, error);
    status = knurl_auth_json_end(&tx, status, error);
    if (status == KNURL_OK)
        knurl_error_clear(error);
    else
        OPENSSL_cleanse(out, 65);
    return status;
}

knurl_status knurl_auth_json_revoke(knurl_auth_json *json, const char *policy,
                                    const char *subject, knurl_error *error)
{
    struct auth_transaction tx;
    char path[67];
    knurl_status status;
    if (json == NULL || !knurl_auth_policy_name(policy) ||
        !knurl_auth_name(subject))
        return knurl_fail(error, KNURL_INVALID, "auth_revoke",
                          "store, policy and client identifier are required");
    if (!subject_path(path, 'c', policy, subject))
        return failed(error);
    status = knurl_auth_json_begin(json, 1, &tx, error);
    if (status == KNURL_OK)
        status = knurl_auth_json_delete(&tx, path, error);
    status = knurl_auth_json_end(&tx, status, error);
    if (status == KNURL_OK)
        knurl_error_clear(error);
    return status;
}

static void enrollment_uri(const char *subject, const char seed[33],
                           char out[KNURL_AUTH_TOTP_URI_CAPACITY])
{
    char label[385], *p = label;
    for (; *subject != 0; ++subject) {
        if (*subject == '@' || *subject == '+') {
            *p++ = '%';
            *p++ = *subject == '@' ? '4' : '2';
            *p++ = *subject == '@' ? '0' : 'B';
        } else {
            *p++ = *subject;
        }
    }
    *p = 0;
    snprintf(out, KNURL_AUTH_TOTP_URI_CAPACITY,
             "otpauth://totp/Knurl:%s?secret=%s&issuer=Knurl", label, seed);
}

knurl_status knurl_auth_json_enroll_totp(knurl_auth_json *json,
                                         const char *subject, int rotate,
                                         char out[KNURL_AUTH_TOTP_URI_CAPACITY],
                                         knurl_error *error)
{
    struct auth_transaction tx;
    struct auth_record record;
    char path[67], seed[33], encrypted[123];
    knurl_status status;
    if (out != NULL)
        out[0] = 0;
    if (json == NULL || !knurl_auth_name(subject) || out == NULL)
        return knurl_fail(
            error, KNURL_INVALID, "auth_enroll_totp",
            "store, account and enrollment URI output are required");
    if (!subject_path(path, 'u', "", subject) ||
        !knurl_auth_seed_generate(seed) ||
        !knurl_auth_seed_seal(json->key, subject, seed, encrypted)) {
        OPENSSL_cleanse(seed, sizeof(seed));
        return failed(error);
    }
    status = knurl_auth_json_begin(json, 1, &tx, error);
    if (status == KNURL_OK)
        status = knurl_auth_json_read(&tx, path, &record, error);
    if (status == KNURL_OK && record.seed[0] != 0 && !rotate)
        status =
            knurl_fail(error, KNURL_CONFLICT, "auth_enroll_totp",
                       "TOTP is already enrolled; request rotation explicitly");
    if (status == KNURL_OK || status == KNURL_EOF) {
        if (!knurl_auth_random_hex(record.revision, 16) ||
            !knurl_auth_random_hex(record.enrollment, 16)) {
            status = failed(error);
        } else {
            snprintf(record.subject, sizeof(record.subject), "%s", subject);
            snprintf(record.seed, sizeof(record.seed), "%s", encrypted);
            status = knurl_auth_json_write(&tx, path, &record, error);
        }
    }
    status = knurl_auth_json_end(&tx, status, error);
    if (status == KNURL_OK) {
        enrollment_uri(subject, seed, out);
        knurl_error_clear(error);
    }
    OPENSSL_cleanse(seed, sizeof(seed));
    return status;
}
