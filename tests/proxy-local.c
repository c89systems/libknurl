#include "proxy_common.h"
#include "proxy_local.h"

#include "proxy-check.h"
#include <string.h>

static rotmos_status custom_error(const rotmos_error *cause, int default_status,
                                  rotmos_proxy_local_response *response,
                                  void *userdata, rotmos_error *error)
{
    const char body[] = {'c', 'u', 's', 't', 'o', 'm', '\0', 'x'};

    (void)userdata;
    PROXY_CHECK(cause != NULL && cause->status == ROTMOS_STATE);
    PROXY_CHECK(default_status == 502);
    PROXY_CHECK(rotmos_proxy_local_respond(response, 503, body, sizeof(body),
                                           error) == ROTMOS_OK);
    return rotmos_proxy_local_add_header(response, "X-Gateway", "custom",
                                         error);
}

static rotmos_status invalid_error(const rotmos_error *cause,
                                   int default_status,
                                   rotmos_proxy_local_response *response,
                                   void *userdata, rotmos_error *error)
{
    (void)cause;
    (void)default_status;
    (void)userdata;
    PROXY_CHECK(rotmos_proxy_local_respond(response, 200, "ok", 2u, error) ==
                ROTMOS_OK);
    PROXY_CHECK(rotmos_proxy_local_add_header(response, "Content-Length", "2",
                                              error) == ROTMOS_INVALID);
    return ROTMOS_OK;
}

int main(void)
{
    rotmos_proxy_local_response response;
    rotmos_proxy_route_data route;
    rotmos_error cause;
    rotmos_error error;
    char body[4];
    char *oversized;

    memset(&route, 0, sizeof(route));
    oversized = malloc(ROTMOS_PROXY_LOCAL_BODY_LIMIT + 1u);
    PROXY_CHECK(oversized != NULL);
    memset(oversized, 'x', ROTMOS_PROXY_LOCAL_BODY_LIMIT + 1u);
    memcpy(body, "abc", sizeof(body));
    rotmos_proxy_local_init(&response);
    PROXY_CHECK(rotmos_proxy_local_respond(&response, 200, body, 3u, &error) ==
                ROTMOS_OK);
    body[0] = 'z';
    PROXY_CHECK(response.body_length == 3u &&
                memcmp(response.body, "abc", 3u) == 0);
    PROXY_CHECK(rotmos_proxy_local_add_header(&response, "Set-Cookie", "a=1",
                                              &error) == ROTMOS_OK);
    PROXY_CHECK(rotmos_proxy_local_add_header(&response, "Set-Cookie", "b=2",
                                              &error) == ROTMOS_OK);
    PROXY_CHECK(response.headers.count == 2u);
    PROXY_CHECK(rotmos_proxy_local_respond(&response, 204, body, 1u, &error) ==
                ROTMOS_INVALID);
    PROXY_CHECK(response.failure == ROTMOS_INVALID);
    rotmos_proxy_local_cleanup(&response);

    rotmos_proxy_local_init(&response);
    PROXY_CHECK(rotmos_proxy_local_respond(&response, 200, oversized,
                                           ROTMOS_PROXY_LOCAL_BODY_LIMIT,
                                           &error) == ROTMOS_OK);
    PROXY_CHECK(response.body_length == ROTMOS_PROXY_LOCAL_BODY_LIMIT);
    PROXY_CHECK(rotmos_proxy_local_respond(&response, 200, oversized,
                                           ROTMOS_PROXY_LOCAL_BODY_LIMIT + 1u,
                                           &error) == ROTMOS_INVALID);
    PROXY_CHECK(rotmos_proxy_local_add_header(&response, "X-Bad", "x\r\ny",
                                              &error) == ROTMOS_INVALID);
    rotmos_proxy_local_cleanup(&response);

    rotmos_proxy_error(&cause, ROTMOS_STATE, "upstream refused connection");
    route.on_error = custom_error;
    rotmos_proxy_local_error(&route, &cause, 502, &response);
    PROXY_CHECK(response.status == 503 && response.body_length == 8u);
    PROXY_CHECK(memcmp(response.body, "custom\0x", 8u) == 0);
    PROXY_CHECK(response.headers.count == 1u);
    rotmos_proxy_local_cleanup(&response);

    route.on_error = invalid_error;
    rotmos_proxy_local_error(&route, &cause, 502, &response);
    PROXY_CHECK(response.status == 502 && response.body_length == 11u);
    PROXY_CHECK(memcmp(response.body, "bad gateway", 11u) == 0);
    PROXY_CHECK(response.headers.count == 0u);
    rotmos_proxy_local_cleanup(&response);

    route.on_error = NULL;
    rotmos_proxy_local_error(&route, &cause, 504, &response);
    PROXY_CHECK(response.status == 504 && response.body_length == 15u);
    PROXY_CHECK(memcmp(response.body, "gateway timeout", 15u) == 0);
    rotmos_proxy_local_cleanup(&response);
    free(oversized);
    return 0;
}
