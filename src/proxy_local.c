#include "proxy_local.h"

#include "proxy_common.h"

#include <stdlib.h>
#include <string.h>

void rotmos_proxy_local_init(rotmos_proxy_local_response *response)
{
    if (response != NULL)
        memset(response, 0, sizeof(*response));
}

void rotmos_proxy_local_cleanup(rotmos_proxy_local_response *response)
{
    if (response == NULL)
        return;
    rotmos_proxy_headers_cleanup(&response->headers);
    free(response->body);
    memset(response, 0, sizeof(*response));
}

rotmos_status rotmos_proxy_local_respond(rotmos_proxy_local_response *response,
                                         int status, const void *body,
                                         size_t body_length,
                                         rotmos_error *error)
{
    unsigned char *copy;

    if (response == NULL) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy local response is required");
        return ROTMOS_INVALID;
    }
    if (status < 200 || status > 599 || status == 101 ||
        body_length > ROTMOS_PROXY_LOCAL_BODY_LIMIT ||
        (body_length != 0u && body == NULL) ||
        ((status == 204 || status == 205 || status == 304) &&
         body_length != 0u)) {
        response->failure = ROTMOS_INVALID;
        rotmos_proxy_error(
            error, ROTMOS_INVALID,
            "proxy local status or body is invalid or exceeds 65536 bytes");
        return ROTMOS_INVALID;
    }
    copy = NULL;
    if (body_length != 0u) {
        copy = (unsigned char *)malloc(body_length);
        if (copy == NULL) {
            response->failure = ROTMOS_NOMEM;
            rotmos_proxy_error(error, ROTMOS_NOMEM,
                               "failed to copy proxy local response body");
            return ROTMOS_NOMEM;
        }
        memcpy(copy, body, body_length);
    }
    free(response->body);
    response->body = copy;
    response->body_length = body_length;
    response->status = status;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status
rotmos_proxy_local_add_header(rotmos_proxy_local_response *response,
                              const char *name, const char *value,
                              rotmos_error *error)
{
    rotmos_proxy_header_status result;

    if (response == NULL || response->status == 0 ||
        !rotmos_proxy_response_header_editable(name)) {
        if (response != NULL)
            response->failure = ROTMOS_INVALID;
        rotmos_proxy_error(
            error, ROTMOS_INVALID,
            "proxy local response cannot edit transport headers");
        return ROTMOS_INVALID;
    }
    result = rotmos_proxy_headers_add(&response->headers, name, value);
    if (result != ROTMOS_PROXY_HEADER_OK) {
        response->failure =
            result == ROTMOS_PROXY_HEADER_NOMEM ? ROTMOS_NOMEM : ROTMOS_INVALID;
        rotmos_proxy_error(
            error, response->failure,
            "proxy local response header is invalid or exceeds limits");
        return response->failure;
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_proxy_local_error(const rotmos_proxy_route_data *route,
                              const rotmos_error *cause, int default_status,
                              rotmos_proxy_local_response *response)
{
    static const char bad_gateway[] = "bad gateway";
    static const char timeout[] = "gateway timeout";
    const char *body;
    rotmos_error error;
    rotmos_status status;

    rotmos_proxy_local_init(response);
    if (route != NULL && route->on_error != NULL) {
        rotmos_error_clear(&error);
        status = route->on_error(cause, default_status, response,
                                 route->on_error_userdata, &error);
        if (status == ROTMOS_OK && response->failure == ROTMOS_OK &&
            response->status != 0)
            return;
        rotmos_proxy_local_cleanup(response);
    }
    body = default_status == 504 ? timeout : bad_gateway;
    (void)rotmos_proxy_local_respond(response, default_status, body,
                                     strlen(body), NULL);
}
