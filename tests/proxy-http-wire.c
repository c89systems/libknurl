#include "proxy_http_wire.h"
#include "proxy_response.h"

#include "proxy-check.h"
#include <stdlib.h>
#include <string.h>

static void line(knurl_proxy_http_response *response, const char *value)
{
    knurl_proxy_http_event event;

    PROXY_CHECK(knurl_proxy_http_response_header(response, value, strlen(value),
                                                 &event, NULL) ==
                KNURL_PROXY_HEADER_OK);
}

static knurl_status rewrite_no_content(knurl_proxy_response *response,
                                       void *userdata, knurl_error *error)
{
    return knurl_proxy_response_set_status(response, *(const int *)userdata,
                                           error);
}

static void test_head_rewritten_to_no_content(int final_status)
{
    knurl_proxy_http_response response;
    knurl_proxy_headers headers;
    knurl_proxy_http_wire_plan plan;
    knurl_error error;
    int status;

    knurl_proxy_http_response_init(&response, 1);
    line(&response, "HTTP/1.1 200 OK\r\n");
    line(&response, "Content-Length: 123\r\n");
    line(&response, "\r\n");
    knurl_proxy_headers_init(&headers);
    PROXY_CHECK(knurl_proxy_headers_sanitize_response(
                    &response.headers, &headers) == KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_response_apply(&response, &headers,
                                           rewrite_no_content, &final_status,
                                           &status, &error) == KNURL_OK);
    PROXY_CHECK(status == final_status);
    response.status = status;
    PROXY_CHECK(knurl_proxy_http_wire_plan_build(&response, &headers, 1, &plan,
                                                 &error) == KNURL_OK);
    PROXY_CHECK(!plan.chunked && !plan.body_allowed);
    PROXY_CHECK(strstr(plan.head, final_status == 204
                                      ? "HTTP/1.1 204 "
                                      : "HTTP/1.1 205 ") == plan.head);
    PROXY_CHECK(strstr(plan.head, "Content-Length:") == NULL);
    PROXY_CHECK(strstr(plan.head, "Transfer-Encoding:") == NULL);
    knurl_proxy_http_wire_plan_cleanup(&plan);
    knurl_proxy_headers_cleanup(&headers);
    knurl_proxy_http_response_cleanup(&response);
}

static void test_http2_chunked_with_trailers(void)
{
    knurl_proxy_http_response response;
    knurl_proxy_headers headers;
    knurl_proxy_http_wire_plan plan;
    knurl_error error;
    char chunk[32];
    char *final;
    size_t written;
    size_t final_length;

    knurl_proxy_http_response_init(&response, 0);
    line(&response, "HTTP/2 200\r\n");
    line(&response, "Content-Length: 3\r\n");
    line(&response, "Connection: X-Private\r\n");
    line(&response, "X-Private: hidden\r\n");
    line(&response, "Set-Cookie: a=1\r\n");
    line(&response, "Set-Cookie: b=2\r\n");
    line(&response, "Trailer: X-Final\r\n");
    line(&response, "\r\n");
    knurl_proxy_headers_init(&headers);
    PROXY_CHECK(knurl_proxy_headers_sanitize_response(
                    &response.headers, &headers) == KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_http_wire_plan_build(&response, &headers, 1, &plan,
                                                 &error) == KNURL_OK);
    PROXY_CHECK(plan.chunked && plan.body_allowed);
    PROXY_CHECK(strstr(plan.head, "Content-Length:") == NULL);
    PROXY_CHECK(strstr(plan.head, "X-Private") == NULL);
    PROXY_CHECK(strstr(plan.head, "Set-Cookie: a=1\r\n") != NULL);
    PROXY_CHECK(strstr(plan.head, "Set-Cookie: b=2\r\n") != NULL);
    PROXY_CHECK(strstr(plan.head, "Transfer-Encoding: chunked\r\n") != NULL);
    PROXY_CHECK(strstr(plan.head, "Trailer: X-Final\r\n") != NULL);
    PROXY_CHECK(knurl_proxy_http_wire_chunk(&plan, (const unsigned char *)"abc",
                                            3u, chunk, sizeof(chunk), &written,
                                            &error) == KNURL_OK);
    PROXY_CHECK(written == 8u && memcmp(chunk, "3\r\nabc\r\n", written) == 0);
    PROXY_CHECK(knurl_proxy_http_response_body(&response, 3u, NULL) ==
                KNURL_PROXY_HEADER_OK);
    line(&response, "X-Final: yes\r\n");
    line(&response, "\r\n");
    PROXY_CHECK(knurl_proxy_http_response_finish(&response, NULL) ==
                KNURL_PROXY_HEADER_OK);
    final = NULL;
    final_length = 0u;
    PROXY_CHECK(knurl_proxy_http_wire_finish(&plan, &response.trailers, &final,
                                             &final_length,
                                             &error) == KNURL_OK);
    PROXY_CHECK(final_length == strlen("0\r\nX-Final: yes\r\n\r\n"));
    PROXY_CHECK(memcmp(final, "0\r\nX-Final: yes\r\n\r\n", final_length) == 0);
    free(final);
    knurl_proxy_http_wire_plan_cleanup(&plan);
    knurl_proxy_headers_cleanup(&headers);
    knurl_proxy_http_response_cleanup(&response);
}

