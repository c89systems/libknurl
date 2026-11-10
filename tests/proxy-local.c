#include "proxy_common.h"
#include "proxy_local.h"

#include "proxy-check.h"
#include <string.h>

static knurl_status custom_error(const knurl_error *cause, int default_status,
                                 knurl_proxy_local_response *response,
                                 void *userdata, knurl_error *error)
{
    const char body[] = {'c', 'u', 's', 't', 'o', 'm', '\0', 'x'};

    (void)userdata;
    PROXY_CHECK(cause != NULL && cause->status == KNURL_STATE);
    PROXY_CHECK(default_status == 502);
    PROXY_CHECK(knurl_proxy_local_respond(response, 503, body, sizeof(body),
                                          error) == KNURL_OK);
    return knurl_proxy_local_add_header(response, "X-Gateway", "custom", error);
}

static knurl_status invalid_error(const knurl_error *cause, int default_status,
                                  knurl_proxy_local_response *response,
                                  void *userdata, knurl_error *error)
{
    (void)cause;
    (void)default_status;
    (void)userdata;
    PROXY_CHECK(knurl_proxy_local_respond(response, 200, "ok", 2u, error) ==
                KNURL_OK);
    PROXY_CHECK(knurl_proxy_local_add_header(response, "Content-Length", "2",
                                             error) == KNURL_INVALID);
    return KNURL_OK;
}

int main(void)
{
    knurl_proxy_local_response response;
    knurl_proxy_route_data route;
    knurl_error cause;
    knurl_error error;
    char body[4];
    char *oversized;

    memset(&route, 0, sizeof(route));
    oversized = malloc(KNURL_PROXY_LOCAL_BODY_LIMIT + 1u);
    PROXY_CHECK(oversized != NULL);
    memset(oversized, 'x', KNURL_PROXY_LOCAL_BODY_LIMIT + 1u);
    memcpy(body, "abc", sizeof(body));
    knurl_proxy_local_init(&response);
    PROXY_CHECK(knurl_proxy_local_respond(&response, 200, body, 3u, &error) ==
                KNURL_OK);
    body[0] = 'z';
    PROXY_CHECK(response.body_length == 3u &&
                memcmp(response.body, "abc", 3u) == 0);
    PROXY_CHECK(knurl_proxy_local_add_header(&response, "Set-Cookie", "a=1",
                                             &error) == KNURL_OK);
    PROXY_CHECK(knurl_proxy_local_add_header(&response, "Set-Cookie", "b=2",
                                             &error) == KNURL_OK);
    PROXY_CHECK(response.headers.count == 2u);
    PROXY_CHECK(knurl_proxy_local_respond(&response, 204, body, 1u, &error) ==
                KNURL_INVALID);
    PROXY_CHECK(response.failure == KNURL_INVALID);
    knurl_proxy_local_cleanup(&response);

    knurl_proxy_local_init(&response);
    PROXY_CHECK(knurl_proxy_local_respond(&response, 200, oversized,
                                          KNURL_PROXY_LOCAL_BODY_LIMIT,
                                          &error) == KNURL_OK);
    PROXY_CHECK(response.body_length == KNURL_PROXY_LOCAL_BODY_LIMIT);
    PROXY_CHECK(knurl_proxy_local_respond(&response, 200, oversized,
                                          KNURL_PROXY_LOCAL_BODY_LIMIT + 1u,
                                          &error) == KNURL_INVALID);
    PROXY_CHECK(knurl_proxy_local_add_header(&response, "X-Bad", "x\r\ny",
                                             &error) == KNURL_INVALID);
    knurl_proxy_local_cleanup(&response);

    knurl_proxy_error(&cause, KNURL_STATE, "upstream refused connection");
    route.on_error = custom_error;
    knurl_proxy_local_error(&route, &cause, 502, &response);
    PROXY_CHECK(response.status == 503 && response.body_length == 8u);
    PROXY_CHECK(memcmp(response.body, "custom\0x", 8u) == 0);
    PROXY_CHECK(response.headers.count == 1u);
    knurl_proxy_local_cleanup(&response);

    route.on_error = invalid_error;
    knurl_proxy_local_error(&route, &cause, 502, &response);
    PROXY_CHECK(response.status == 502 && response.body_length == 11u);
    PROXY_CHECK(memcmp(response.body, "bad gateway", 11u) == 0);
    PROXY_CHECK(response.headers.count == 0u);
    knurl_proxy_local_cleanup(&response);

    route.on_error = NULL;
    knurl_proxy_local_error(&route, &cause, 504, &response);
    PROXY_CHECK(response.status == 504 && response.body_length == 15u);
    PROXY_CHECK(memcmp(response.body, "gateway timeout", 15u) == 0);
    knurl_proxy_local_cleanup(&response);
    free(oversized);
    return 0;
}
