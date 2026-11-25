#define _POSIX_C_SOURCE 200809L
#include "auth_internal.h"
#include "response_internal.h"
#include "loop_internal.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int knurl_auth_name(const char *name)
{
    size_t length;
    if (name == NULL)
        return 0;
    length = strspn(name, "abcdefghijklmnopqrstuvwxyz"
                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-@+");
    return length != 0 && length <= 128 && name[length] == 0;
}

int knurl_auth_policy_name(const char *name)
{
    return knurl_auth_name(name) && strpbrk(name, "@+") == NULL;
}

int knurl_auth_unhex(const char *text, unsigned char *data, size_t length)
{
    static const char alphabet[] = "0123456789abcdef";
    const char *a, *b;
    size_t i;
    if (text == NULL || strlen(text) != 2 * length)
        return 0;
    for (i = 0; i < length; ++i) {
        a = strchr(alphabet, text[2 * i]);
        b = strchr(alphabet, text[2 * i + 1]);
        if (a == NULL || b == NULL)
            return 0;
        data[i] = (unsigned char)((a - alphabet) * 16 + (b - alphabet));
    }
    return 1;
}

void knurl_auth_hex(const unsigned char *data, size_t length, char *out)
{
    static const char alphabet[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < length; ++i) {
        out[2 * i] = alphabet[data[i] >> 4];
        out[2 * i + 1] = alphabet[data[i] & 15];
    }
    out[2 * length] = 0;
}

int knurl_auth_random_hex(char *out, size_t length)
{
    unsigned char random[32];
    int ok = length <= sizeof(random) && RAND_bytes(random, (int)length) == 1;
    if (ok)
        knurl_auth_hex(random, length, out);
    OPENSSL_cleanse(random, sizeof(random));
    return ok;
}

int knurl_auth_digest(const unsigned char *key, const char *scope,
                      const char *subject, const char *value, char out[65])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_PKEY *mac = NULL;
    unsigned char bytes[32];
    size_t length = sizeof(bytes);
    unsigned int digest_length;
    int ok = 0;
    if (ctx == NULL)
        return 0;
    if (key != NULL)
        mac = EVP_PKEY_new_raw_private_key(EVP_PKEY_HMAC, NULL, key, 32);
    if (key != NULL) {
        ok = mac != NULL &&
             EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, mac) == 1 &&
             EVP_DigestSignUpdate(ctx, scope, strlen(scope) + 1) == 1 &&
             EVP_DigestSignUpdate(ctx, subject, strlen(subject) + 1) == 1 &&
             EVP_DigestSignUpdate(ctx, value, strlen(value)) == 1 &&
             EVP_DigestSignFinal(ctx, bytes, &length) == 1 && length == 32;
    } else {
        ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
             EVP_DigestUpdate(ctx, scope, strlen(scope) + 1) == 1 &&
             EVP_DigestUpdate(ctx, subject, strlen(subject) + 1) == 1 &&
             EVP_DigestUpdate(ctx, value, strlen(value)) == 1 &&
             EVP_DigestFinal_ex(ctx, bytes, &digest_length) == 1 &&
             digest_length == 32;
    }
    if (ok)
        knurl_auth_hex(bytes, sizeof(bytes), out);
    OPENSSL_cleanse(bytes, sizeof(bytes));
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(mac);
    return ok;
}

void knurl_auth_config_init(knurl_auth_config *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        config->method = KNURL_AUTH_SESSION;
    }
}

