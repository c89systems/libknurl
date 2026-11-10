#include "proxy_local.h"

#include "proxy_common.h"

#include <stdlib.h>
#include <string.h>

void knurl_proxy_local_init(knurl_proxy_local_response *response)
{
    if (response != NULL)
        memset(response, 0, sizeof(*response));
}

void knurl_proxy_local_cleanup(knurl_proxy_local_response *response)
{
    if (response == NULL)
        return;
    knurl_proxy_headers_cleanup(&response->headers);
    free(response->body);
    memset(response, 0, sizeof(*response));
}

knurl_status knurl_proxy_local_respond(knurl_proxy_local_response *response,
                                       int status, const void *body,
                                       size_t body_length, knurl_error *error)
{
    unsigned char *copy;

    if (response == NULL) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy local response is required");
        return KNURL_INVALID;
    }
    if (status < 200 || status > 599 || status == 101 ||
        body_length > KNURL_PROXY_LOCAL_BODY_LIMIT ||
        (body_length != 0u && body == NULL) ||
        ((status == 204 || status == 205 || status == 304) &&
         body_length != 0u)) {
        response->failure = KNURL_INVALID;
        knurl_proxy_error(
            error, KNURL_INVALID,
            "proxy local status or body is invalid or exceeds 65536 bytes");
        return KNURL_INVALID;
    }
    copy = NULL;
    if (body_length != 0u) {
        copy = (unsigned char *)malloc(body_length);
        if (copy == NULL) {
            response->failure = KNURL_NOMEM;
            knurl_proxy_error(error, KNURL_NOMEM,
                              "failed to copy proxy local response body");
            return KNURL_NOMEM;
        }
        memcpy(copy, body, body_length);
    }
    free(response->body);
    response->body = copy;
    response->body_length = body_length;
    response->status = status;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_proxy_local_add_header(knurl_proxy_local_response *response,
                                          const char *name, const char *value,
                                          knurl_error *error)
{
    knurl_proxy_header_status result;

    if (response == NULL || response->status == 0 ||
        !knurl_proxy_response_header_editable(name)) {
        if (response != NULL)
            response->failure = KNURL_INVALID;
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy local response cannot edit transport headers");
        return KNURL_INVALID;
    }
    result = knurl_proxy_headers_add(&response->headers, name, value);
    if (result != KNURL_PROXY_HEADER_OK) {
        response->failure =
            result == KNURL_PROXY_HEADER_NOMEM ? KNURL_NOMEM : KNURL_INVALID;
        knurl_proxy_error(
            error, response->failure,
            "proxy local response header is invalid or exceeds limits");
        return response->failure;
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_proxy_local_error(const knurl_proxy_route_data *route,
                             const knurl_error *cause, int default_status,
                             knurl_proxy_local_response *response)
{
    static const char bad_gateway[] = "bad gateway";
    static const char timeout[] = "gateway timeout";
    const char *body;
    knurl_error error;
    knurl_status status;

    knurl_proxy_local_init(response);
    if (route != NULL && route->on_error != NULL) {
        knurl_error_clear(&error);
        status = route->on_error(cause, default_status, response,
                                 route->on_error_userdata, &error);
        if (status == KNURL_OK && response->failure == KNURL_OK &&
            response->status != 0)
            return;
        knurl_proxy_local_cleanup(response);
    }
    body = default_status == 504 ? timeout : bad_gateway;
    (void)knurl_proxy_local_respond(response, default_status, body,
                                    strlen(body), NULL);
}
