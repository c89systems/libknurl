#include "auth_http.h"
#include "json_internal.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

struct knurl_auth_issuer {
    lonejson_oidc_jwks_cache keys;
    pthread_mutex_t lock;
    char *name;
    char *jwks_uri;
    char *ca_file;
    char *audience;
    char *scope;
    char *token_type;
    int pinned;
    lonejson_int64 retry_at;
};

void knurl_auth_oidc_close(struct knurl_auth_issuer *issuer)
{
    if (issuer == NULL)
        return;
    lonejson_oidc_jwks_cache_cleanup(&issuer->keys);
    pthread_mutex_destroy(&issuer->lock);
    free(issuer->name);
    free(issuer->jwks_uri);
    free(issuer->ca_file);
    free(issuer->audience);
    free(issuer->scope);
    free(issuer->token_type);
    free(issuer);
}

static void key_policy(const struct knurl_auth_issuer *issuer,
                       lonejson_oidc_jwks_cache_policy *keys)
{
    memset(keys, 0, sizeof(*keys));
    keys->issuer = issuer->name;
    keys->jwks_uri =
        issuer->jwks_uri != NULL ? issuer->jwks_uri : issuer->keys.jwks_uri;
    keys->now = (lonejson_int64)time(NULL);
    keys->ttl_seconds = issuer->pinned ? 315360000 : 300;
    keys->max_jwks_bytes = 65536;
}

knurl_status knurl_auth_oidc_init(const knurl_auth_oidc *config,
                                  struct knurl_auth_issuer **out,
                                  knurl_error *error)
{
    struct knurl_auth_issuer *issuer;
    lonejson_oidc_jwks_cache_policy keys;
    lonejson_status status;
    knurl_status result;
    lonejson *runtime;
    int owned;
    if (!knurl_auth_https_url(config->issuer) ||
        (config->jwks_uri != NULL && !knurl_auth_https_url(config->jwks_uri)) ||
        config->audience == NULL || config->audience[0] == 0 ||
        config->scope == NULL || config->scope[0] == 0 ||
        (config->pin_jwks && config->jwks_json == NULL) ||
        (config->jwks_json != NULL && config->jwks_uri == NULL))
        return knurl_fail(error, KNURL_INVALID, "auth_oidc",
                          "HTTPS issuer, audience and scope are required. "
                          "Seeded JWKS requires its HTTPS URI");
    issuer = calloc(1, sizeof(*issuer));
    if (issuer == NULL)
        return knurl_fail(error, KNURL_NOMEM, "auth_oidc", "allocating issuer");
    if (pthread_mutex_init(&issuer->lock, NULL) != 0) {
        free(issuer);
        return knurl_fail(error, KNURL_BACKEND, "auth_oidc",
                          "initializing signing key cache lock");
    }
    lonejson_oidc_jwks_cache_init(&issuer->keys);
    issuer->pinned = config->pin_jwks;
    issuer->name = knurl_copy_text(config->issuer);
    issuer->jwks_uri = knurl_copy_text(config->jwks_uri);
    issuer->ca_file = knurl_copy_text(config->ca_file);
    issuer->audience = knurl_copy_text(config->audience);
    issuer->scope = knurl_copy_text(config->scope);
    issuer->token_type = knurl_copy_text(config->token_type);
    if (issuer->name == NULL ||
        (config->jwks_uri != NULL && issuer->jwks_uri == NULL) ||
        (config->ca_file != NULL && issuer->ca_file == NULL) ||
        issuer->audience == NULL || issuer->scope == NULL ||
        (config->token_type != NULL && issuer->token_type == NULL)) {
        knurl_auth_oidc_close(issuer);
        return knurl_fail(error, KNURL_NOMEM, "auth_oidc",
                          "copying issuer configuration");
    }
    if (config->jwks_json == NULL) {
        *out = issuer;
        return KNURL_OK;
    }
    result = knurl_json_runtime(NULL, &runtime, &owned, error);
    if (result == KNURL_OK) {
        key_policy(issuer, &keys);
        status = lonejson_oidc_jwks_cache_update_json(
            runtime, &issuer->keys, &keys, config->jwks_json,
            strlen(config->jwks_json), NULL);
        lonejson_free(runtime);
        if (status != LONEJSON_STATUS_OK)
            result = knurl_fail(error, KNURL_INVALID, "auth_oidc",
                                "invalid JWKS document");
    }
    if (result != KNURL_OK)
        knurl_auth_oidc_close(issuer);
    else
        *out = issuer;
    return result;
}

