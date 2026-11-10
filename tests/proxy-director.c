#include "proxy_director.h"

#include "proxy-check.h"
#include <stdlib.h>
#include <string.h>

static knurl_status change_request(const knurl_proxy_inbound *in,
                                   knurl_proxy_outbound *out, void *userdata,
                                   knurl_error *error)
{
    const char *name;
    const char *value;

    (void)userdata;
    PROXY_CHECK(knurl_proxy_inbound_method(in) == KNURL_HTTP_POST);
    PROXY_CHECK(strcmp(knurl_proxy_inbound_path(in), "/original") == 0);
    PROXY_CHECK(strcmp(knurl_proxy_inbound_query(in), "a=1&a=2") == 0);
    PROXY_CHECK(strcmp(knurl_proxy_inbound_host(in), "client.test") == 0);
    PROXY_CHECK(!knurl_proxy_inbound_websocket(in));
    PROXY_CHECK(knurl_proxy_inbound_header_count(in) == 2u);
    PROXY_CHECK(knurl_proxy_inbound_header_at(in, 1u, &name, &value) ==
                KNURL_OK);
    PROXY_CHECK(strcmp(name, "X-Trace") == 0);
    PROXY_CHECK(strcmp(value, "inbound") == 0);
    PROXY_CHECK(knurl_proxy_outbound_select_target(out, 1u, error) == KNURL_OK);
    PROXY_CHECK(knurl_proxy_outbound_set_method(out, KNURL_HTTP_PUT, error) ==
                KNURL_OK);
    PROXY_CHECK(knurl_proxy_outbound_set_path(out, "/other", error) ==
                KNURL_OK);
    PROXY_CHECK(knurl_proxy_outbound_set_query(out, "new=3&new=4", error) ==
                KNURL_OK);
    PROXY_CHECK(knurl_proxy_outbound_set_host(out, "public.test:8080", error) ==
                KNURL_OK);
    PROXY_CHECK(knurl_proxy_outbound_set_header(out, "X-Trace", "changed",
                                                error) == KNURL_OK);
    return knurl_proxy_outbound_add_header(out, "X-Extra", "ok", error);
}

static knurl_status invalid_edit(const knurl_proxy_inbound *in,
                                 knurl_proxy_outbound *out, void *userdata,
                                 knurl_error *error)
{
    int kind;

    (void)in;
    kind = *(int *)userdata;
    if (kind == 1)
        PROXY_CHECK(knurl_proxy_outbound_select_target(out, 2u, error) ==
                    KNURL_INVALID);
    if (kind == 2)
        PROXY_CHECK(knurl_proxy_outbound_set_host(out, "bad\r\nHost: injected",
                                                  error) == KNURL_INVALID);
    if (kind == 3)
        PROXY_CHECK(knurl_proxy_outbound_add_header(out, "Connection",
                                                    "keep-alive",
                                                    error) == KNURL_INVALID);
    if (kind == 4)
        PROXY_CHECK(knurl_proxy_outbound_set_path(out, "/../secret", error) ==
                    KNURL_INVALID);
    if (kind == 5)
        PROXY_CHECK(knurl_proxy_outbound_set_method(out, KNURL_HTTP_POST,
                                                    error) == KNURL_INVALID);
    if (kind == 6)
        PROXY_CHECK(knurl_proxy_outbound_add_header(out, "Sec-WebSocket-Key",
                                                    "bad",
                                                    error) == KNURL_INVALID);
    if (kind == 7)
        PROXY_CHECK(knurl_proxy_outbound_add_header(out, "Forwarded", "for=bad",
                                                    error) == KNURL_INVALID);
    if (kind == 8)
        PROXY_CHECK(knurl_proxy_outbound_set_query(out, "q=%0a", error) ==
                    KNURL_OK);
    if (kind == 9)
        PROXY_CHECK(knurl_proxy_outbound_add_header(out, "X-Invalid",
                                                    "bad\r\nnext",
                                                    error) == KNURL_INVALID);
    if (kind == 10)
        PROXY_CHECK(knurl_proxy_outbound_set_method(out, KNURL_HTTP_ANY,
                                                    error) == KNURL_INVALID);
    if (kind == 11)
        PROXY_CHECK(knurl_proxy_outbound_set_method(out, (knurl_http_method)999,
                                                    error) == KNURL_INVALID);
    /* Even if the callback ignores the failed setter, admission must fail. */
    return KNURL_OK;
}

