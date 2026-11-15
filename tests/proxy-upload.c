#include "proxy_upload.h"

#include "proxy-check.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

static void test_fixed_backpressure(void)
{
    knurl_proxy_upload_buffer upload;
    knurl_proxy_headers headers;
    knurl_http_frame_result result;
    knurl_error error;
    unsigned char *body;
    char chunk[8192];
    size_t consumed;
    size_t amount;

    body = (unsigned char *)malloc(8197u);
    PROXY_CHECK(body != NULL);
    memset(body, 'a', 8192u);
    memset(body + 8192u, 'b', 5u);
    knurl_proxy_headers_init(&headers);
    PROXY_CHECK(knurl_proxy_upload_init(&upload, 8192u, 0, 8197u, &headers,
                                        &error) == KNURL_OK);
    PROXY_CHECK(knurl_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) ==
                CURL_READFUNC_PAUSE);
    PROXY_CHECK(!knurl_proxy_upload_can_resume(&upload));
    result = knurl_proxy_upload_feed(&upload, body, 8197u, &consumed);
    PROXY_CHECK(result == KNURL_HTTP_FRAME_PAUSED && consumed == 8192u);
    PROXY_CHECK(knurl_proxy_upload_full(&upload));
    PROXY_CHECK(knurl_proxy_upload_can_resume(&upload));
    knurl_proxy_upload_resumed(&upload);
    amount = knurl_proxy_upload_read(chunk, 1u, 4096u, &upload);
    PROXY_CHECK(amount == 4096u);
    PROXY_CHECK(memcmp(chunk, body, amount) == 0);
    result = knurl_proxy_upload_feed(&upload, body + consumed, 5u, &consumed);
    PROXY_CHECK(result == KNURL_HTTP_FRAME_COMPLETE && consumed == 5u);
    PROXY_CHECK(upload.complete && !knurl_proxy_upload_full(&upload));
    amount = knurl_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload);
    PROXY_CHECK(amount == 4101u);
    PROXY_CHECK(memcmp(chunk, body + 4096u, amount) == 0);
    PROXY_CHECK(knurl_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) ==
                0u);
    knurl_proxy_upload_cleanup(&upload);
    knurl_proxy_headers_cleanup(&headers);
    free(body);
}

static void test_chunked_trailers(void)
{
    static const char wire[] =
        "3\r\nabc\r\n0\r\nX-Trace: done\r\n\r\nGET /next";
    knurl_proxy_upload_buffer upload;
    knurl_proxy_headers headers;
    knurl_http_frame_result result;
    knurl_error error;
    struct curl_slist *sent_trailers;
    char chunk[8];
    size_t consumed;

    knurl_proxy_headers_init(&headers);
    PROXY_CHECK(knurl_proxy_headers_add(&headers, "Trailer", "X-Trace") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_upload_init(&upload, 8192u, 1, 0u, &headers,
                                        &error) == KNURL_OK);
    result = knurl_proxy_upload_feed(&upload, (const unsigned char *)wire,
                                     sizeof(wire) - 1u, &consumed);
    PROXY_CHECK(result == KNURL_HTTP_FRAME_COMPLETE);
    PROXY_CHECK(consumed == sizeof(wire) - sizeof("GET /next"));
    PROXY_CHECK(knurl_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) ==
                3u);
    PROXY_CHECK(memcmp(chunk, "abc", 3u) == 0);
    PROXY_CHECK(knurl_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) ==
                0u);
    PROXY_CHECK(upload.trailers.count == 1u);
    PROXY_CHECK(strcmp(upload.trailers.fields[0].name, "X-Trace") == 0);
    PROXY_CHECK(strcmp(upload.trailers.fields[0].value, "done") == 0);
    sent_trailers = NULL;
    PROXY_CHECK(knurl_proxy_upload_curl_trailers(&sent_trailers, &upload) ==
                CURL_TRAILERFUNC_OK);
    PROXY_CHECK(sent_trailers != NULL &&
                strcmp(sent_trailers->data, "X-Trace: done") == 0);
    curl_slist_free_all(sent_trailers);
    knurl_proxy_upload_cleanup(&upload);

    PROXY_CHECK(knurl_proxy_upload_init(&upload, 8192u, 1, 0u, &headers,
                                        &error) == KNURL_OK);
    result = knurl_proxy_upload_feed(
        &upload, (const unsigned char *)"0\r\nX-Other: bad\r\n\r\n", 19u,
        &consumed);
    PROXY_CHECK(result == KNURL_HTTP_FRAME_INVALID);
    knurl_proxy_upload_cleanup(&upload);
    knurl_proxy_headers_cleanup(&headers);
}

