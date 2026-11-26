#define _POSIX_C_SOURCE 200809L
#include "auth_internal.h"
#include "kore_internal.h"
#include "loop_internal.h"
#include "logging_internal.h"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *users_id(const knurl_auth_config *config)
{
    return config->users_id != NULL ? config->users_id : "";
}

knurl_status knurl_auth_exchange_run(struct knurl_auth_job *job,
                                     knurl_auth_operation operation,
                                     knurl_auth_exchange *parameters,
                                     knurl_error *error)
{
    const knurl_auth_config *config = &job->auth->config;
    knurl_auth_backend_fn fn = config->backend;
    void *data = config->userdata;
    if ((operation == KNURL_AUTH_PASSWORD || operation == KNURL_AUTH_EMAIL) &&
        config->users != NULL) {
        fn = config->users;
        data = config->users_userdata;
    }
    parameters->policy = config->name;
    parameters->users_id = users_id(config);
    parameters->scope = job->scope;
    parameters->peer = job->peer;
    parameters->login = config->factors
                            ? knurl_auth_factor_storage(config->factors)
                            : config->login;
    parameters->factors = config->factors;
    return fn(operation, parameters, data, error);
}

knurl_status knurl_auth_checked_identity(knurl_auth_exchange *x,
                                         knurl_status status,
                                         knurl_error *error)
{
    if (status == KNURL_OK &&
        (x->identity.subject[sizeof(x->identity.subject) - 1] != 0 ||
         x->revision[sizeof(x->revision) - 1] != 0 ||
         !knurl_auth_name(x->identity.subject)))
        return knurl_fail(error, KNURL_BACKEND, "auth_backend",
                          "backend returned an invalid subject");
    return status;
}

static knurl_status session(struct knurl_auth_job *job, knurl_auth_operation op,
                            const char *subject, const char *revision,
                            knurl_error *error)
{
    char digest[65];
    knurl_auth_exchange x;
    knurl_status status;
    memset(&x, 0, sizeof(x));
    if (!knurl_auth_digest(NULL, job->scope, "session", job->cookie, digest))
        return knurl_fail(error, KNURL_BACKEND, "auth_session",
                          "hashing session");
    x.subject = subject;
    if (revision != NULL && job->auth->config.users == NULL)
        memcpy(x.revision, revision, sizeof(x.revision));
    x.digest = digest;
    x.lifetime_seconds = job->auth->config.session_seconds;
    status = knurl_auth_exchange_run(job, op, &x, error);
    if (op == KNURL_AUTH_SESSION_FIND) {
        status = knurl_auth_checked_identity(&x, status, error);
        if (status == KNURL_OK) {
            const knurl_auth_config *config = &job->auth->config;
            x.identity.method = KNURL_AUTH_SESSION;
            snprintf(x.identity.users_id, sizeof(x.identity.users_id), "%s",
                     users_id(config));
            if (config->session_check != NULL) {
                status = config->session_check(
                    &x.identity, config->session_check_userdata, error);
                if (status != KNURL_OK && status != KNURL_EOF)
                    return knurl_fail(error, KNURL_BACKEND, "session_check",
                                      "external account check failed");
            }
            if (status == KNURL_OK) {
                job->identity = x.identity;
                memcpy(job->revision, x.revision, sizeof(job->revision));
            }
        }
    }
    return status;
}

int knurl_auth_email_code_make(char code[7])
{
    unsigned long n;
    unsigned char bytes[3];
    do {
        if (RAND_bytes(bytes, sizeof(bytes)) != 1)
            return 0;
        n = ((unsigned long)bytes[0] << 16) | ((unsigned long)bytes[1] << 8) |
            (unsigned long)bytes[2];
    } while (n >= 16000000UL);
    snprintf(code, 7, "%06lu", n % 1000000UL);
    return 1;
}

knurl_status knurl_auth_email_deliver(knurl_job *task,
                                      struct knurl_auth_job *job,
                                      knurl_auth_exchange *state,
                                      const char *address, const char *code,
                                      knurl_error *error)
{
    const knurl_auth_config *config = &job->auth->config;
    knurl_status status =
        knurl_job_cancelled(task)
            ? KNURL_CANCELLED
            : config->send_email(address, code, config->email_userdata, error);
    state->active = status == KNURL_OK;
    if (state->active)
        return knurl_auth_exchange_run(job, KNURL_AUTH_EMAIL_FINISH, state,
                                       error);
    (void)knurl_auth_exchange_run(job, KNURL_AUTH_EMAIL_FINISH, state, NULL);
    return status;
}

