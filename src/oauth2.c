#define _POSIX_C_SOURCE 200809L
#include "auth_http.h"
#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define TOKEN_LIMIT 65536
#define ACCESS_LIMIT 16384

struct knurl_oauth2_client {
    knurl_oauth2_config config;
    lonejson_oauth2_token_flow flow;
    pthread_mutex_t lock;
    size_t references;
    lonejson_int64 renew_at;
    lonejson_int64 retry_at;
};

static void wipe(char *secret)
{
    if (secret != NULL)
        OPENSSL_cleanse(secret, strlen(secret));
}

void knurl_oauth2_client_retain(knurl_oauth2_client *client)
{
    if (client != NULL)
        (void)__atomic_add_fetch(&client->references, 1, __ATOMIC_RELAXED);
}

void knurl_oauth2_client_close(knurl_oauth2_client *client)
{
    if (client == NULL ||
        __atomic_sub_fetch(&client->references, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    wipe((char *)client->config.client_secret);
    free((char *)client->config.issuer);
    free((char *)client->config.token_endpoint);
    free((char *)client->config.client_id);
    free((char *)client->config.client_secret);
    free((char *)client->config.scope);
    free((char *)client->config.audience);
    free((char *)client->config.resource);
    free((char *)client->config.ca_file);
    wipe(client->flow.access_token);
    wipe(client->flow.refresh_token);
    wipe(client->flow.id_token);
    lonejson_oauth2_token_flow_cleanup(&client->flow);
    pthread_mutex_destroy(&client->lock);
    free(client);
}

knurl_status knurl_oauth2_client_new(const knurl_oauth2_config *config,
                                     knurl_oauth2_client **out,
                                     knurl_error *error)
{
    knurl_oauth2_client *client;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL || config->client_id == NULL ||
        config->client_id[0] == 0 || config->client_secret == NULL ||
        config->client_secret[0] == 0 || strlen(config->client_id) > 1024 ||
        strlen(config->client_secret) > 4096 ||
        (config->scope != NULL && strlen(config->scope) > 4096) ||
        (config->audience != NULL && strlen(config->audience) > 4096) ||
        (config->resource != NULL && strlen(config->resource) > 4096) ||
        (config->token_endpoint == NULL && config->issuer == NULL) ||
        (config->issuer != NULL && !knurl_auth_https_url(config->issuer)) ||
        (config->token_endpoint != NULL &&
         !knurl_auth_https_url(config->token_endpoint)))
        return knurl_fail(error, KNURL_INVALID, "oauth2",
                          "HTTPS endpoint or issuer and client credentials "
                          "are required");
    client = calloc(1, sizeof(*client));
    if (client == NULL)
        return knurl_fail(error, KNURL_NOMEM, "oauth2", "allocating client");
    if (pthread_mutex_init(&client->lock, NULL) != 0) {
        free(client);
        return knurl_fail(error, KNURL_BACKEND, "oauth2",
                          "initializing token cache lock");
    }
    client->references = 1;
    client->config.client_secret_post = config->client_secret_post;
    lonejson_oauth2_token_flow_init(&client->flow);
#define COPY(field)                                                            \
    do {                                                                       \
        if (config->field != NULL &&                                           \
            (client->config.field = knurl_copy_text(config->field)) == NULL)   \
            goto memory;                                                       \
    } while (0)
    COPY(issuer);
    COPY(token_endpoint);
    COPY(client_id);
    COPY(client_secret);
    COPY(scope);
    COPY(audience);
    COPY(resource);
    COPY(ca_file);
#undef COPY
    *out = client;
    knurl_error_clear(error);
    return KNURL_OK;
memory:
    knurl_oauth2_client_close(client);
    return knurl_fail(error, KNURL_NOMEM, "oauth2",
                      "copying client configuration");
}

static int append(lonejson_owned_buffer *form, const char *text)
{
    return lonejson_owned_buffer_sink(form, text, strlen(text), NULL) ==
           LONEJSON_STATUS_OK;
}

static int field(lonejson_owned_buffer *form, const char *key,
                 const char *value)
{
    char *encoded;
    int ok;
    if (value == NULL)
        return 1;
    encoded = curl_easy_escape(NULL, value, 0);
    ok = encoded != NULL && append(form, key) && append(form, encoded);
    wipe(encoded);
    curl_free(encoded);
    return ok;
}

static char *basic(const knurl_oauth2_config *config)
{
    lonejson_owned_buffer secret;
    char *line = NULL;
    lonejson_owned_buffer_init(&secret);
    if (field(&secret, "", config->client_id) &&
        field(&secret, ":", config->client_secret)) {
        line = malloc(7 + 4 * ((secret.len + 2) / 3));
        if (line != NULL) {
            memcpy(line, "Basic ", 6);
            EVP_EncodeBlock((unsigned char *)line + 6,
                            (const unsigned char *)secret.data,
                            (int)secret.len);
        }
    }
    wipe(secret.data);
    lonejson_owned_buffer_free(&secret);
    return line;
}

static int bearer(const char *value)
{
    size_t length = strlen(value), suffix;
    if (length == 0 || length > ACCESS_LIMIT)
        return 0;
    suffix =
        strspn(value, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                      "0123456789-._~+/");
    return suffix != 0 && suffix + strspn(value + suffix, "=") == length;
}

struct oauth_error {
    char error[64];
};

static int invalid_grant(lonejson *runtime, const lonejson_http_response *http)
{
    static const lonejson_field fields[] = {LONEJSON_FIELD_STRING_FIXED_REQ(
        struct oauth_error, error, "error", LONEJSON_OVERFLOW_FAIL)};
    LONEJSON_MAP_DEFINE(map, struct oauth_error, fields);
    struct oauth_error response;
    memset(&response, 0, sizeof(response));
    return http->status_code == 400 && http->body.data != NULL &&
           lonejson_parse_buffer(runtime, &map, &response, http->body.data,
                                 http->body.len, NULL) == LONEJSON_STATUS_OK &&
           strcmp(response.error, "invalid_grant") == 0;
}

static knurl_status request_token(knurl_oauth2_client *client,
                                  lonejson *runtime, int refresh,
                                  lonejson_int64 now)
{
    const knurl_oauth2_config *config = &client->config;
    const char *grant =
        refresh ? "grant_type=refresh_token" : "grant_type=client_credentials";
    lonejson_oauth2_token_response token;
    lonejson_oauth2_token_flow next;
    lonejson_owned_buffer form;
    lonejson_http_request request;
    lonejson_http_response http;
    knurl_status status = KNURL_BACKEND;
    char *authorization = NULL;
    char *borrowed = NULL;
    lonejson_int64 lead;
    lonejson_owned_buffer_init(&form);
    lonejson_http_response_init(&http);
    lonejson_oauth2_token_response_init(&token);
    lonejson_oauth2_token_flow_init(&next);
    if (!append(&form, grant) ||
        !(refresh ? field(&form, "&refresh_token=", client->flow.refresh_token)
                  : field(&form, "&scope=", config->scope) &&
                        field(&form, "&audience=", config->audience) &&
                        field(&form, "&resource=", config->resource)))
        goto done;
    if (config->client_secret_post) {
        if (!field(&form, "&client_id=", config->client_id) ||
            !field(&form, "&client_secret=", config->client_secret))
            goto done;
    } else {
        authorization = basic(config);
        if (authorization == NULL)
            goto done;
    }
    memset(&request, 0, sizeof(request));
    request.method = "POST";
    request.url = config->token_endpoint;
    request.content_type = "application/x-www-form-urlencoded";
    request.authorization = authorization;
    request.body = form.data;
    request.body_len = form.len;
    request.max_response_bytes = TOKEN_LIMIT;
    if (knurl_auth_http((void *)config->ca_file, &request, &http, NULL) !=
        LONEJSON_STATUS_OK)
        goto done;
    if (refresh && invalid_grant(runtime, &http)) {
        status = KNURL_EOF;
        goto done;
    }
    if (http.status_code != 200 ||
        lonejson_oauth2_token_response_parse_json(
            runtime, http.body.data, http.body.len, TOKEN_LIMIT, &token,
            NULL) != LONEJSON_STATUS_OK ||
        strcasecmp(token.token_type, "Bearer") != 0 ||
        !bearer(token.access_token) ||
        (token.has_expires_in && token.expires_in <= 0))
        goto done;
    /* Retain a refresh token omitted from a successful refresh response. */
    if (refresh && token.refresh_token == NULL)
        token.refresh_token = borrowed = client->flow.refresh_token;
    if (lonejson_oauth2_token_flow_update_response(&next, &token, now, NULL) ==
        LONEJSON_STATUS_OK)
        status = KNURL_OK;
    if (borrowed != NULL)
        token.refresh_token = NULL;
    if (status == KNURL_OK) {
        wipe(client->flow.access_token);
        wipe(client->flow.refresh_token);
        wipe(client->flow.id_token);
        lonejson_oauth2_token_flow_cleanup(&client->flow);
        client->flow = next;
        /* Without expires_in, obtain a fresh token on the next use. */
        lead = token.has_expires_in ? token.expires_in / 10 : 0;
        if (lead > 30)
            lead = 30;
        client->renew_at = next.has_expires_at ? next.expires_at - lead : now;
    }
done:
    wipe(token.access_token);
    wipe(token.refresh_token);
    wipe(token.id_token);
    lonejson_oauth2_token_response_cleanup(&token);
    knurl_auth_http_clear(&http);
    wipe(authorization);
    free(authorization);
    wipe(form.data);
    lonejson_owned_buffer_free(&form);
    return status;
}

static knurl_status renew(knurl_oauth2_client *client, lonejson_int64 now,
                          knurl_error *error)
{
    lonejson_oidc_discovery discovery;
    lonejson *runtime;
    knurl_status status =
        knurl_auth_http_runtime(client->config.ca_file, &runtime, error);
    if (status != KNURL_OK)
        return status;
    if (client->config.token_endpoint == NULL) {
        lonejson_oidc_discovery_init(&discovery);
        if (lonejson_oidc_fetch_discovery(runtime, client->config.issuer,
                                          TOKEN_LIMIT, &discovery,
                                          NULL) == LONEJSON_STATUS_OK &&
            knurl_auth_https_url(discovery.token_endpoint))
            client->config.token_endpoint =
                knurl_copy_text(discovery.token_endpoint);
        lonejson_oidc_discovery_cleanup(&discovery);
    }
    status = KNURL_BACKEND;
    if (client->config.token_endpoint != NULL) {
        int refresh = client->flow.refresh_token != NULL;
        status = request_token(client, runtime, refresh, now);
        if (status == KNURL_EOF)
            status = request_token(client, runtime, 0, now);
    }
    lonejson_free(runtime);
    return status;
}

knurl_status knurl_oauth2_client_token(knurl_oauth2_client *client, char *out,
                                       size_t capacity, knurl_error *error)
{
    lonejson_int64 now;
    knurl_status status = KNURL_BACKEND;
    if (client == NULL || out == NULL || capacity == 0)
        return knurl_fail(error, KNURL_INVALID, "oauth2",
                          "client and token output are required");
    out[0] = 0;
    pthread_mutex_lock(&client->lock);
    now = (lonejson_int64)time(NULL);
    if (now >= 0 && client->flow.access_token != NULL && now < client->renew_at)
        status = KNURL_OK;
    else if (now >= 0 && now >= client->retry_at) {
        status = renew(client, now, error);
        if (status != KNURL_OK)
            client->retry_at = (lonejson_int64)time(NULL) + 2;
    }
    now = (lonejson_int64)time(NULL);
    if (status != KNURL_OK && now >= 0 && client->flow.has_expires_at &&
        now < client->flow.expires_at)
        status = KNURL_OK;
    if (status == KNURL_OK && client->flow.has_expires_at &&
        now >= client->flow.expires_at)
        status = KNURL_BACKEND;
    if (status == KNURL_OK) {
        size_t length = strlen(client->flow.access_token);
        if (length >= capacity)
            status = KNURL_LIMIT;
        else
            memcpy(out, client->flow.access_token, length + 1);
    }
    pthread_mutex_unlock(&client->lock);
    if (status == KNURL_OK) {
        knurl_error_clear(error);
        return status;
    }
    return knurl_fail(error, status, "oauth2",
                      status == KNURL_LIMIT ? "token output is too small"
                                            : "token endpoint unavailable");
}