static void test_fixed_direct_receive(void)
{
    knurl_proxy_upload_buffer upload;
    knurl_proxy_headers headers;
    knurl_error error;
    unsigned char *space;
    char chunk[8192];
    size_t capacity;

    knurl_proxy_headers_init(&headers);
    PROXY_CHECK(knurl_proxy_upload_init(&upload, 8192u, 0, 8197u, &headers,
                                        &error) == KNURL_OK);
    space = knurl_proxy_upload_reserve_fixed(&upload, &capacity);
    PROXY_CHECK(space != NULL && capacity == 8192u);
    memset(space, 'a', capacity);
    PROXY_CHECK(knurl_proxy_upload_commit_fixed(&upload, capacity) ==
                KNURL_HTTP_FRAME_MORE);
    PROXY_CHECK(knurl_proxy_upload_full(&upload));
    PROXY_CHECK(knurl_proxy_upload_reserve_fixed(&upload, &capacity) == NULL);
    PROXY_CHECK(capacity == 0u);
    PROXY_CHECK(knurl_proxy_upload_read(chunk, 1u, 4096u, &upload) == 4096u);
    space = knurl_proxy_upload_reserve_fixed(&upload, &capacity);
    PROXY_CHECK(space != NULL && capacity == 5u);
    memset(space, 'b', capacity);
    PROXY_CHECK(knurl_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) ==
                4096u);
    PROXY_CHECK(memcmp(chunk, "aaaa", 4u) == 0);
    PROXY_CHECK(knurl_proxy_upload_reserve_fixed(&upload, &capacity) == space);
    PROXY_CHECK(capacity == 5u);
    PROXY_CHECK(knurl_proxy_upload_commit_fixed(&upload, capacity) ==
                KNURL_HTTP_FRAME_COMPLETE);
    PROXY_CHECK(upload.complete);
    PROXY_CHECK(knurl_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) ==
                5u);
    PROXY_CHECK(memcmp(chunk, "bbbbb", 5u) == 0);
    PROXY_CHECK(knurl_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) ==
                0u);
    knurl_proxy_upload_cleanup(&upload);
    knurl_proxy_headers_cleanup(&headers);
}

static void test_body_limit(void)
{
    static const unsigned char chunked[] = "5\r\nabcde\r\n0\r\n\r\n";
    knurl_proxy_upload_buffer upload;
    knurl_proxy_headers headers;
    knurl_error error;
    size_t consumed;
    size_t capacity;
    unsigned char *space;
    char byte;

    knurl_proxy_headers_init(&headers);
    PROXY_CHECK(knurl_proxy_upload_init(&upload, 8192u, 1, 0u, &headers,
                                        &error) == KNURL_OK);
    upload.body_limit = 3u;
    PROXY_CHECK(knurl_proxy_upload_feed(&upload, chunked, sizeof(chunked) - 1u,
                                        &consumed) == KNURL_HTTP_FRAME_INVALID);
    PROXY_CHECK(upload.failure == KNURL_LIMIT && upload.body_accepted == 0u &&
                !upload.complete && consumed == 3u);
    PROXY_CHECK(knurl_proxy_upload_read(&byte, 1u, 1u, &upload) ==
                CURL_READFUNC_ABORT);
    knurl_proxy_upload_cleanup(&upload);

    PROXY_CHECK(knurl_proxy_upload_init(&upload, 8192u, 0, 5u, &headers,
                                        &error) == KNURL_OK);
    upload.body_limit = 3u;
    space = knurl_proxy_upload_reserve_fixed(&upload, &capacity);
    PROXY_CHECK(space != NULL && capacity == 5u);
    memset(space, 'a', capacity);
    PROXY_CHECK(knurl_proxy_upload_commit_fixed(&upload, capacity) ==
                KNURL_HTTP_FRAME_INVALID);
    PROXY_CHECK(upload.failure == KNURL_LIMIT && upload.end == 0u &&
                upload.body_accepted == 0u && !upload.complete);
    knurl_proxy_upload_cleanup(&upload);

    PROXY_CHECK(knurl_proxy_upload_init(&upload, 8192u, 0, 3u, &headers,
                                        &error) == KNURL_OK);
    upload.body_limit = 3u;
    PROXY_CHECK(knurl_proxy_upload_feed(&upload, (const unsigned char *)"abc",
                                        3u, &consumed) ==
                KNURL_HTTP_FRAME_COMPLETE);
    PROXY_CHECK(upload.body_accepted == 3u && consumed == 3u);
    knurl_proxy_upload_cleanup(&upload);
    knurl_proxy_headers_cleanup(&headers);
}

int main(void)
{
    test_fixed_backpressure();
    test_fixed_direct_receive();
    test_chunked_trailers();
    test_body_limit();
    return 0;
}