knurl_status knurl_auth_new(const knurl_auth_config *config, knurl_auth **out,
                            knurl_error *error)
{
    knurl_auth *auth;
    knurl_status status;
    int builtin, oidc;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "auth_new",
                          "policy configuration and output are required");
    if (config->method == KNURL_AUTH_MACHINE && config->factors != 0 &&
        config->users != NULL && config->machine_check == NULL)
        return knurl_fail(error, KNURL_INVALID, "auth_new",
                          "machine workflows with a separate users backend "
                          "require machine_check");
    builtin = config->client_id != NULL || config->client_secret != NULL;
    oidc = config->oidc.issuer != NULL || config->oidc.audience != NULL ||
           config->oidc.jwks_uri != NULL || config->oidc.jwks_json != NULL ||
           config->oidc.scope != NULL || config->oidc.token_type != NULL ||
           config->oidc.ca_file != NULL || config->oidc.pin_jwks;
    if (!knurl_auth_policy_name(config->name) ||
        (config->backend == NULL && config->json == NULL && !builtin &&
         !oidc) ||
        ((builtin || oidc) &&
         (config->method != KNURL_AUTH_MACHINE || config->backend != NULL ||
          config->json != NULL || config->factors != 0)) ||
        (builtin &&
         (oidc || !knurl_auth_name(config->client_id) ||
          config->client_secret == NULL || strlen(config->client_secret) < 32 ||
          strlen(config->client_secret) > 512)) ||
        (config->method != KNURL_AUTH_SESSION &&
         config->method != KNURL_AUTH_MACHINE) ||
        (config->session_check != NULL &&
         config->method != KNURL_AUTH_SESSION) ||
        (config->machine_check != NULL &&
         config->method != KNURL_AUTH_MACHINE) ||
        config->login < KNURL_AUTH_LOGIN_PASSWORD ||
        config->login > KNURL_AUTH_LOGIN_EMAIL ||
        (config->factors != 0 &&
         (config->login != KNURL_AUTH_LOGIN_PASSWORD ||
          config->factors >
              (KNURL_AUTH_FACTOR_EMAIL | KNURL_AUTH_FACTOR_PASSWORD |
               KNURL_AUTH_FACTOR_TOTP) ||
          config->factors == KNURL_AUTH_FACTOR_TOTP)) ||
        config->session_seconds > 2592000UL || config->smtp.timeout_ms < 0 ||
        config->smtp.timeout_ms > 10000L ||
        (((config->method == KNURL_AUTH_SESSION &&
           config->login == KNURL_AUTH_LOGIN_EMAIL) ||
          (config->factors & KNURL_AUTH_FACTOR_EMAIL)) &&
         ((config->send_email == NULL &&
           (config->smtp.url == NULL ||
            !knurl_auth_email_address(config->smtp.from) ||
            (strncmp(config->smtp.url, "smtp://", 7) &&
             strncmp(config->smtp.url, "smtps://", 8)))) ||
          (config->email_key == NULL && config->json == NULL))))
        return knurl_fail(
            error, KNURL_INVALID, "auth_new",
            "invalid policy, backend, session lifetime or email configuration");
    auth = calloc(1, sizeof(*auth));
    if (auth == NULL)
        return knurl_fail(error, KNURL_NOMEM, "auth_new", "allocating policy");
    auth->references = 1;
    auth->config = *config;
    auth->config.client_id = NULL;
    auth->config.client_secret = NULL;
    memset(&auth->config.oidc, 0, sizeof(auth->config.oidc));
    knurl_auth_json_retain(config->json);
    auth->config.email_key = NULL;
    auth->config.name = knurl_copy_text(config->name);
    auth->config.smtp.url = knurl_copy_text(config->smtp.url);
    auth->config.smtp.from = knurl_copy_text(config->smtp.from);
    auth->config.smtp.username = knurl_copy_text(config->smtp.username);
    auth->config.smtp.password = knurl_copy_text(config->smtp.password);
    auth->config.smtp.ca_file = knurl_copy_text(config->smtp.ca_file);
    if (auth->config.name == NULL ||
        (config->smtp.url && !auth->config.smtp.url) ||
        (config->smtp.from && !auth->config.smtp.from) ||
        (config->smtp.username && !auth->config.smtp.username) ||
        (config->smtp.password && !auth->config.smtp.password) ||
        (config->smtp.ca_file && !auth->config.smtp.ca_file)) {
        knurl_auth_close(auth);
        return knurl_fail(error, KNURL_NOMEM, "auth_new", "copying policy");
    }
    if (config->email_key != NULL) {
        if (!knurl_auth_unhex(config->email_key, auth->email_key, 32)) {
            knurl_auth_close(auth);
            return knurl_fail(error, KNURL_INVALID, "auth_new",
                              "email_key requires 64 lowercase hex digits");
        }
    } else if (config->json != NULL) {
        memcpy(auth->email_key, config->json->key, 32);
    }
    if (builtin &&
        !knurl_auth_digest(NULL, "client", config->client_id,
                           config->client_secret, auth->client_digest)) {
        knurl_auth_close(auth);
        return knurl_fail(error, KNURL_BACKEND, "auth_new",
                          "hashing client secret");
    }
    if (oidc) {
        status = knurl_auth_oidc_init(&config->oidc, &auth->issuer, error);
        if (status != KNURL_OK) {
            knurl_auth_close(auth);
            return status;
        }
    }
    if (auth->config.session_seconds == 0)
        auth->config.session_seconds = 28800;
    if (auth->config.backend == NULL && config->json != NULL) {
        auth->config.backend = knurl_auth_json_exchange;
        auth->config.userdata = config->json;
    }
    if (auth->config.send_email == NULL) {
        auth->config.send_email = knurl_auth_smtp_send;
        auth->config.email_userdata = &auth->config.smtp;
    }
    *out = auth;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_auth_retain(knurl_auth *auth)
{
    if (auth != NULL)
        ++auth->references;
}

