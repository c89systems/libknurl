#include "auth_internal.h"
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

unsigned int knurl_auth_next_factor(unsigned int policy, unsigned int factor)
{
    unsigned int next;
    for (next = KNURL_AUTH_FACTOR_EMAIL; next <= KNURL_AUTH_FACTOR_TOTP;
         next <<= 1)
        if (next > factor && (next & policy))
            return next;
    return 0;
}

knurl_auth_login knurl_auth_factor_storage(unsigned int policy)
{
    if (policy & KNURL_AUTH_FACTOR_TOTP)
        return KNURL_AUTH_LOGIN_TOTP;
    if (policy & KNURL_AUTH_FACTOR_EMAIL)
        return KNURL_AUTH_LOGIN_EMAIL;
    return KNURL_AUTH_LOGIN_PASSWORD;
}

knurl_status knurl_auth_workflow_pending(knurl_request *request,
                                         const char *workflow,
                                         unsigned int factor,
                                         knurl_error *error)
{
    char json[160];
    const char *step = factor == KNURL_AUTH_FACTOR_EMAIL      ? "email_code"
                       : factor == KNURL_AUTH_FACTOR_PASSWORD ? "password"
                                                              : "totp";
    knurl_status status;
    snprintf(json, sizeof(json),
             "{\"workflow\":\"%s\",\"step\":\"%s\",\"required\":\"%s\"}",
             workflow, step,
             factor == KNURL_AUTH_FACTOR_PASSWORD ? "password" : "code");
    status = knurl_auth_json_reply(request, 202, json, error);
    OPENSSL_cleanse(json, sizeof(json));
    return status;
}

static knurl_status failed(knurl_error *error)
{
    return knurl_fail(error, KNURL_BACKEND, "auth_workflow",
                      "cannot prepare authentication state");
}

static knurl_status email_start(knurl_job *task, struct knurl_auth_job *job,
                                const char *next, knurl_error *error)
{
    char code[7], digest[65];
    knurl_auth_exchange account, state;
    knurl_status status;
    const knurl_auth_config *config = &job->auth->config;
    memset(&account, 0, sizeof(account));
    account.subject = job->subject;
    status = knurl_auth_checked_identity(
        &account,
        knurl_auth_exchange_run(job, KNURL_AUTH_EMAIL, &account, error), error);
    if (status != KNURL_OK && status != KNURL_EOF)
        return status;
    memset(&state, 0, sizeof(state));
    state.active = status == KNURL_OK &&
                   account.email[sizeof(account.email) - 1] == 0 &&
                   knurl_auth_email_address(account.email);
    state.subject = state.active ? account.identity.subject : job->subject;
    if (state.active && config->users == NULL)
        memcpy(state.revision, account.revision, sizeof(state.revision));
    state.factor = KNURL_AUTH_FACTOR_EMAIL;
    state.next_workflow = next;
    if (!knurl_auth_email_code_make(code))
        return failed(error);
    if (!knurl_auth_digest(job->auth->email_key, next, state.subject, code,
                           digest)) {
        OPENSSL_cleanse(code, sizeof(code));
        return failed(error);
    }
    state.digest = digest;
    status = knurl_job_cancelled(task)
                 ? KNURL_CANCELLED
                 : knurl_auth_exchange_run(job, KNURL_AUTH_WORKFLOW_BEGIN,
                                           &state, error);
    if (status == KNURL_OK && state.active && !knurl_job_cancelled(task))
        status = config->send_email(account.email, code, config->email_userdata,
                                    error);
    OPENSSL_cleanse(code, sizeof(code));
    return status;
}

static knurl_status terminal(struct knurl_auth_job *job,
                             knurl_auth_exchange *state, char digest[65],
                             knurl_error *error)
{
    const knurl_auth_config *config = &job->auth->config;
    if (!knurl_auth_random_hex(job->cookie, 32))
        return failed(error);
    if (config->method == KNURL_AUTH_SESSION) {
        if (!knurl_auth_digest(NULL, job->scope, "session", job->cookie,
                               digest))
            return failed(error);
        state->session_digest = digest;
        state->lifetime_seconds = config->session_seconds;
    } else {
        knurl_auth_login storage = knurl_auth_factor_storage(config->factors);
        char id[33];
        char kind = storage == KNURL_AUTH_LOGIN_TOTP    ? 'o'
                    : storage == KNURL_AUTH_LOGIN_EMAIL ? 'n'
                                                        : 'm';
        if (!knurl_auth_random_hex(id, 16))
            return failed(error);
        snprintf(job->client_id, sizeof(job->client_id), "knurl-%c-%s", kind,
                 id);
        state->client_id = job->client_id;
        state->client_secret = job->cookie;
    }
    return KNURL_OK;
}