static int refresh_keys(struct knurl_auth_issuer *issuer, lonejson *runtime,
                        lonejson_oidc_jwks_cache_policy *keys)
{
    lonejson_oidc_discovery discovery;
    lonejson_status status = LONEJSON_STATUS_OK;
    if (issuer->pinned || keys->now < issuer->retry_at)
        return 0;
    lonejson_oidc_discovery_init(&discovery);
    if (issuer->jwks_uri == NULL) {
        status = lonejson_oidc_fetch_discovery(runtime, issuer->name, 65536,
                                               &discovery, NULL);
        keys->jwks_uri = discovery.jwks_uri;
    }
    if (status == LONEJSON_STATUS_OK && knurl_auth_https_url(keys->jwks_uri))
        status = lonejson_oidc_jwks_cache_refresh(runtime, &issuer->keys, keys,
                                                  NULL);
    else
        status = LONEJSON_STATUS_INVALID_ARGUMENT;
    lonejson_oidc_discovery_cleanup(&discovery);
    key_policy(issuer, keys);
    issuer->retry_at = keys->now + 5;
    return status == LONEJSON_STATUS_OK;
}

static int access_token_type(const struct knurl_auth_issuer *issuer,
                             const char *type)
{
    if (type == NULL)
        return 0;
    if (issuer->token_type != NULL)
        return strcasecmp(type, issuer->token_type) == 0;
    return strcasecmp(type, "at+jwt") == 0 ||
           strcasecmp(type, "application/at+jwt") == 0;
}

knurl_status knurl_auth_oidc_verify(struct knurl_auth_issuer *issuer,
                                    knurl_auth_exchange *x, knurl_error *error)
{
    static const char *const algorithms[] = {"RS256", "PS256", "ES256"};
    static const char *const required[] = {"exp", "sub"};
    const char *name = issuer->name, *audience = issuer->audience;
    const char *scope = issuer->scope;
    lonejson_oidc_jwks_cache_policy keys;
    lonejson_jwt_claim_policy claims;
    lonejson_oidc_bearer_validation_request request;
    lonejson_oidc_bearer_validation token;
    lonejson_auth_provider crypto;
    lonejson_status status;
    knurl_status result;
    lonejson *runtime;
    int refreshed = 0;
    result = knurl_auth_http_runtime(issuer->ca_file, &runtime, error);
    if (result != KNURL_OK)
        return result;
    memset(&claims, 0, sizeof(claims));
    claims.accepted_algs = algorithms;
    claims.accepted_alg_count = sizeof(algorithms) / sizeof(algorithms[0]);
    claims.accepted_issuers = &name;
    claims.accepted_issuer_count = 1;
    claims.accepted_audiences = &audience;
    claims.accepted_audience_count = 1;
    claims.required_scopes = &scope;
    claims.required_scope_count = 1;
    claims.required_claims = required;
    claims.required_claim_count = sizeof(required) / sizeof(required[0]);
    claims.max_token_bytes = 16384;
    request.authorization_header = x->secret;
    request.jwks_cache = &issuer->keys;
    request.jwks_policy = &keys;
    request.claim_policy = &claims;
    lonejson_oidc_bearer_validation_init(&token);
    status = lonejson_auth_provider_init_openssl(&crypto, NULL, NULL);
    if (status == LONEJSON_STATUS_OK)
        status = lonejson_set_auth_provider(runtime, &crypto, NULL);
    if (status != LONEJSON_STATUS_OK)
        result = knurl_fail(error, KNURL_BACKEND, "auth_oidc",
                            "installing signature verifier");
    else {
        pthread_mutex_lock(&issuer->lock);
        key_policy(issuer, &keys);
        if (!lonejson_oidc_jwks_cache_is_fresh(&issuer->keys, &keys))
            refreshed = refresh_keys(issuer, runtime, &keys);
        claims.now = keys.now;
        status = lonejson_oidc_validate_bearer_token(runtime, &request, &token,
                                                     NULL);
        if (!refreshed &&
            token.failure == LONEJSON_AUTH_FAILURE_KEY_NOT_FOUND &&
            refresh_keys(issuer, runtime, &keys)) {
            lonejson_oidc_bearer_validation_cleanup(&token);
            lonejson_oidc_bearer_validation_init(&token);
            claims.now = keys.now;
            status = lonejson_oidc_validate_bearer_token(runtime, &request,
                                                         &token, NULL);
        }
        pthread_mutex_unlock(&issuer->lock);
        if (status == LONEJSON_STATUS_OK &&
            token.failure == LONEJSON_AUTH_FAILURE_NONE &&
            access_token_type(issuer, token.header.typ) &&
            token.claims.sub != NULL && token.claims.sub[0] != 0 &&
            strlen(token.claims.sub) < sizeof(x->identity.subject)) {
            memcpy(x->identity.subject, token.claims.sub,
                   strlen(token.claims.sub) + 1);
        } else if (token.failure == LONEJSON_AUTH_FAILURE_CACHE_UNAVAILABLE) {
            result = knurl_fail(error, KNURL_BACKEND, "auth_oidc",
                                "issuer signing keys unavailable");
        } else {
            result = KNURL_EOF;
        }
    }
    lonejson_oidc_bearer_validation_cleanup(&token);
    lonejson_free(runtime);
    return result;
}
