#include "proxy_response.h"

#include "proxy-check.h"
#include <string.h>

static knurl_status modify(knurl_proxy_response *response, void *userdata,
                           knurl_error *error)
{
    const char *name;
    const char *value;

    (void)userdata;
    PROXY_CHECK(knurl_proxy_response_status(response) == 200);
    PROXY_CHECK(knurl_proxy_response_header_count(response) == 2u);
    PROXY_CHECK(knurl_proxy_response_header_at(response, 1u, &name, &value) ==
                KNURL_OK);
    PROXY_CHECK(strcmp(name, "X-Source") == 0);
    PROXY_CHECK(strcmp(value, "original") == 0);
    PROXY_CHECK(knurl_proxy_response_set_status(response, 202, error) ==
                KNURL_OK);
    PROXY_CHECK(knurl_proxy_response_set_header(response, "X-Source", "changed",
                                                error) == KNURL_OK);
    PROXY_CHECK(knurl_proxy_response_remove_header(response, "X-Remove",
                                                   error) == KNURL_OK);
    return knurl_proxy_response_add_header(response, "X-New", "yes", error);
}

static knurl_status invalid(knurl_proxy_response *response, void *userdata,
                            knurl_error *error)
{
    int kind;

    kind = *(int *)userdata;
    if (kind == 1)
        PROXY_CHECK(knurl_proxy_response_set_status(response, 204, error) ==
                    KNURL_INVALID);
    if (kind == 2)
        PROXY_CHECK(knurl_proxy_response_set_status(response, 101, error) ==
                    KNURL_INVALID);
    if (kind == 3)
        PROXY_CHECK(knurl_proxy_response_add_header(response, "Content-Length",
                                                    "2",
                                                    error) == KNURL_INVALID);
    if (kind == 4)
        PROXY_CHECK(
            knurl_proxy_response_set_header(response, "Transfer-Encoding",
                                            "chunked", error) == KNURL_INVALID);
    if (kind == 5)
        PROXY_CHECK(
            knurl_proxy_response_add_header(response, "Sec-WebSocket-Accept",
                                            "bad", error) == KNURL_INVALID);
    if (kind == 6)
        PROXY_CHECK(knurl_proxy_response_add_header(response, "X-Invalid",
                                                    "a\r\nb",
                                                    error) == KNURL_INVALID);
    if (kind == 7)
        PROXY_CHECK(knurl_proxy_response_add_header(response, "Forwarded",
                                                    "for=bad",
                                                    error) == KNURL_INVALID);
    if (kind == 8)
        PROXY_CHECK(knurl_proxy_response_set_status(response, 600, error) ==
                    KNURL_INVALID);
    if (kind == 9)
        PROXY_CHECK(knurl_proxy_response_set_status(response, 205, error) ==
                    KNURL_INVALID);
    return KNURL_OK;
}

static knurl_status change_bodyless(knurl_proxy_response *response,
                                    void *userdata, knurl_error *error)
{
    (void)userdata;
    PROXY_CHECK(knurl_proxy_response_status(response) == 204);
    return knurl_proxy_response_set_status(response, 304, error);
}

static void check_modify(void)
{
    knurl_proxy_http_response upstream;
    knurl_proxy_headers sanitized;
    knurl_error error;
    int status;

    memset(&upstream, 0, sizeof(upstream));
    upstream.status = 200;
    upstream.body_allowed = 1;
    knurl_proxy_headers_init(&sanitized);
    PROXY_CHECK(knurl_proxy_headers_add(&sanitized, "X-Remove", "gone") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_headers_add(&sanitized, "X-Source", "original") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_response_apply(&upstream, &sanitized, modify, NULL,
                                           &status, &error) == KNURL_OK);
    PROXY_CHECK(status == 202);
    PROXY_CHECK(upstream.status == 200);
    PROXY_CHECK(sanitized.count == 2u);
    PROXY_CHECK(strcmp(sanitized.fields[0].name, "X-Source") == 0);
    PROXY_CHECK(strcmp(sanitized.fields[0].value, "changed") == 0);
    PROXY_CHECK(strcmp(sanitized.fields[1].name, "X-New") == 0);
    knurl_proxy_headers_cleanup(&sanitized);
}