static void test_fixed_and_bodyless(void)
{
    knurl_proxy_http_response response;
    knurl_proxy_headers headers;
    knurl_proxy_http_wire_plan plan;
    knurl_error error;
    char chunk[8];
    char *final;
    size_t written;
    size_t final_length;

    knurl_proxy_http_response_init(&response, 0);
    line(&response, "HTTP/1.1 200 OK\r\n");
    line(&response, "Content-Length: 3\r\n");
    line(&response, "\r\n");
    knurl_proxy_headers_init(&headers);
    PROXY_CHECK(knurl_proxy_headers_sanitize_response(
                    &response.headers, &headers) == KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_http_wire_plan_build(&response, &headers, 0, &plan,
                                                 &error) == KNURL_OK);
    PROXY_CHECK(!plan.chunked && plan.body_allowed);
    PROXY_CHECK(strstr(plan.head, "Content-Length: 3\r\n") != NULL);
    PROXY_CHECK(knurl_proxy_http_wire_chunk(&plan, (const unsigned char *)"abc",
                                            3u, chunk, sizeof(chunk), &written,
                                            &error) == KNURL_OK);
    PROXY_CHECK(written == 3u && memcmp(chunk, "abc", written) == 0);
    PROXY_CHECK(knurl_proxy_http_wire_finish(&plan, &response.trailers, &final,
                                             &final_length,
                                             &error) == KNURL_OK);
    PROXY_CHECK(final == NULL && final_length == 0u);
    knurl_proxy_http_wire_plan_cleanup(&plan);
    knurl_proxy_headers_cleanup(&headers);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 1);
    line(&response, "HTTP/1.1 304 Not Modified\r\n");
    line(&response, "Content-Length: 123\r\n");
    line(&response, "\r\n");
    knurl_proxy_headers_init(&headers);
    PROXY_CHECK(knurl_proxy_headers_sanitize_response(
                    &response.headers, &headers) == KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_http_wire_plan_build(&response, &headers, 1, &plan,
                                                 &error) == KNURL_OK);
    PROXY_CHECK(!plan.chunked && !plan.body_allowed);
    PROXY_CHECK(strstr(plan.head, "Content-Length: 123\r\n") != NULL);
    PROXY_CHECK(strstr(plan.head, "Transfer-Encoding:") == NULL);
    knurl_proxy_http_wire_plan_cleanup(&plan);
    knurl_proxy_headers_cleanup(&headers);
    knurl_proxy_http_response_cleanup(&response);

    knurl_proxy_http_response_init(&response, 0);
    line(&response, "HTTP/1.1 205 Reset Content\r\n");
    line(&response, "Content-Length: 0\r\n");
    line(&response, "\r\n");
    knurl_proxy_headers_init(&headers);
    PROXY_CHECK(knurl_proxy_headers_sanitize_response(
                    &response.headers, &headers) == KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_http_wire_plan_build(&response, &headers, 1, &plan,
                                                 &error) == KNURL_OK);
    PROXY_CHECK(!plan.chunked && !plan.body_allowed);
    PROXY_CHECK(strstr(plan.head, "Content-Length:") == NULL);
    PROXY_CHECK(strstr(plan.head, "Transfer-Encoding:") == NULL);
    knurl_proxy_http_wire_plan_cleanup(&plan);
    knurl_proxy_headers_cleanup(&headers);
    knurl_proxy_http_response_cleanup(&response);
}