knurl_status knurl_auth_workflow_run(knurl_job *task,
                                     struct knurl_auth_job *job,
                                     knurl_error *error)
{
    const knurl_auth_config *config = &job->auth->config;
    knurl_auth_exchange state, identity, attempt;
    knurl_status status;
    char previous[65], next[65], code_digest[65], session_digest[65];
    unsigned int factor;
    memset(&identity, 0, sizeof(identity));
    memset(&state, 0, sizeof(state));
    if (!knurl_auth_digest(NULL, job->scope, job->path_digest,
                           job->next_workflow, next))
        return failed(error);
    factor = knurl_auth_next_factor(config->factors, 0);
    state.subject = job->subject;
    if (job->workflow[0] != 0) {
        if (!knurl_auth_digest(NULL, job->scope, job->path_digest,
                               job->workflow, previous))
            return failed(error);
        identity.workflow = previous;
        status = knurl_auth_checked_identity(
            &identity,
            knurl_auth_exchange_run(job, KNURL_AUTH_WORKFLOW_FIND, &identity,
                                    error),
            error);
        if (status != KNURL_OK)
            return status;
        factor = identity.factor;
        if ((factor != KNURL_AUTH_FACTOR_EMAIL &&
             factor != KNURL_AUTH_FACTOR_PASSWORD &&
             factor != KNURL_AUTH_FACTOR_TOTP) ||
            (config->factors & factor) == 0)
            return failed(error);
        if ((factor == KNURL_AUTH_FACTOR_PASSWORD) !=
            ((job->fields & KNURL_AUTH_FIELD_PASSWORD) != 0))
            return KNURL_EOF;
        state.workflow = previous;
        state.subject = identity.identity.subject;
        memcpy(state.revision, identity.revision, sizeof(state.revision));
    }
    memset(&attempt, 0, sizeof(attempt));
    attempt.subject = state.subject;
    status = knurl_auth_exchange_run(job, KNURL_AUTH_ATTEMPT, &attempt, error);
    if (status != KNURL_OK || knurl_job_cancelled(task))
        return status == KNURL_OK ? KNURL_CANCELLED : status;
    if (job->workflow[0] == 0 && factor == KNURL_AUTH_FACTOR_EMAIL)
        return email_start(task, job, next, error);
    if (factor == KNURL_AUTH_FACTOR_EMAIL) {
        if (!knurl_auth_digest(job->auth->email_key, previous, state.subject,
                               job->code, code_digest))
            return failed(error);
        state.digest = code_digest;
    } else if (factor == KNURL_AUTH_FACTOR_TOTP) {
        state.secret = job->code;
    } else {
        knurl_auth_exchange proof;
        memset(&proof, 0, sizeof(proof));
        proof.subject = state.subject;
        proof.secret = job->credential;
        status = knurl_auth_checked_identity(
            &proof,
            knurl_auth_exchange_run(job, KNURL_AUTH_PASSWORD, &proof, error),
            error);
        if (status != KNURL_OK)
            return status;
        if (state.workflow != NULL &&
            (strcmp(state.subject, proof.identity.subject) != 0 ||
             (config->users == NULL &&
              strcmp(state.revision, proof.revision) != 0)))
            return KNURL_EOF;
        memcpy(job->subject, proof.identity.subject, sizeof(job->subject));
        state.subject = job->subject;
        if (config->users == NULL)
            memcpy(state.revision, proof.revision, sizeof(state.revision));
    }
    state.factor = factor;
    job->next_factor = knurl_auth_next_factor(config->factors, factor);
    if (job->next_factor != 0)
        state.next_workflow = next;
    else {
        status = terminal(job, &state, session_digest, error);
        if (status != KNURL_OK)
            return status;
    }
    if (knurl_job_cancelled(task))
        return KNURL_CANCELLED;
    return knurl_auth_exchange_run(job, KNURL_AUTH_WORKFLOW_STEP, &state,
                                   error);
}