static void check_invalid(void)
{
    knurl_proxy_http_response upstream;
    knurl_proxy_headers sanitized;
    knurl_error error;
    int status;
    int kind;

    memset(&upstream, 0, sizeof(upstream));
    upstream.status = 200;
    upstream.body_allowed = 1;
    knurl_proxy_headers_init(&sanitized);
    for (kind = 1; kind <= 9; ++kind) {
        status = 0;
        PROXY_CHECK(knurl_proxy_response_apply(&upstream, &sanitized, invalid,
                                               &kind, &status,
                                               &error) == KNURL_INVALID);
        PROXY_CHECK(status == 0);
        knurl_proxy_headers_cleanup(&sanitized);
    }
    upstream.status = 204;
    upstream.body_allowed = 0;
    status = 0;
    PROXY_CHECK(knurl_proxy_response_apply(&upstream, &sanitized, NULL, NULL,
                                           &status, &error) == KNURL_OK);
    PROXY_CHECK(status == 204);
    PROXY_CHECK(knurl_proxy_response_apply(&upstream, &sanitized,
                                           change_bodyless, NULL, &status,
                                           &error) == KNURL_OK);
    PROXY_CHECK(status == 304);
    PROXY_CHECK(upstream.status == 204);
    upstream.status = 101;
    PROXY_CHECK(knurl_proxy_response_apply(&upstream, &sanitized, NULL, NULL,
                                           &status, &error) == KNURL_INVALID);
    knurl_proxy_headers_cleanup(&sanitized);
}

static knurl_status modify_borrowed(knurl_proxy_response *response,
                                    void *userdata, knurl_error *error)
{
    const char *name;
    const char *value;
    int kind;

    kind = *(int *)userdata;
    PROXY_CHECK(knurl_proxy_response_header_at(response, 0u, &name, &value) ==
                KNURL_OK);
    if (kind == 1)
        return knurl_proxy_response_set_header(response, name, value, error);
    if (kind == 2)
        return knurl_proxy_response_set_header(response, "X-Source", value,
                                               error);
    return knurl_proxy_response_remove_header(response, name, error);
}

static void check_borrowed_edits(void)
{
    knurl_proxy_http_response upstream;
    knurl_proxy_headers sanitized;
    knurl_error error;
    int status;
    int kind;

    memset(&upstream, 0, sizeof(upstream));
    upstream.status = 200;
    upstream.body_allowed = 1;
    for (kind = 1; kind <= 3; ++kind) {
        knurl_proxy_headers_init(&sanitized);
        PROXY_CHECK(
            knurl_proxy_headers_add(&sanitized, "X-Source", "original") ==
            KNURL_PROXY_HEADER_OK);
        PROXY_CHECK(knurl_proxy_headers_add(&sanitized, "X-Keep", "yes") ==
                    KNURL_PROXY_HEADER_OK);
        PROXY_CHECK(knurl_proxy_headers_add(&sanitized, "x-source", "repeat") ==
                    KNURL_PROXY_HEADER_OK);
        PROXY_CHECK(knurl_proxy_response_apply(&upstream, &sanitized,
                                               modify_borrowed, &kind, &status,
                                               &error) == KNURL_OK);
        PROXY_CHECK(sanitized.count == (kind == 3 ? 1u : 2u));
        PROXY_CHECK(strcmp(sanitized.fields[0].name, "X-Keep") == 0);
        PROXY_CHECK(strcmp(sanitized.fields[0].value, "yes") == 0);
        if (kind != 3) {
            PROXY_CHECK(strcmp(sanitized.fields[1].name, "X-Source") == 0);
            PROXY_CHECK(strcmp(sanitized.fields[1].value, "original") == 0);
        }
        knurl_proxy_headers_cleanup(&sanitized);
    }
}

int main(void)
{
    check_modify();
    check_invalid();
    check_borrowed_edits();
    return 0;
}