static knurl_status email_start(knurl_job *task, struct knurl_auth_job *job,
                                knurl_auth_exchange *x, knurl_error *error)
{
    char digest[65], reservation[65], code[7];
    knurl_status status;
    knurl_auth_exchange state;
    if (x->email[0] == 0 || x->email[sizeof(x->email) - 1] != 0)
        return KNURL_EOF;
    if (!knurl_auth_random_hex(reservation, 32) ||
        !knurl_auth_email_code_make(code))
        return knurl_fail(error, KNURL_BACKEND, "auth_email",
                          "generating email code");
    if (!knurl_auth_digest(job->auth->email_key, job->scope,
                           x->identity.subject, code, digest)) {
        OPENSSL_cleanse(code, sizeof(code));
        return knurl_fail(error, KNURL_BACKEND, "auth_email", "hashing code");
    }
    memset(&state, 0, sizeof(state));
    state.subject = x->identity.subject;
    state.digest = digest;
    state.session_digest = reservation;
    state.lifetime_seconds = 600;
    if (job->auth->config.users == NULL)
        memcpy(state.revision, x->revision, sizeof(state.revision));
    status = knurl_job_cancelled(task)
                 ? KNURL_CANCELLED
                 : knurl_auth_exchange_run(job, KNURL_AUTH_EMAIL_SAVE, &state,
                                           error);
    if (status == KNURL_OK)
        status =
            knurl_auth_email_deliver(task, job, &state, x->email, code, error);
    OPENSSL_cleanse(code, sizeof(code));
    return status;
}

static knurl_status login(knurl_job *task, struct knurl_auth_job *job,
                          knurl_error *error)
{
    knurl_auth_exchange x, factor;
    knurl_status status;
    knurl_auth_login kind = job->auth->config.login;
    char digest[65], session_digest[65];
    memset(&x, 0, sizeof(x));
    x.subject = job->subject;
    if (kind == KNURL_AUTH_LOGIN_EMAIL) {
        status = knurl_auth_checked_identity(
            &x, knurl_auth_exchange_run(job, KNURL_AUTH_EMAIL, &x, error),
            error);
        if (status != KNURL_OK || knurl_job_cancelled(task))
            return status == KNURL_OK ? KNURL_CANCELLED : status;
        if (job->endpoint == KNURL_AUTH_EMAIL_START)
            return email_start(task, job, &x, error);
    } else {
        x.secret = job->credential;
        status = knurl_auth_checked_identity(
            &x, knurl_auth_exchange_run(job, KNURL_AUTH_PASSWORD, &x, error),
            error);
    }
    if (status != KNURL_OK || knurl_job_cancelled(task))
        return status == KNURL_OK ? KNURL_CANCELLED : status;
    if (!knurl_auth_random_hex(job->cookie, 32))
        return knurl_fail(error, KNURL_BACKEND, "auth_session",
                          "generating session");
    if (kind == KNURL_AUTH_LOGIN_PASSWORD)
        return session(job, KNURL_AUTH_SESSION_SAVE, x.identity.subject,
                       x.revision, error);
    memset(&factor, 0, sizeof(factor));
    factor.subject = x.identity.subject;
    if (job->auth->config.users == NULL)
        memcpy(factor.revision, x.revision, sizeof(factor.revision));
    if (!knurl_auth_digest(NULL, job->scope, "session", job->cookie,
                           session_digest))
        return knurl_fail(error, KNURL_BACKEND, "auth_session",
                          "hashing session");
    factor.session_digest = session_digest;
    factor.lifetime_seconds = job->auth->config.session_seconds;
    if (kind == KNURL_AUTH_LOGIN_TOTP) {
        factor.secret = job->code;
        return knurl_auth_exchange_run(job, KNURL_AUTH_TOTP_SESSION, &factor,
                                       error);
    }
    if (!knurl_auth_digest(job->auth->email_key, job->scope, factor.subject,
                           job->code, digest))
        return knurl_fail(error, KNURL_BACKEND, "auth_email", "hashing code");
    factor.digest = digest;
    return knurl_auth_exchange_run(job, KNURL_AUTH_EMAIL_SESSION, &factor,
                                   error);
}

