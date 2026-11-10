#include "proxy_http_upstream.h"

#include "proxy_common.h"

#include <stdlib.h>
#include <string.h>

/* Keep curl's receive request separate from the route's bounded body queue.
 * Smaller curl buffers fragmented slow-reader transfers in local profiling. */
#define KNURL_PROXY_CURL_RECEIVE_BUFFER_MAX 524288u

static size_t knurl_proxy_http_upstream_header(char *data, size_t size,
                                               size_t count, void *userdata)
{
    knurl_proxy_http_upstream *upstream;
    knurl_proxy_http_event event;
    knurl_proxy_header_status status;
    size_t length;

    upstream = (knurl_proxy_http_upstream *)userdata;
    if (size != 0u && count > ((size_t)-1) / size) {
        upstream->failed = 1;
        return 0u;
    }
    length = size * count;
    status = knurl_proxy_http_response_header(&upstream->response, data, length,
                                              &event, NULL);
    if (status == KNURL_PROXY_HEADER_OK && event == KNURL_PROXY_HTTP_FINAL) {
        status = knurl_proxy_headers_sanitize_response(
            &upstream->response.headers, &upstream->outbound_headers);
    }
    if (status != KNURL_PROXY_HEADER_OK) {
        upstream->failed = 1;
        return 0u;
    }
    if (event != KNURL_PROXY_HTTP_MORE && upstream->ready != NULL) {
        upstream->ready(upstream, event, upstream->userdata);
    }
    if (upstream->failed)
        return 0u;
    if (event == KNURL_PROXY_HTTP_FINAL && upstream->head_upload) {
        /* Curl's upload mode waits for representation bytes after a HEAD reply.
         * End this transfer at its final headers; no response body may follow.
         */
        upstream->head_upload_aborted = 1;
        return 0u;
    }
    return length;
}

static size_t knurl_proxy_http_upstream_download(char *data, size_t size,
                                                 size_t count, void *userdata)
{
    knurl_proxy_http_upstream *upstream;
    size_t length;

    upstream = (knurl_proxy_http_upstream *)userdata;
    if (size != 0u && count > ((size_t)-1) / size) {
        upstream->failed = 1;
        return 0u;
    }
    length = size * count;
    if (upstream->body_length != upstream->body_offset) {
        upstream->paused = 1;
        return CURL_WRITEFUNC_PAUSE;
    }
    if (length > upstream->body_capacity ||
        knurl_proxy_http_response_body(&upstream->response, length, NULL) !=
            KNURL_PROXY_HEADER_OK) {
        upstream->failed = 1;
        return 0u;
    }
    if (length != 0u) {
        if (upstream->body_storage == NULL) {
            upstream->body_storage = (unsigned char *)malloc(
                upstream->body_capacity + KNURL_PROXY_HTTP_BODY_HEADROOM +
                KNURL_PROXY_HTTP_BODY_TAILROOM);
            if (upstream->body_storage == NULL) {
                upstream->failed = 1;
                return 0u;
            }
            upstream->body =
                upstream->body_storage + KNURL_PROXY_HTTP_BODY_HEADROOM;
        }
        memcpy(upstream->body, data, length);
        upstream->body_offset = 0u;
        upstream->body_length = length;
        if (upstream->ready != NULL) {
            upstream->ready(upstream, KNURL_PROXY_HTTP_MORE,
                            upstream->userdata);
        }
    }
    return length;
}

