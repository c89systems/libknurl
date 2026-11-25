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

/* An empty session revision identifies a subject that had no local account
 * when the session was issued. Establishing TOTP creates one. */
static knurl_status session_revision(struct auth_transaction *tx,
                                     const char *subject, char revision[33],
                                     int capture, knurl_error *error)
{
    struct auth_record account;
    knurl_status status = user(tx, subject, &account, error);
    if (status == KNURL_EOF && revision[0] == 0)
        return KNURL_OK;
    if (status != KNURL_OK)
        return status;
    if (account.disabled)
        return KNURL_EOF;
    if (capture && revision[0] == 0)
        memcpy(revision, account.revision, sizeof(account.revision));
    return strcmp(account.revision, revision) == 0 ? KNURL_OK : KNURL_EOF;
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

static knurl_status totp_check(struct auth_transaction *tx, const char *subject,
                               const char *code,
                               const struct auth_record *account,
                               lonejson_int64 now, char path[67],
                               struct auth_record *record, knurl_error *error)
{
    knurl_status status;
    if (!subject_path(path, 't', "", subject))
        return failed(error);
    status = knurl_auth_json_read(tx, path, record, error);
    if (status != KNURL_OK && status != KNURL_EOF)
        return status;
    if (strcmp(record->enrollment, account->enrollment) != 0)
        record->last_step = 0;
    if (record->last_step < account->last_step)
        record->last_step = account->last_step;
    record->last_step = knurl_auth_totp_step(
        tx->store->key, subject, account->seed, code, now, record->last_step);
    if (record->last_step == 0)
        return KNURL_EOF;
    memcpy(record->enrollment, account->enrollment, sizeof(record->enrollment));
    return KNURL_OK;
}

static knurl_status totp(struct auth_transaction *tx, knurl_auth_exchange *x,
                         lonejson_int64 now, knurl_error *error)
{
    char path[67];
    struct auth_record account, record;
    knurl_status status = user(tx, x->subject, &account, error);
    if (status == KNURL_EOF && x->revision[0] == 0)
        return KNURL_CONFLICT;
    if (status != KNURL_OK)
        return status;
    if (account.disabled ||
        (x->revision[0] && strcmp(x->revision, account.revision) != 0))
        return KNURL_EOF;
    if (account.seed[0] == 0 || account.enrollment[0] == 0)
        return KNURL_CONFLICT;
    status = totp_check(tx, x->subject, x->secret, &account, now, path, &record,
                        error);
    if (status != KNURL_OK)
        return status;
    memcpy(x->revision, account.revision, sizeof(x->revision));
    return knurl_auth_json_write(tx, path, &record, error);
}

static knurl_status email_delivery(struct auth_transaction *tx,
                                   knurl_auth_operation op,
                                   knurl_auth_exchange *x, lonejson_int64 now,
                                   knurl_error *error)
{
    char path[67], reserved[67];
    struct auth_record pending, record;
    knurl_status status;
    if (!subject_path(path, 'e', x->scope, x->subject) ||
        !subject_path(reserved, 'l', x->scope, x->subject))
        return failed(error);
    status = knurl_auth_json_read(tx, reserved, &pending, error);
    if (op == KNURL_AUTH_EMAIL_FINISH) {
        if (status != KNURL_OK)
            return status;
        if (CRYPTO_memcmp(pending.secret, x->session_digest, 64) != 0)
            return KNURL_EOF;
        status = knurl_auth_json_delete(tx, reserved, error);
        if (status != KNURL_OK || !x->active)
            return status;
        if (pending.expires <= now)
            return KNURL_EOF;
        status = session_revision(tx, x->subject, pending.revision, 0, error);
        if (status != KNURL_OK)
            return status;
        memset(&record, 0, sizeof(record));
        record.updated = now;
        record.expires = now + (lonejson_int64)x->lifetime_seconds;
        memcpy(record.secret, x->digest, 65);
        memcpy(record.revision, pending.revision, sizeof(record.revision));
        return knurl_auth_json_write(tx, path, &record, error);
    }
    if (status != KNURL_OK && status != KNURL_EOF)
        return status;
    if (status == KNURL_OK && pending.expires > now)
        return KNURL_LIMIT;
    status = session_revision(tx, x->subject, x->revision, 1, error);
    if (status != KNURL_OK)
        return status;
    status = knurl_auth_json_read(tx, path, &record, error);
    if (status != KNURL_OK && status != KNURL_EOF)
        return status;
    if (status == KNURL_OK && now < record.updated + 60)
        return KNURL_LIMIT;
    memset(&pending, 0, sizeof(pending));
    pending.expires = now + 60;
    memcpy(pending.secret, x->session_digest, 65);
    memcpy(pending.revision, x->revision, sizeof(pending.revision));
    return knurl_auth_json_write(tx, reserved, &pending, error);
}

static knurl_status email(struct auth_transaction *tx, knurl_auth_exchange *x,
                          lonejson_int64 now, knurl_error *error)
{
    char path[67];
    struct auth_record record;
    knurl_status status;
    if (!subject_path(path, 'e', x->scope, x->subject))
        return failed(error);
    status = session_revision(tx, x->subject, x->revision, 1, error);
    if (status != KNURL_OK)
        return status;
    status = knurl_auth_json_read(tx, path, &record, error);
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
    char kind = x->login == KNURL_AUTH_LOGIN_TOTP    ? 'w'
                : x->login == KNURL_AUTH_LOGIN_EMAIL ? 'v'
                                                     : 's';
    record_path(path, kind, x->digest);
    if (op == KNURL_AUTH_SESSION_REVOKE)
        return knurl_auth_json_delete(tx, path, error);
    if (op == KNURL_AUTH_SESSION_SAVE) {
        status = session_revision(tx, x->subject, x->revision, 1, error);
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
    status = session_revision(tx, record.subject, record.revision, 0, error);
    if (status == KNURL_OK) {
        memcpy(x->identity.subject, record.subject, sizeof(record.subject));
        memcpy(x->revision, record.revision, sizeof(x->revision));
    }
    return status;
}

static char client_kind(const char *id)
{
    if (strlen(id) == 40 && strncmp(id, "knurl-", 6) == 0 &&
        (id[6] == 'm' || id[6] == 'n' || id[6] == 'o') && id[7] == '-' &&
        strspn(id + 8, "0123456789abcdef") == 32)
        return id[6];
    return 'c';
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
    if (!subject_path(path, client_kind(name), x->policy, name) ||
        !knurl_auth_digest(tx->store->key, x->policy, name, secret, digest))
        status = failed(error);
    else {
        status = knurl_auth_json_read(tx, path, &record, error);
        if (status == KNURL_OK && CRYPTO_memcmp(record.secret, digest, 64) != 0)
            status = KNURL_EOF;
        if (status == KNURL_OK)
            status = current(tx, record.subject, record.revision, error);
    }
    if (status == KNURL_OK)
        memcpy(x->identity.subject, record.subject, sizeof(record.subject));
    knurl_auth_basic_clear(secret);
    return status;
}

static char workflow_kind(const knurl_auth_exchange *x)
{
    if (x->login == KNURL_AUTH_LOGIN_TOTP)
        return 'h';
    if (x->login == KNURL_AUTH_LOGIN_EMAIL)
        return 'g';
    return 'f';
}

static knurl_status workflow_read(struct auth_transaction *tx,
                                  knurl_auth_exchange *x,
                                  struct auth_record *record, char path[67],
                                  lonejson_int64 now, knurl_error *error)
{
    knurl_status status;
    record_path(path, workflow_kind(x), x->workflow);
    status = knurl_auth_json_read(tx, path, record, error);
    if (status != KNURL_OK)
        return status;
    if (record->expires <= now || record->attempts >= 5 || record->updated == 0)
        return KNURL_EOF;
    if (record->disabled) {
        struct auth_record account;
        status = user(tx, record->subject, &account, error);
        return status == KNURL_EOF ? KNURL_OK : status;
    }
    return current(tx, record->subject, record->revision, error);
}

static knurl_status rollback(knurl_status status, knurl_error *error)
{
    if (status == KNURL_LIMIT || status == KNURL_EOF)
        return knurl_fail(error, KNURL_BACKEND, "auth_workflow",
                          "cannot persist completed factor and its result");
    return status;
}

static knurl_status workflow_begin(struct auth_transaction *tx,
                                   knurl_auth_exchange *x, lonejson_int64 now,
                                   knurl_error *error)
{
    struct auth_record row;
    char resend[67], next[67];
    knurl_status status;
    if (!subject_path(resend, workflow_kind(x), x->scope, x->subject))
        return failed(error);
    status = current(tx, x->subject, x->revision, error);
    if (status != KNURL_OK)
        return status;
    status = knurl_auth_json_read(tx, resend, &row, error);
    if (status != KNURL_OK && status != KNURL_EOF)
        return status;
    if (row.expires > now)
        return KNURL_LIMIT;
    memset(&row, 0, sizeof(row));
    row.expires = now + 60;
    if (x->active)
        memcpy(row.secret, x->next_workflow, 65);
    status = knurl_auth_json_write(tx, resend, &row, error);
    if (status != KNURL_OK)
        return status;
    memset(&row, 0, sizeof(row));
    row.expires = now + 600;
    row.updated = x->active ? 0 : KNURL_AUTH_FACTOR_EMAIL;
    row.disabled = !x->active;
    memcpy(row.secret, x->digest, 65);
    memcpy(row.revision, x->revision, sizeof(row.revision));
    snprintf(row.subject, sizeof(row.subject), "%s", x->subject);
    record_path(next, workflow_kind(x), x->next_workflow);
    return rollback(knurl_auth_json_write(tx, next, &row, error), error);
}

static knurl_status workflow_delivery_finish(struct auth_transaction *tx,
                                             knurl_auth_exchange *x,
                                             lonejson_int64 now,
                                             knurl_error *error)
{
    struct auth_record reservation, row;
    char resend[67], next[67];
    knurl_status status;
    if (!subject_path(resend, workflow_kind(x), x->scope, x->subject))
        return failed(error);
    status = knurl_auth_json_read(tx, resend, &reservation, error);
    if (status != KNURL_OK)
        return status;
    if (CRYPTO_memcmp(reservation.secret, x->next_workflow, 64) != 0)
        return KNURL_EOF;
    record_path(next, workflow_kind(x), x->next_workflow);
    status = x->active && reservation.expires > now
                 ? current(tx, x->subject, x->revision, error)
                 : KNURL_EOF;
    if (status == KNURL_EOF) {
        knurl_status cleanup = knurl_auth_json_delete(tx, next, error);
        if (cleanup == KNURL_OK)
            cleanup = knurl_auth_json_delete(tx, resend, error);
        return cleanup == KNURL_OK ? (x->active ? status : KNURL_OK)
                                   : rollback(cleanup, error);
    }
    if (status != KNURL_OK)
        return status;
    status = knurl_auth_json_read(tx, next, &row, error);
    if (status != KNURL_OK)
        return status;
    row.updated = KNURL_AUTH_FACTOR_EMAIL;
    row.expires = now + 600;
    reservation.expires = now + 60;
    memset(reservation.secret, 0, sizeof(reservation.secret));
    status = knurl_auth_json_write(tx, next, &row, error);
    if (status == KNURL_OK)
        status = knurl_auth_json_write(tx, resend, &reservation, error);
    return rollback(status, error);
}

static knurl_status workflow_result(struct auth_transaction *tx,
                                    knurl_auth_exchange *x,
                                    struct auth_record *row, lonejson_int64 now,
                                    knurl_error *error)
{
    struct auth_record credential;
    char path[67];
    if (x->next_workflow != NULL) {
        record_path(path, workflow_kind(x), x->next_workflow);
        row->updated = knurl_auth_next_factor(x->factors, x->factor);
        row->attempts = 0;
        memset(row->secret, 0, sizeof(row->secret));
        return knurl_auth_json_write(tx, path, row, error);
    }
    if (x->client_id == NULL) {
        knurl_auth_exchange save = *x;
        save.digest = x->session_digest;
        return session(tx, KNURL_AUTH_SESSION_SAVE, &save, now, error);
    }
    memset(&credential, 0, sizeof(credential));
    if (!subject_path(path, client_kind(x->client_id), x->policy,
                      x->client_id) ||
        !knurl_auth_digest(tx->store->key, x->policy, x->client_id,
                           x->client_secret, credential.secret))
        return failed(error);
    memcpy(credential.revision, x->revision, sizeof(credential.revision));
    snprintf(credential.subject, sizeof(credential.subject), "%s", x->subject);
    snprintf(credential.policy, sizeof(credential.policy), "%s", x->policy);
    return knurl_auth_json_write(tx, path, &credential, error);
}

static knurl_status workflow_step(struct auth_transaction *tx,
                                  knurl_auth_exchange *x, lonejson_int64 now,
                                  knurl_error *error)
{
    struct auth_record row;
    char path[67];
    knurl_status status;
    if (x->workflow != NULL) {
        status = workflow_read(tx, x, &row, path, now, error);
        if (status != KNURL_OK)
            return status;
        if (row.updated != x->factor || strcmp(row.subject, x->subject) ||
            strcmp(row.revision, x->revision))
            return KNURL_EOF;
        if (x->factor == KNURL_AUTH_FACTOR_EMAIL) {
            int matches = CRYPTO_memcmp(row.secret, x->digest, 64) == 0;
            if (!matches || row.disabled) {
                ++row.attempts;
                status = knurl_auth_json_write(tx, path, &row, error);
                return status == KNURL_OK ? KNURL_EOF : status;
            }
        } else if (row.disabled)
            return KNURL_EOF;
        if (x->factor == KNURL_AUTH_FACTOR_TOTP) {
            status = totp(tx, x, now, error);
            if (status != KNURL_OK)
                return status;
        } else if (x->next_workflow == NULL && x->client_id == NULL) {
            status = session_revision(tx, x->subject, x->revision, 1, error);
            if (status != KNURL_OK)
                return status;
        }
        status = knurl_auth_json_delete(tx, path, error);
        if (status != KNURL_OK)
            return rollback(status, error);
    } else {
        status = x->next_workflow == NULL && x->client_id == NULL
                     ? session_revision(tx, x->subject, x->revision, 1, error)
                     : current(tx, x->subject, x->revision, error);
        if (status != KNURL_OK)
            return status;
        memset(&row, 0, sizeof(row));
        row.expires = now + 600;
        snprintf(row.subject, sizeof(row.subject), "%s", x->subject);
        memcpy(row.revision, x->revision, sizeof(row.revision));
    }
    return rollback(workflow_result(tx, x, &row, now, error), error);
}

static knurl_status enrollment(struct auth_transaction *, knurl_auth_exchange *,
                               knurl_auth_operation, lonejson_int64,
                               knurl_error *);

knurl_status knurl_auth_json_exchange(knurl_auth_operation op,
                                      knurl_auth_exchange *x, void *arg,
                                      knurl_error *error)
{
    knurl_auth_json *json = arg;
    struct auth_transaction tx;
    lonejson_int64 now = (lonejson_int64)time(NULL);
    knurl_status status;
    int locked = op != KNURL_AUTH_PASSWORD && op != KNURL_AUTH_EMAIL;
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
    case KNURL_AUTH_TOTP_SESSION:
        status = totp(&tx, x, now, error);
        break;
    case KNURL_AUTH_TOTP_ENROLL:
    case KNURL_AUTH_TOTP_CONFIRM: {
        knurl_auth_exchange proof = *x;
        proof.digest = x->session_digest;
        status = session(&tx, KNURL_AUTH_SESSION_FIND, &proof, now, error);
        if (status == KNURL_OK && strcmp(proof.identity.subject, x->subject))
            status = KNURL_EOF;
        if (status == KNURL_OK)
            status = enrollment(&tx, x, op, now, error);
        break;
    }
    case KNURL_AUTH_EMAIL_SAVE:
        status = email_delivery(&tx, op, x, now, error);
        break;
    case KNURL_AUTH_EMAIL_SESSION:
        status = email(&tx, x, now, error);
        break;
    case KNURL_AUTH_EMAIL_FINISH:
        status = x->next_workflow != NULL
                     ? workflow_delivery_finish(&tx, x, now, error)
                     : email_delivery(&tx, op, x, now, error);
        break;
    case KNURL_AUTH_WORKFLOW_BEGIN:
        status = workflow_begin(&tx, x, now, error);
        break;
    case KNURL_AUTH_WORKFLOW_FIND: {
        struct auth_record row;
        char path[67];
        status = workflow_read(&tx, x, &row, path, now, error);
        if (status == KNURL_OK) {
            x->factor = (unsigned int)row.updated;
            memcpy(x->identity.subject, row.subject, sizeof(row.subject));
            memcpy(x->revision, row.revision, sizeof(row.revision));
        }
        break;
    }
    case KNURL_AUTH_WORKFLOW_STEP:
        status = workflow_step(&tx, x, now, error);
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
    if (status == KNURL_OK &&
        (op == KNURL_AUTH_TOTP_SESSION || op == KNURL_AUTH_EMAIL_SESSION)) {
        knurl_auth_exchange save = *x;
        save.digest = x->session_digest;
        status = session(&tx, KNURL_AUTH_SESSION_SAVE, &save, now, error);
        /* Discard staged factor changes even for session storage errors that
         * ordinarily commit attempt counters. */
        if (status == KNURL_LIMIT || status == KNURL_EOF)
            status = knurl_fail(error, KNURL_BACKEND, "auth_session",
                                "cannot persist completed login");
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
        !knurl_auth_name(subject) || out == NULL || client_kind(subject) != 'c')
        return knurl_fail(error, KNURL_INVALID, "auth_issue",
                          "store, policy, non-workflow client identifier and "
                          "output required");
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
    if (!subject_path(path, client_kind(subject), policy, subject))
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

static int activate_seed(struct auth_record *account, const char *subject,
                         const char *encrypted, lonejson_int64 step)
{
    if (!knurl_auth_random_hex(account->revision, 16) ||
        !knurl_auth_random_hex(account->enrollment, 16))
        return 0;
    snprintf(account->subject, sizeof(account->subject), "%s", subject);
    snprintf(account->seed, sizeof(account->seed), "%s", encrypted);
    account->last_step = step;
    return 1;
}

static knurl_status enrollment(struct auth_transaction *tx,
                               knurl_auth_exchange *x, knurl_auth_operation op,
                               lonejson_int64 now, knurl_error *error)
{
    struct auth_record account, pending, replay;
    char path[67], staged[67], step_path[67], seed[33];
    lonejson_int64 step;
    knurl_status status;
    if (!subject_path(path, 'u', "", x->subject) ||
        !subject_path(staged, 'y', "", x->subject))
        return failed(error);
    status = knurl_auth_json_read(tx, path, &account, error);
    if (status != KNURL_OK && status != KNURL_EOF)
        return status;
    if (account.disabled ||
        (x->revision[0] && strcmp(x->revision, account.revision) != 0))
        return KNURL_EOF;
    if (op == KNURL_AUTH_TOTP_CONFIRM) {
        status = knurl_auth_json_read(tx, staged, &pending, error);
        if (status != KNURL_OK)
            return status;
        if (pending.expires <= now ||
            strcmp(pending.revision, account.revision) != 0 ||
            CRYPTO_memcmp(pending.secret, x->session_digest, 64) != 0)
            return KNURL_EOF;
        step = knurl_auth_totp_step(tx->store->key, x->subject, pending.seed,
                                    x->secret, now, 0);
        if (step == 0)
            return KNURL_EOF;
        if (!activate_seed(&account, x->subject, pending.seed, step))
            return failed(error);
        status = knurl_auth_json_delete(tx, staged, error);
        if (status == KNURL_OK)
            status = knurl_auth_json_write(tx, path, &account, error);
        return rollback(status, error);
    }
    if (account.seed[0] != 0 && x->secret[0] == 0)
        return KNURL_CONFLICT;
    if (x->secret[0] != 0) {
        if (account.seed[0] == 0)
            return KNURL_EOF;
        status = totp_check(tx, x->subject, x->secret, &account, now, step_path,
                            &replay, error);
        if (status != KNURL_OK)
            return status;
    }
    memset(&pending, 0, sizeof(pending));
    if (!knurl_auth_seed_generate(seed) ||
        !knurl_auth_seed_seal(tx->store->key, x->subject, seed, pending.seed)) {
        OPENSSL_cleanse(seed, sizeof(seed));
        return failed(error);
    }
    pending.expires = now + 600;
    memcpy(pending.revision, account.revision, sizeof(pending.revision));
    memcpy(pending.secret, x->session_digest, 65);
    status = knurl_auth_json_write(tx, staged, &pending, error);
    if (status == KNURL_OK && x->secret[0] != 0) {
        /* Reserve the old seed's accepted step in the same write as the
         * pending seed. Later codes from the old seed still work. */
        account.last_step = replay.last_step;
        status =
            rollback(knurl_auth_json_write(tx, path, &account, error), error);
    }
    if (status == KNURL_OK)
        enrollment_uri(x->subject, seed, x->totp_uri);
    OPENSSL_cleanse(seed, sizeof(seed));
    return status;
}

static knurl_status enroll(struct auth_transaction *tx, const char *subject,
                           int rotate, char *out, knurl_error *error)
{
    struct auth_record record;
    char path[67], seed[33], encrypted[123];
    knurl_status status;
    if (!subject_path(path, 'u', "", subject))
        return failed(error);
    status = knurl_auth_json_read(tx, path, &record, error);
    if (status != KNURL_OK && status != KNURL_EOF)
        return status;
    if (record.seed[0] != 0 && !rotate)
        return knurl_fail(error, KNURL_CONFLICT, "auth_enroll_totp",
                          "TOTP is already enrolled");
    if (!knurl_auth_seed_generate(seed) ||
        !knurl_auth_seed_seal(tx->store->key, subject, seed, encrypted) ||
        !activate_seed(&record, subject, encrypted, 0)) {
        OPENSSL_cleanse(seed, sizeof(seed));
        return failed(error);
    }
    status = knurl_auth_json_write(tx, path, &record, error);
    if (status == KNURL_OK)
        enrollment_uri(subject, seed, out);
    OPENSSL_cleanse(seed, sizeof(seed));
    return status;
}

knurl_status knurl_auth_json_enroll_totp(knurl_auth_json *json,
                                         const char *subject, int rotate,
                                         char out[KNURL_AUTH_TOTP_URI_CAPACITY],
                                         knurl_error *error)
{
    struct auth_transaction tx;
    knurl_status status;
    if (out != NULL)
        out[0] = 0;
    if (json == NULL || !knurl_auth_name(subject) || out == NULL)
        return knurl_fail(
            error, KNURL_INVALID, "auth_enroll_totp",
            "store, account and enrollment URI output are required");
    status = knurl_auth_json_begin(json, 1, &tx, error);
    if (status == KNURL_OK)
        status = enroll(&tx, subject, rotate, out, error);
    status = knurl_auth_json_end(&tx, status, error);
    if (status == KNURL_OK)
        knurl_error_clear(error);
    else
        OPENSSL_cleanse(out, KNURL_AUTH_TOTP_URI_CAPACITY);
    return status;
}
