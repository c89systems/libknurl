#include "auth_internal.h"
#include "json_internal.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

struct knurl_auth_issuer {
    lonejson_oidc_jwks_cache keys;
    char *audience;
    char *scope;
    char *token_type;
};

void knurl_auth_oidc_close(struct knurl_auth_issuer *issuer)
{
    if (issuer == NULL)
        return;
    lonejson_oidc_jwks_cache_cleanup(&issuer->keys);
    free(issuer->audience);
    free(issuer->scope);
    free(issuer->token_type);
    free(issuer);
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
    if (config->issuer == NULL || config->jwks_uri == NULL ||
        strncmp(config->issuer, "https://", 8) != 0 ||
        strncmp(config->jwks_uri, "https://", 8) != 0 ||
        config->audience == NULL || config->audience[0] == 0 ||
        config->scope == NULL || config->scope[0] == 0 ||
        config->jwks_json == NULL)
        return knurl_fail(error, KNURL_INVALID, "auth_oidc",
                          "issuer, audience, JWKS URI, JWKS JSON and required "
                          "scope are needed");
    issuer = calloc(1, sizeof(*issuer));
    if (issuer == NULL)
        return knurl_fail(error, KNURL_NOMEM, "auth_oidc", "allocating issuer");
    lonejson_oidc_jwks_cache_init(&issuer->keys);
    issuer->audience = knurl_copy_text(config->audience);
    issuer->scope = knurl_copy_text(config->scope);
    issuer->token_type = knurl_copy_text(
        config->token_type != NULL ? config->token_type : "at+jwt");
    if (issuer->audience == NULL || issuer->scope == NULL ||
        issuer->token_type == NULL) {
        knurl_auth_oidc_close(issuer);
        return knurl_fail(error, KNURL_NOMEM, "auth_oidc",
                          "copying issuer configuration");
    }
    result = knurl_json_runtime(NULL, &runtime, &owned, error);
    if (result == KNURL_OK) {
        memset(&keys, 0, sizeof(keys));
        keys.issuer = config->issuer;
        keys.jwks_uri = config->jwks_uri;
        keys.now = (lonejson_int64)time(NULL);
        keys.ttl_seconds =
            315360000; /* Pinned keys are rotated by configuration. */
        keys.max_jwks_bytes = 65536;
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

knurl_status knurl_auth_oidc_verify(struct knurl_auth_issuer *issuer,
                                    knurl_auth_exchange *x, knurl_error *error)
{
    static const char *const algorithms[] = {"RS256", "PS256", "ES256"};
    static const char *const required[] = {"exp", "sub"};
    const char *name = issuer->keys.issuer, *audience = issuer->audience;
    const char *scope = issuer->scope;
    lonejson_oidc_jwks_cache_policy keys;
    lonejson_jwt_claim_policy claims;
    lonejson_oidc_bearer_validation_request request;
    lonejson_oidc_bearer_validation token;
    lonejson_auth_provider crypto;
    lonejson_status status;
    knurl_status result;
    lonejson *runtime;
    int owned;
    result = knurl_json_runtime(NULL, &runtime, &owned, error);
    if (result != KNURL_OK)
        return result;
    memset(&keys, 0, sizeof(keys));
    keys.issuer = name;
    keys.jwks_uri = issuer->keys.jwks_uri;
    keys.now = (lonejson_int64)time(NULL);
    keys.ttl_seconds = 315360000;
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
    claims.now = keys.now;
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
        status = lonejson_oidc_validate_bearer_token(runtime, &request, &token,
                                                     NULL);
        if (status == LONEJSON_STATUS_OK &&
            token.failure == LONEJSON_AUTH_FAILURE_NONE &&
            token.header.typ != NULL &&
            strcasecmp(token.header.typ, issuer->token_type) == 0 &&
            knurl_auth_name(token.claims.sub)) {
            memcpy(x->identity.subject, token.claims.sub,
                   strlen(token.claims.sub) + 1);
        } else {
            result = KNURL_EOF;
        }
    }
    lonejson_oidc_bearer_validation_cleanup(&token);
    lonejson_free(runtime);
    return result;
}
