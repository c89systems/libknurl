#include "auth_internal.h"
#include "json_internal.h"
#include <curl/curl.h>
#include <knurl/tls.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int knurl_auth_https_url(const char *text)
{
    CURLU *url;
    char *fragment = NULL;
    int valid;
    if (text == NULL || strncmp(text, "https://", 8) != 0 ||
        strlen(text) > 4096 || strchr(text, '#') != NULL)
        return 0;
    url = curl_url();
    if (url == NULL)
        return 0;
    valid = curl_url_set(url, CURLUPART_URL, text, CURLU_DISALLOW_USER) ==
                CURLUE_OK &&
            curl_url_get(url, CURLUPART_FRAGMENT, &fragment, 0) ==
                CURLUE_NO_FRAGMENT;
    curl_free(fragment);
    curl_url_cleanup(url);
    return valid;
}

struct http_body {
    lonejson_owned_buffer *buffer;
    size_t limit;
};

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_ready;

static void curl_start(void)
{
    curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT);
}

static size_t receive(char *data, size_t size, size_t count, void *userdata)
{
    struct http_body *body = userdata;
    size_t length = size * count;
    if (length > body->limit - body->buffer->len ||
        lonejson_owned_buffer_sink(body->buffer, data, length, NULL) !=
            LONEJSON_STATUS_OK)
        return 0;
    return length;
}

static int header(struct curl_slist **list, const char *name, const char *value)
{
    struct curl_slist *next;
    size_t capacity;
    char *line;
    if (value == NULL)
        return 1;
    if (strpbrk(value, "\r\n") != NULL)
        return 0;
    capacity = strlen(name) + strlen(value) + 3;
    line = malloc(capacity);
    if (line == NULL)
        return 0;
    snprintf(line, capacity, "%s: %s", name, value);
    next = curl_slist_append(*list, line);
    OPENSSL_cleanse(line, capacity);
    free(line);
    if (next == NULL)
        return 0;
    *list = next;
    return 1;
}

void knurl_auth_http_clear(lonejson_http_response *response)
{
    if (response->body.data != NULL)
        OPENSSL_cleanse(response->body.data, response->body.len);
    lonejson_http_response_cleanup(response);
}

lonejson_status knurl_auth_http(void *userdata,
                                const lonejson_http_request *request,
                                lonejson_http_response *response,
                                lonejson_error *error)
{
    const char *ca = userdata;
    CURL *curl;
    CURLcode result = CURLE_FAILED_INIT;
    struct curl_slist *headers = NULL, *item;
    struct http_body body;
    (void)error;
    if (!knurl_auth_https_url(request->url))
        return LONEJSON_STATUS_INVALID_ARGUMENT;
    pthread_once(&curl_once, curl_start);
    if (curl_ready != CURLE_OK)
        return LONEJSON_STATUS_IO_ERROR;
    curl = curl_easy_init();
    if (curl == NULL)
        return LONEJSON_STATUS_IO_ERROR;
    if (!header(&headers, "Content-Type", request->content_type) ||
        !header(&headers, "Authorization", request->authorization))
        goto done;
    body.buffer = &response->body;
    body.limit =
        request->max_response_bytes != 0 ? request->max_response_bytes : 65536;
    if (ca == NULL)
        ca = knurl_tls_system_ca_file();
#define SET(option, value)                                                     \
    do {                                                                       \
        result = curl_easy_setopt(curl, option, value);                        \
        if (result != CURLE_OK)                                                \
            goto done;                                                         \
    } while (0)
    SET(CURLOPT_URL, request->url);
    SET(CURLOPT_PROTOCOLS_STR, "https");
    SET(CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_TIMEOUT_MS, 3000L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    if (ca != NULL)
        SET(CURLOPT_CAINFO, ca);
    SET(CURLOPT_HTTPHEADER, headers);
    SET(CURLOPT_USERAGENT,
        request->user_agent != NULL ? request->user_agent : "knurl");
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, &body);
    if (strcmp(request->method, "POST") == 0) {
        SET(CURLOPT_POSTFIELDS, request->body);
        SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request->body_len);
    }
#undef SET
    result = curl_easy_perform(curl);
    if (result == CURLE_OK)
        result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE,
                                   &response->status_code);
done:
    curl_easy_cleanup(curl);
    for (item = headers; item != NULL; item = item->next)
        OPENSSL_cleanse(item->data, strlen(item->data));
    curl_slist_free_all(headers);
    if (result != CURLE_OK)
        knurl_auth_http_clear(response);
    return result == CURLE_OK ? LONEJSON_STATUS_OK : LONEJSON_STATUS_IO_ERROR;
}

knurl_status knurl_auth_http_runtime(const char *ca, lonejson **runtime,
                                     knurl_error *error)
{
    lonejson_http_provider provider;
    knurl_status status;
    int owned;
    status = knurl_json_runtime(NULL, runtime, &owned, error);
    if (status != KNURL_OK)
        return status;
    if (lonejson_http_provider_init_simple(&provider, (void *)ca, "knurl",
                                           knurl_auth_http,
                                           NULL) != LONEJSON_STATUS_OK ||
        lonejson_set_http_provider(*runtime, &provider, NULL) !=
            LONEJSON_STATUS_OK) {
        lonejson_free(*runtime);
        return knurl_fail(error, KNURL_BACKEND, "auth_http",
                          "cannot initialize HTTPS transport");
    }
    return KNURL_OK;
}