static void test_in_place_chunk(void)
{
    knurl_proxy_http_wire_plan plan;
    knurl_error error;
    unsigned char storage[64];
    unsigned char *body;
    const unsigned char *wire;
    size_t written;

    memset(&plan, 0, sizeof(plan));
    plan.body_allowed = 1;
    body = storage + 32u;
    memcpy(body, "abc", 3u);
    plan.chunked = 1;
    PROXY_CHECK(knurl_proxy_http_wire_chunk_in_place(
                    &plan, body, 3u, 32u, sizeof(storage) - 35u, &wire,
                    &written, &error) == KNURL_OK);
    PROXY_CHECK(written == 8u && memcmp(wire, "3\r\nabc\r\n", written) == 0);
    PROXY_CHECK(memcmp(body, "abc", 3u) == 0);

    PROXY_CHECK(knurl_proxy_http_wire_chunk_in_place(
                    &plan, body, 3u, 2u, sizeof(storage) - 35u, &wire, &written,
                    &error) == KNURL_INVALID);
    PROXY_CHECK(wire == NULL && written == 0u);
    PROXY_CHECK(knurl_proxy_http_wire_chunk_in_place(&plan, body, 3u, 32u, 1u,
                                                     &wire, &written,
                                                     &error) == KNURL_INVALID);
    PROXY_CHECK(wire == NULL && written == 0u);

    plan.chunked = 0;
    PROXY_CHECK(knurl_proxy_http_wire_chunk_in_place(&plan, body, 3u, 0u, 0u,
                                                     &wire, &written,
                                                     &error) == KNURL_OK);
    PROXY_CHECK(wire == body && written == 3u);
}

static void test_wide_length(void)
{
    knurl_proxy_http_response response;
    knurl_proxy_headers headers;
    knurl_proxy_http_wire_plan plan;
    knurl_proxy_http_response_init(&response, 0);
    line(&response, "HTTP/1.1 200 OK\r\n");
    line(&response, "Content-Length: 18446744073709551615\r\n");
    line(&response, "\r\n");
    knurl_proxy_headers_init(&headers);
    PROXY_CHECK(knurl_proxy_headers_sanitize_response(
                    &response.headers, &headers) == KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_http_wire_plan_build(&response, &headers, 0, &plan,
                                                 NULL) == KNURL_OK);
    PROXY_CHECK(strstr(plan.head, "Content-Length: 18446744073709551615\r\n") !=
                NULL);
    knurl_proxy_http_wire_plan_cleanup(&plan);
    knurl_proxy_headers_cleanup(&headers);
    knurl_proxy_http_response_cleanup(&response);
}

int main(void)
{
    test_wide_length();
    test_head_rewritten_to_no_content(204);
    test_head_rewritten_to_no_content(205);
    test_http2_chunked_with_trailers();
    test_fixed_and_bodyless();
    test_in_place_chunk();
    return 0;
}