knurl_status knurl_proxy_http_upstream_init(
    knurl_proxy_http_upstream *upstream, CURL *easy, const char *url,
    const char *request_target, const char *method,
    struct curl_slist *request_headers, size_t buffer_limit,
    long connect_timeout_ms, long total_timeout_ms,
    const knurl_proxy_http_upload *upload,
    knurl_proxy_http_upstream_ready_fn ready, void *userdata,
    knurl_error *error)
{
    size_t capacity;
    CURLcode code;
    int head;

    if (upstream == NULL || easy == NULL || url == NULL ||
        request_target == NULL || method == NULL || buffer_limit < 8192u ||
        buffer_limit > 1048576u || connect_timeout_ms < 0L ||
        total_timeout_ms < 0L ||
        (upload != NULL &&
         (upload->read == NULL ||
          (upload->known_length != 0 && upload->known_length != 1) ||
          (upload->known_length &&
           ((curl_off_t)upload->content_length < 0 ||
            (uint64_t)(curl_off_t)upload->content_length !=
                upload->content_length))))) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid proxy upstream transfer configuration");
        return KNURL_INVALID;
    }
    memset(upstream, 0, sizeof(*upstream));
    head = strcmp(method, "HEAD") == 0;
    capacity =
        buffer_limit > CURL_MAX_WRITE_SIZE ? buffer_limit : CURL_MAX_WRITE_SIZE;
    upstream->body_capacity = capacity;
    upstream->easy = easy;
    upstream->ready = ready;
    upstream->userdata = userdata;
    knurl_proxy_http_response_init(&upstream->response, head);
    upstream->head_upload = head && upload != NULL;
    knurl_proxy_headers_init(&upstream->outbound_headers);
    code = curl_easy_setopt(easy, CURLOPT_URL, url);
    if (code == CURLE_OK)
        code = curl_easy_setopt(easy, CURLOPT_REQUEST_TARGET, request_target);
    if (code == CURLE_OK)
        code = curl_easy_setopt(easy, CURLOPT_NOBODY, head ? 1L : 0L);
    if (code == CURLE_OK)
        code = curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method);
    if (code == CURLE_OK && upload != NULL)
        code = curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);
    if (code == CURLE_OK && upload != NULL)
        code = curl_easy_setopt(easy, CURLOPT_READFUNCTION, upload->read);
    if (code == CURLE_OK && upload != NULL)
        code = curl_easy_setopt(easy, CURLOPT_READDATA, upload->userdata);
    if (code == CURLE_OK && upload != NULL && upload->trailers != NULL)
        code =
            curl_easy_setopt(easy, CURLOPT_TRAILERFUNCTION, upload->trailers);
    if (code == CURLE_OK && upload != NULL && upload->trailers != NULL)
        code = curl_easy_setopt(easy, CURLOPT_TRAILERDATA, upload->userdata);
    if (code == CURLE_OK && upload != NULL)
        code = curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE,
                                upload->known_length
                                    ? (curl_off_t)upload->content_length
                                    : (curl_off_t)-1);
    if (code == CURLE_OK && upstream->head_upload)
        code = curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 1L);
    if (code == CURLE_OK)
        code = curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request_headers);
    if (code == CURLE_OK)
        code = curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION,
                                knurl_proxy_http_upstream_header);
    if (code == CURLE_OK)
        code = curl_easy_setopt(easy, CURLOPT_HEADERDATA, upstream);
    if (code == CURLE_OK)
        code = curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
                                knurl_proxy_http_upstream_download);
    if (code == CURLE_OK)
        code = curl_easy_setopt(easy, CURLOPT_WRITEDATA, upstream);
    if (code == CURLE_OK)
        code = curl_easy_setopt(
            easy, CURLOPT_BUFFERSIZE,
            (long)(buffer_limit < KNURL_PROXY_CURL_RECEIVE_BUFFER_MAX
                       ? buffer_limit
                       : KNURL_PROXY_CURL_RECEIVE_BUFFER_MAX));
    if (code == CURLE_OK)
        code = curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                                connect_timeout_ms);
    if (code == CURLE_OK)
        code = curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, total_timeout_ms);
    if (code == CURLE_OK)
        code = curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    if (code != CURLE_OK) {
        knurl_proxy_http_upstream_cleanup(upstream);
        knurl_proxy_error(error, KNURL_STATE,
                          "failed to configure proxy upstream transfer");
        return KNURL_STATE;
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

unsigned char *
knurl_proxy_http_upstream_body(knurl_proxy_http_upstream *upstream,
                               size_t *length)
{
    if (length != NULL)
        *length = upstream != NULL
                      ? upstream->body_length - upstream->body_offset
                      : 0u;
    if (upstream == NULL || upstream->body_length == upstream->body_offset)
        return NULL;
    return upstream->body + upstream->body_offset;
}

void knurl_proxy_http_upstream_consume(knurl_proxy_http_upstream *upstream,
                                       size_t length)
{
    if (upstream == NULL ||
        length > upstream->body_length - upstream->body_offset)
        return;
    upstream->body_offset += length;
    if (upstream->body_offset == upstream->body_length) {
        upstream->body_offset = 0u;
        upstream->body_length = 0u;
    }
}

knurl_status
knurl_proxy_http_upstream_resume(knurl_proxy_http_upstream *upstream,
                                 knurl_error *error)
{
    if (upstream == NULL || upstream->easy == NULL ||
        upstream->body_length != upstream->body_offset) {
        knurl_proxy_error(error, KNURL_STATE,
                          "proxy downstream must drain before curl resumes");
        return KNURL_STATE;
    }
    if (upstream->paused) {
        upstream->paused = 0;
        if (curl_easy_pause(upstream->easy, CURLPAUSE_CONT) != CURLE_OK) {
            upstream->failed = 1;
            knurl_proxy_error(error, KNURL_STATE,
                              "failed to resume proxy upstream transfer");
            return KNURL_STATE;
        }
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_proxy_header_status
knurl_proxy_http_upstream_finish(knurl_proxy_http_upstream *upstream,
                                 CURLcode result, const char **reason)
{
    knurl_proxy_http_event event;
    knurl_proxy_header_status status;

    if (upstream == NULL || upstream->failed ||
        (result != CURLE_OK &&
         !(result == CURLE_WRITE_ERROR && upstream->head_upload_aborted))) {
        if (reason != NULL)
            *reason = "upstream transfer or framing failed";
        return KNURL_PROXY_HEADER_INVALID;
    }
    status = knurl_proxy_http_response_curl_complete(&upstream->response,
                                                     &event, reason);
    if (status == KNURL_PROXY_HEADER_OK && event == KNURL_PROXY_HTTP_TRAILERS &&
        upstream->ready != NULL)
        upstream->ready(upstream, event, upstream->userdata);
    return status;
}

void knurl_proxy_http_upstream_cleanup(knurl_proxy_http_upstream *upstream)
{
    if (upstream == NULL)
        return;
    knurl_proxy_http_response_cleanup(&upstream->response);
    knurl_proxy_headers_cleanup(&upstream->outbound_headers);
    free(upstream->body_storage);
    memset(upstream, 0, sizeof(*upstream));
}