static knurl_status verify(knurl_job *task, struct knurl_auth_job *job,
                           knurl_error *error)
{
    knurl_auth_exchange x;
    knurl_status status;
    char digest[65];
    if (job->admission) {
        const knurl_auth_config *config = &job->auth->config;
        if (job->auth->config.method == KNURL_AUTH_SESSION)
            return session(job, KNURL_AUTH_SESSION_FIND, NULL, NULL, error);
        memset(&x, 0, sizeof(x));
        x.secret = job->credential;
        x.policy = job->auth->config.name;
        x.scope = job->scope;
        x.users_id = users_id(config);
        x.peer = job->peer;
        status = knurl_auth_machine_verify(&x, job->auth, error);
        if (job->auth->issuer == NULL)
            status = knurl_auth_checked_identity(&x, status, error);
        snprintf(x.identity.users_id, sizeof(x.identity.users_id), "%s",
                 users_id(config));
        if (status == KNURL_OK && config->machine_check != NULL) {
            x.identity.method = KNURL_AUTH_MACHINE;
            status = config->machine_check(
                &x.identity, config->machine_check_userdata, error);
            if (status != KNURL_OK && status != KNURL_EOF)
                status = knurl_fail(error, KNURL_BACKEND, "machine_check",
                                    "external account check failed");
        }
        if (status == KNURL_OK) {
            job->identity = x.identity;
            job->identity.method = KNURL_AUTH_MACHINE;
        }
        return status;
    }
    if (job->endpoint == KNURL_AUTH_LOGOUT)
        return job->cookie[0] != 0
                   ? session(job, KNURL_AUTH_SESSION_REVOKE, NULL, NULL, error)
                   : KNURL_OK;
    if (job->endpoint == KNURL_AUTH_WORKFLOW)
        return knurl_auth_workflow_run(task, job, error);
    if (job->endpoint == KNURL_AUTH_TOTP_ENROLL_ENDPOINT) {
        status = session(job, KNURL_AUTH_SESSION_FIND, NULL, NULL, error);
        if (status != KNURL_OK)
            return status;
        memset(&x, 0, sizeof(x));
        x.subject = job->identity.subject;
        status = knurl_auth_exchange_run(job, KNURL_AUTH_ATTEMPT, &x, error);
        if (status != KNURL_OK || knurl_job_cancelled(task))
            return status == KNURL_OK ? KNURL_CANCELLED : status;
        memset(&x, 0, sizeof(x));
        x.subject = job->identity.subject;
        x.secret = job->code;
        if (!knurl_auth_digest(NULL, job->scope, "session", job->cookie,
                               digest))
            return knurl_fail(error, KNURL_BACKEND, "auth_enroll_totp",
                              "hashing enrollment session");
        x.session_digest = digest;
        memcpy(x.revision, job->revision, sizeof(x.revision));
        status =
            knurl_auth_exchange_run(job,
                                    job->confirm_totp ? KNURL_AUTH_TOTP_CONFIRM
                                                      : KNURL_AUTH_TOTP_ENROLL,
                                    &x, error);
        if (status == KNURL_OK && !job->confirm_totp) {
            static const char safe[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                "0123456789-._~+/:?&=%@";
            if (x.totp_uri[sizeof(x.totp_uri) - 1] != 0 ||
                strncmp(x.totp_uri, "otpauth://totp/", 15) != 0 ||
                x.totp_uri[strspn(x.totp_uri, safe)] != 0)
                status = knurl_fail(error, KNURL_BACKEND, "auth_enroll_totp",
                                    "backend returned invalid TOTP URI");
            else
                memcpy(job->totp_uri, x.totp_uri, sizeof(job->totp_uri));
        }
        OPENSSL_cleanse(x.totp_uri, sizeof(x.totp_uri));
        return status;
    }
    memset(&x, 0, sizeof(x));
    x.subject = job->subject;
    status = knurl_auth_exchange_run(job, KNURL_AUTH_ATTEMPT, &x, error);
    if (status != KNURL_OK || knurl_job_cancelled(task))
        return status == KNURL_OK ? KNURL_CANCELLED : status;
    return login(task, job, error);
}

static knurl_status work(knurl_job *task, knurl_bytes arguments, void *userdata,
                         knurl_error *error)
{
    struct knurl_auth_job *job = userdata;
    (void)arguments;
    job->status = verify(task, job, error);
    return job->status == KNURL_EOF || job->status == KNURL_CONFLICT
               ? KNURL_OK
               : job->status;
}

static void finish(struct knurl_auth_job *job, knurl_status status)
{
    knurl_request *request = job->request;
    knurl_status result;
    const char *challenge = NULL, *cookie = NULL;
    char value[320];
    int code;
    if (request == NULL)
        return;
    job->request = NULL;
    request->impl->auth_job = NULL;
    request->impl->blocking_job_pending = 0;
    if (job->admission && status == KNURL_OK) {
        request->impl->auth_identity = job->identity;
        knurl_kore_headers_resume(request, 1);
        return;
    }
    if (job->endpoint == KNURL_AUTH_WORKFLOW && status == KNURL_OK) {
        if (job->next_factor != 0) {
            result = knurl_auth_workflow_pending(request, job->next_workflow,
                                                 job->next_factor, NULL);
            if (result != KNURL_OK)
                knurl_kore_request_fail(request, result, 500);
            return;
        }
        if (job->auth->config.method == KNURL_AUTH_MACHINE) {
            char json[160];
            snprintf(json, sizeof(json),
                     "{\"client_id\":\"%s\",\"client_secret\":\"%s\"}",
                     job->client_id, job->cookie);
            result = knurl_auth_json_reply(request, 201, json, NULL);
            OPENSSL_cleanse(json, sizeof(json));
            if (result != KNURL_OK)
                knurl_kore_request_fail(request, result, 500);
            return;
        }
    }
    if (!job->admission && job->endpoint == KNURL_AUTH_TOTP_ENROLL_ENDPOINT &&
        status == KNURL_OK && !job->confirm_totp) {
        char json[KNURL_AUTH_TOTP_URI_CAPACITY + 12];
        snprintf(json, sizeof(json), "{\"uri\":\"%s\"}", job->totp_uri);
        result = knurl_auth_json_reply(request, 202, json, NULL);
        OPENSSL_cleanse(json, sizeof(json));
        if (result != KNURL_OK)
            knurl_kore_request_fail(request, result, 500);
        return;
    }
    if (status == KNURL_OK || (job->endpoint == KNURL_AUTH_LOGOUT &&
                               !job->admission && status == KNURL_EOF))
        code = 204;
    else if (status == KNURL_EOF)
        code = 401;
    else if (status == KNURL_LIMIT)
        code = 429;
    else if (status == KNURL_INVALID && job->endpoint == KNURL_AUTH_WORKFLOW)
        code = 400;
    else if (status == KNURL_CONFLICT && !job->admission)
        code = job->endpoint == KNURL_AUTH_TOTP_ENROLL_ENDPOINT ? 409 : 428;
    else
        code = 503;
    if (job->admission && code == 401)
        challenge = job->auth->issuer != NULL
                        ? "Bearer realm=\"knurl\", error=\"invalid_token\""
                        : knurl_auth_challenge(job->auth);
    if (!job->admission && code == 204 &&
        job->endpoint != KNURL_AUTH_TOTP_ENROLL_ENDPOINT) {
        if (job->endpoint == KNURL_AUTH_LOGOUT)
            snprintf(value, sizeof(value),
                     KNURL_AUTH_COOKIE_PREFIX
                     "%s=; Path=/; Max-Age=0; Secure; HttpOnly; "
                     "SameSite=Strict",
                     job->auth->config.name);
        else
            snprintf(value, sizeof(value),
                     KNURL_AUTH_COOKIE_PREFIX
                     "%s=%s; Path=/; Max-Age=%lu; Secure; "
                     "HttpOnly; SameSite=Strict",
                     job->auth->config.name, job->cookie,
                     job->auth->config.session_seconds);
        cookie = value;
    }
    result = knurl_auth_reply(request, code, challenge, cookie, NULL);
    if (result != KNURL_OK)
        knurl_kore_request_fail(request, result, 500);
    else if (job->admission)
        knurl_kore_headers_resume(request, 0);
}

static void completed(knurl_job *task, const knurl_job_outcome *outcome,
                      void *userdata)
{
    struct knurl_auth_job *job = userdata;
    knurl_status status =
        outcome->cause == KNURL_OK ? job->status : outcome->cause;
    knurl_error error;
    (void)task;
    if (status > KNURL_EOF && status != KNURL_LIMIT &&
        status != KNURL_CONFLICT && status != KNURL_CANCELLED &&
        !(status == KNURL_INVALID && job->endpoint == KNURL_AUTH_WORKFLOW)) {
        (void)knurl_fail(&error, status,
                         job->admission ? "auth_admission" : "auth_endpoint",
                         knurl_status_string(status));
        if (!job->admission &&
            strcmp(outcome->error.operation, "auth_email") == 0 &&
            job->auth->config.send_email == knurl_auth_smtp_send)
            error = outcome->error;
        knurl_kore_log_failure(job->runtime, "auth", &error);
    }
    finish(job, status);
}

static void interrupted(knurl_job *task, knurl_status status, void *userdata)
{
    (void)task;
    finish(userdata, status);
}

void knurl_auth_job_free(struct knurl_auth_job *job)
{
    OPENSSL_cleanse(job, sizeof(*job));
    free(job);
}

static void retired(const knurl_job_outcome *outcome, void *userdata)
{
    struct knurl_auth_job *job = userdata;
    (void)outcome;
    knurl_auth_close(job->auth);
    knurl_auth_job_free(job);
}

void knurl_auth_disconnect(knurl_request *request)
{
    struct knurl_auth_job *job = request->impl->auth_job;
    if (job == NULL)
        return;
    request->impl->auth_job = NULL;
    request->impl->auth_pending_headers = 0;
    job->request = NULL;
    (void)knurl_job_cancel(job->job, NULL);
}

static void scope(struct knurl_auth_job *job, knurl_request *request)
{
    const char *authority = request->impl->authority.data;
    const char *id = users_id(&job->auth->config);
    size_t i, start;
    snprintf(job->scope, sizeof(job->scope), "%s/%d/%d/%u/%s%s",
             job->auth->config.name, (int)job->auth->config.login,
             (int)job->auth->config.method, job->auth->config.factors, id,
             *id != 0 ? "/" : "");
    start = strlen(job->scope);
    for (i = 0; authority[i] && start + i < sizeof(job->scope) - 1; ++i) {
        char ch = authority[i];
        job->scope[start + i] =
            ch >= 'A' && ch <= 'Z' ? (char)(ch + ('a' - 'A')) : ch;
    }
    snprintf(job->peer, sizeof(job->peer), "%s",
             request->impl->effective_address.data != NULL
                 ? (const char *)request->impl->effective_address.data
                 : "");
}

knurl_status knurl_auth_executor(knurl_request *request,
                                 enum knurl_auth_service service,
                                 knurl_executor **out, knurl_error *error)
{
    struct knurl_loop_impl *loop = knurl_request_loop(request)->impl;
    knurl_executor **slot = service == KNURL_AUTH_PROXY ? &loop->oauth2_executor
                            : service == KNURL_AUTH_OIDC ? &loop->oidc_executor
                            : service == KNURL_AUTH_MAIL ? &loop->mail_executor
                                                         : &loop->auth_executor;
    knurl_executor_config pool;
    knurl_status status = KNURL_OK;
    if (*slot == NULL) {
        knurl_executor_config_init(&pool);
        pool.max_jobs = 32;
        pool.auth_pool = 1;
        status = knurl_executor_new(knurl_request_server(request), &pool, slot,
                                    error);
    }
    *out = *slot;
    return status;
}

knurl_status knurl_auth_start(struct knurl_auth_job *job, knurl_error *error)
{
    knurl_request *request = job->request;
    knurl_executor *pool;
    knurl_job_config config;
    knurl_status status;
    int mail = !job->admission && (job->endpoint == KNURL_AUTH_EMAIL_START ||
                                   (job->endpoint == KNURL_AUTH_WORKFLOW &&
                                    job->fields == KNURL_AUTH_FIELD_USER));
    if (request->impl->authority.length > 300) {
        knurl_auth_job_free(job);
        return knurl_fail(error, KNURL_INVALID, "auth_submit",
                          "authority too long");
    }
    job->runtime = knurl_request_server(request)->impl->runtime;
    scope(job, request);
    status = knurl_auth_executor(request,
                                 job->auth->issuer != NULL ? KNURL_AUTH_OIDC
                                 : mail                    ? KNURL_AUTH_MAIL
                                                           : KNURL_AUTH_LOCAL,
                                 &pool, error);
    if (status == KNURL_OK) {
        const char *issuer = knurl_auth_oidc_issuer_name(job->auth->issuer);
        knurl_job_config_init(&config);
        config.work = work;
        config.completed = completed;
        config.interrupted = interrupted;
        config.retired = retired;
        config.userdata = job;
        config.serial_key = issuer;
        config.serial_key_size = issuer == NULL ? 0 : strlen(issuer);
        config.timeout_ms = 12000;
        status = knurl_executor_submit(pool, &config, &job->job, error);
    }
    if (status != KNURL_OK) {
        knurl_auth_job_free(job);
        return status;
    }
    knurl_auth_retain(job->auth);
    if (mail) {
        /* Acknowledge independently of account existence and delivery timing.
         */
        job->request = NULL;
    } else {
        request->impl->auth_job = job;
        request->impl->blocking_job_pending = 1;
    }
    return KNURL_OK;
}