void knurl_auth_close(knurl_auth *auth)
{
    if (auth == NULL || --auth->references != 0)
        return;
    knurl_auth_json_close(auth->config.json);
    knurl_auth_oidc_close(auth->issuer);
    free((void *)auth->config.name);
    free((void *)auth->config.smtp.url);
    free((void *)auth->config.smtp.from);
    free((void *)auth->config.smtp.username);
    if (auth->config.smtp.password != NULL)
        OPENSSL_cleanse((void *)auth->config.smtp.password,
                        strlen(auth->config.smtp.password));
    free((void *)auth->config.smtp.password);
    free((void *)auth->config.smtp.ca_file);
    OPENSSL_cleanse(auth->email_key, sizeof(auth->email_key));
    OPENSSL_cleanse(auth->client_digest, sizeof(auth->client_digest));
    free(auth);
}

static knurl_status attach(knurl_router *router, knurl_auth **target,
                           knurl_auth *auth, knurl_error *error)
{
    if (router->impl->frozen)
        return knurl_fail(error, KNURL_STATE, "route_auth", "router is active");
    knurl_auth_retain(auth);
    knurl_auth_close(*target);
    *target = auth;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_route_set_auth(knurl_route *route, knurl_auth *auth,
                                  knurl_error *error)
{
    if (route == NULL)
        return knurl_fail(error, KNURL_INVALID, "route_auth",
                          "route is required");
    return attach(route->impl->router, &route->impl->auth, auth, error);
}

knurl_status knurl_route_group_set_auth(knurl_route_group *group,
                                        knurl_auth *auth, knurl_error *error)
{
    if (group == NULL)
        return knurl_fail(error, KNURL_INVALID, "route_auth",
                          "group is required");
    return attach(group->impl->router, &group->impl->auth, auth, error);
}

const knurl_auth_identity *knurl_request_auth(const knurl_request *request)
{
    return request != NULL && request->impl->auth_identity.method != 0
               ? &request->impl->auth_identity
               : NULL;
}

int knurl_auth_cookie(knurl_request *request, const knurl_auth *auth,
                      char out[65])
{
    const char *start, *end;
    char name[150];
    size_t i, length;
    unsigned char bytes[32];
    snprintf(name, sizeof(name),
             KNURL_AUTH_COOKIE_PREFIX "%s=", auth->config.name);
    length = strlen(name);
    out[0] = 0;
    for (i = 0; i < request->impl->header_count; ++i) {
        if (!knurl_text_equal_case(request->impl->headers[i].name.data,
                                   "cookie"))
            continue;
        start = request->impl->headers[i].value.data;
        while (*start != 0) {
            start += strspn(start, " \t;");
            end = strchr(start, ';');
            if (end == NULL)
                end = start + strlen(start);
            if ((size_t)(end - start) >= length &&
                memcmp(start, name, length) == 0) {
                if (out[0] || (size_t)(end - start) != length + 64)
                    return 0;
                memcpy(out, start + length, 64);
                out[64] = 0;
                if (!knurl_auth_unhex(out, bytes, sizeof(bytes)))
                    return 0;
            }
            start = end;
        }
    }
    OPENSSL_cleanse(bytes, sizeof(bytes));
    return out[0] != 0;
}

knurl_status knurl_auth_reply(knurl_request *request, int status,
                              const char *challenge, const char *cookie,
                              knurl_error *error)
{
    knurl_response *response = knurl_request_response(request);
    knurl_status result;
    knurl_u64 zero = {0, 0};
    if (!request->impl->outcome.body_complete &&
        (request->impl->has_coding || request->impl->content_length.high ||
         request->impl->content_length.low))
        response->impl->close_connection = 1;
    result = knurl_response_status(response, status, error);
    if (result == KNURL_OK)
        result = knurl_response_header_set(response, "cache-control",
                                           "no-store", error);
    if (result == KNURL_OK && challenge != NULL)
        result = knurl_response_header_set(response, "www-authenticate",
                                           challenge, error);
    if (result == KNURL_OK && cookie != NULL)
        result =
            knurl_response_header_set(response, "set-cookie", cookie, error);
    if (result == KNURL_OK && status != 204)
        result = knurl_response_known_length(response, zero, error);
    if (result == KNURL_OK)
        result = knurl_response_begin(response, error);
    if (result == KNURL_OK)
        result = knurl_response_end(response, NULL, 0, error);
    return result;
}

knurl_status knurl_auth_json_reply(knurl_request *request, int code,
                                   const char *json, knurl_error *error)
{
    knurl_response *response = knurl_request_response(request);
    size_t size = strlen(json), accepted;
    knurl_u64 length = {0, 0};
    knurl_status status = knurl_response_status(response, code, error);
    length.low = (unsigned long)size;
    if (status == KNURL_OK)
        status = knurl_response_header_set(response, "cache-control",
                                           "no-store", error);
    if (status == KNURL_OK)
        status = knurl_response_header_set(response, "content-type",
                                           "application/json", error);
    if (status == KNURL_OK)
        status = knurl_response_known_length(response, length, error);
    if (status == KNURL_OK)
        status = knurl_response_begin(response, error);
    if (status == KNURL_OK) {
        status = knurl_response_write(response, json, size, &accepted, error);
        if (status == KNURL_OK && accepted != size)
            status = knurl_fail(error, KNURL_LIMIT, "auth_reply",
                                "auth response exceeds output limit");
    }
    if (status == KNURL_OK)
        status = knurl_response_end(response, NULL, 0, error);
    return status;
}

static knurl_status authorization(knurl_request *request,
                                  struct knurl_auth_job *job)
{
    size_t i, count = 0;
    knurl_bytes value;
    for (i = 0; i < request->impl->header_count; ++i) {
        if (!knurl_text_equal_case(request->impl->headers[i].name.data,
                                   "authorization"))
            continue;
        value = request->impl->headers[i].value;
        if (++count > 1 || value.length >= sizeof(job->credential))
            return KNURL_EOF;
        memcpy(job->credential, value.data, value.length);
    }
    return count == 1 ? KNURL_OK : KNURL_EOF;
}

const char *knurl_auth_challenge(const knurl_auth *auth)
{
    if (auth->config.method != KNURL_AUTH_MACHINE)
        return NULL;
    if (auth->issuer != NULL)
        return "Bearer realm=\"knurl\"";
    if (auth->config.json != NULL || auth->client_digest[0] != 0)
        return "Basic realm=\"knurl\"";
    return "Basic realm=\"knurl\", Bearer realm=\"knurl\"";
}

knurl_status knurl_auth_machine_verify(knurl_auth_exchange *x, knurl_auth *auth,
                                       knurl_error *error)
{
    char ID[129], digest[65];
    const char *secret;
    int ok;
    if (auth->config.backend != NULL)
        return auth->config.backend(KNURL_AUTH_MACHINE_VERIFY, x,
                                    auth->config.userdata, error);
    if (auth->issuer != NULL)
        return knurl_auth_oidc_verify(auth->issuer, x, error);
    if (!knurl_auth_basic(x->secret, ID, &secret))
        return KNURL_EOF;
    ok = knurl_auth_digest(NULL, "client", ID, secret, digest) &&
         CRYPTO_memcmp(digest, auth->client_digest, 64) == 0;
    knurl_auth_basic_clear(secret);
    if (!ok)
        return KNURL_EOF;
    memcpy(x->identity.subject, ID, strlen(ID) + 1);
    return KNURL_OK;
}

knurl_status knurl_auth_admit(knurl_request *request, knurl_request_plan *plan,
                              knurl_error *error)
{
    struct knurl_route_impl *route = request->impl->route->impl;
    struct knurl_auth_job *job;
    knurl_auth *auth = route->auth;
    knurl_status status;
    if (auth == NULL && route->group != NULL)
        auth = route->group->impl->auth;
    if (auth == NULL)
        return KNURL_OK;
    plan->disposition = KNURL_REQUEST_REPLY;
    if (!request->impl->tls)
        return knurl_auth_reply(request, 403, NULL, NULL, error);
    job = calloc(1, sizeof(*job));
    if (job == NULL)
        return knurl_fail(error, KNURL_NOMEM, "route_auth",
                          "allocating admission");
    job->auth = auth;
    job->request = request;
    job->admission = 1;
    if (auth->config.method == KNURL_AUTH_MACHINE)
        status = authorization(request, job);
    else
        status = knurl_auth_cookie(request, auth, job->cookie) ? KNURL_OK
                                                               : KNURL_EOF;
    if (status == KNURL_OK)
        status = knurl_auth_start(job, error);
    else
        knurl_auth_job_free(job);
    if (status == KNURL_OK) {
        plan->disposition = KNURL_REQUEST_MANAGED;
        request->impl->auth_pending_headers = 1;
        return KNURL_AGAIN;
    }
    return knurl_auth_reply(
        request, status == KNURL_EOF ? 401 : 503,
        status == KNURL_EOF ? knurl_auth_challenge(auth) : NULL, NULL, error);
}