static void check_change(void)
{
    char primary[] = "https://primary.test/base";
    char alternate[] = "http://other.test/alt";
    char *targets[2];
    knurl_proxy_route_data route;
    knurl_proxy_headers inbound;
    knurl_proxy_headers sanitized;
    knurl_proxy_outbound out;
    knurl_error error;
    char *target;
    char *authority;

    memset(&route, 0, sizeof(route));
    targets[0] = primary;
    targets[1] = alternate;
    route.targets = targets;
    route.target_count = 2u;
    route.rewrite = change_request;
    knurl_proxy_headers_init(&inbound);
    knurl_proxy_headers_init(&sanitized);
    PROXY_CHECK(knurl_proxy_headers_add(&inbound, "Host", "client.test") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_headers_add(&inbound, "X-Trace", "inbound") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_headers_add(&sanitized, "X-Trace", "inbound") ==
                KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(knurl_proxy_director_prepare(
                    &route, KNURL_HTTP_POST, "/original", "a=1&a=2",
                    "client.test", 0, NULL, &inbound, &sanitized, &out, &target,
                    &authority, &error) == KNURL_OK);
    PROXY_CHECK(strcmp(target, "/alt/other?new=3&new=4") == 0);
    PROXY_CHECK(strcmp(authority, "public.test:8080") == 0);
    PROXY_CHECK(strcmp(route.targets[out.target_index], targets[1]) == 0);
    PROXY_CHECK(out.method == KNURL_HTTP_PUT);
    PROXY_CHECK(out.headers.count == 2u);
    PROXY_CHECK(strcmp(out.headers.fields[0].value, "changed") == 0);
    PROXY_CHECK(strcmp(out.headers.fields[1].name, "X-Extra") == 0);
    PROXY_CHECK(strcmp(sanitized.fields[0].value, "inbound") == 0);
    free(target);
    free(authority);
    knurl_proxy_director_cleanup(&out);
    knurl_proxy_headers_cleanup(&sanitized);
    knurl_proxy_headers_cleanup(&inbound);
}

static void check_invalid(void)
{
    char primary[] = "https://primary.test/base";
    char alternate[] = "http://other.test/alt";
    char *targets[2];
    knurl_proxy_route_data route;
    knurl_proxy_headers inbound;
    knurl_proxy_headers sanitized;
    knurl_proxy_outbound out;
    knurl_error error;
    char *target;
    char *authority;
    int kind;

    memset(&route, 0, sizeof(route));
    targets[0] = primary;
    targets[1] = alternate;
    route.targets = targets;
    route.target_count = 2u;
    route.rewrite = invalid_edit;
    route.rewrite_userdata = &kind;
    knurl_proxy_headers_init(&inbound);
    knurl_proxy_headers_init(&sanitized);
    PROXY_CHECK(knurl_proxy_headers_add(&inbound, "Host", "client.test") ==
                KNURL_PROXY_HEADER_OK);
    for (kind = 1; kind <= 11; ++kind) {
        target = NULL;
        authority = NULL;
        PROXY_CHECK(knurl_proxy_director_prepare(
                        &route, KNURL_HTTP_GET, "/original", "a=1",
                        "client.test", kind == 5 || kind == 6, NULL, &inbound,
                        &sanitized, &out, &target, &authority,
                        &error) == KNURL_INVALID);
        PROXY_CHECK(target == NULL);
        PROXY_CHECK(authority == NULL);
        knurl_proxy_director_cleanup(&out);
    }
    PROXY_CHECK(knurl_proxy_director_prepare(
                    &route, KNURL_HTTP_GET, "/original", "a=%0a", "client.test",
                    0, NULL, &inbound, &sanitized, &out, &target, &authority,
                    &error) == KNURL_INVALID);
    knurl_proxy_director_cleanup(&out);
    knurl_proxy_headers_cleanup(&sanitized);
    knurl_proxy_headers_cleanup(&inbound);
}

int main(void)
{
    check_change();
    check_invalid();
    return 0;
}
