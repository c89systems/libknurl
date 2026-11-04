#include "proxy_response.h"

#include "proxy_common.h"

#include <string.h>

int rotmos_proxy_response_status(const rotmos_proxy_response *response)
{
    return response != NULL ? response->status : 0;
}

size_t rotmos_proxy_response_header_count(const rotmos_proxy_response *response)
{
    return response != NULL && response->headers != NULL
               ? response->headers->count
               : 0u;
}

rotmos_status
rotmos_proxy_response_header_at(const rotmos_proxy_response *response,
                                size_t index, const char **name,
                                const char **value)
{
    if (name != NULL)
        *name = NULL;
    if (value != NULL)
        *value = NULL;
    if (response == NULL || response->headers == NULL || name == NULL ||
        value == NULL || index >= response->headers->count)
        return ROTMOS_INVALID;
    *name = response->headers->fields[index].name;
    *value = response->headers->fields[index].value;
    return ROTMOS_OK;
}

rotmos_status rotmos_proxy_response_set_status(rotmos_proxy_response *response,
                                               int status, rotmos_error *error)
{
    int body_allowed;

    if (response == NULL || response->upstream == NULL) {
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy response view is required");
        return ROTMOS_INVALID;
    }
    body_allowed = !response->upstream->head_request && status != 204 &&
                   status != 205 && status != 304;
    if (status < 200 || status > 599 || status == 101 ||
        body_allowed != response->upstream->body_allowed) {
        response->failure = ROTMOS_INVALID;
        rotmos_proxy_error(
            error, ROTMOS_INVALID,
            "proxy response status conflicts with streamed body");
        return ROTMOS_INVALID;
    }
    response->status = status;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static rotmos_status
rotmos_proxy_response_header_status(rotmos_proxy_header_status status,
                                    rotmos_error *error)
{
    if (status == ROTMOS_PROXY_HEADER_OK) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (status == ROTMOS_PROXY_HEADER_NOMEM) {
        rotmos_proxy_error(error, ROTMOS_NOMEM,
                           "failed to allocate proxy response header");
        return ROTMOS_NOMEM;
    }
    rotmos_proxy_error(error, ROTMOS_INVALID,
                       "proxy response header is invalid or exceeds limits");
    return ROTMOS_INVALID;
}

rotmos_status rotmos_proxy_response_add_header(rotmos_proxy_response *response,
                                               const char *name,
                                               const char *value,
                                               rotmos_error *error)
{
    rotmos_status status;

    if (response == NULL || response->headers == NULL ||
        !rotmos_proxy_response_header_editable(name)) {
        if (response != NULL)
            response->failure = ROTMOS_INVALID;
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy response cannot edit transport headers");
        return ROTMOS_INVALID;
    }
    status = rotmos_proxy_response_header_status(
        rotmos_proxy_headers_add(response->headers, name, value), error);
    if (status != ROTMOS_OK)
        response->failure = status;
    return status;
}

rotmos_status
rotmos_proxy_response_remove_header(rotmos_proxy_response *response,
                                    const char *name, rotmos_error *error)
{
    if (response == NULL || response->headers == NULL ||
        !rotmos_proxy_response_header_editable(name)) {
        if (response != NULL)
            response->failure = ROTMOS_INVALID;
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy response cannot edit transport headers");
        return ROTMOS_INVALID;
    }
    rotmos_proxy_headers_remove(response->headers, name);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_proxy_response_set_header(rotmos_proxy_response *response,
                                               const char *name,
                                               const char *value,
                                               rotmos_error *error)
{
    rotmos_status status;

    if (response == NULL || response->headers == NULL ||
        !rotmos_proxy_response_header_editable(name)) {
        if (response != NULL)
            response->failure = ROTMOS_INVALID;
        rotmos_proxy_error(error, ROTMOS_INVALID,
                           "proxy response cannot edit transport headers");
        return ROTMOS_INVALID;
    }
    status = rotmos_proxy_response_header_status(
        rotmos_proxy_headers_set(response->headers, name, value), error);
    if (status != ROTMOS_OK)
        response->failure = status;
    return status;
}

rotmos_status rotmos_proxy_response_apply(
    const rotmos_proxy_http_response *upstream, rotmos_proxy_headers *sanitized,
    rotmos_proxy_modify_response_fn modify, void *userdata,
    int *downstream_status, rotmos_error *error)
{
    rotmos_proxy_response view;
    rotmos_status status;

    if (upstream == NULL || sanitized == NULL || downstream_status == NULL ||
        upstream->status < 200 || upstream->status > 599 ||
        upstream->status == 101) {
        rotmos_proxy_error(
            error, ROTMOS_INVALID,
            "proxy response requires final non-upgrade metadata");
        return ROTMOS_INVALID;
    }
    view.upstream = upstream;
    view.headers = sanitized;
    view.status = upstream->status;
    view.failure = ROTMOS_OK;
    if (modify != NULL) {
        status = modify(&view, userdata, error);
        if (status != ROTMOS_OK)
            return status;
        if (view.failure != ROTMOS_OK) {
            rotmos_proxy_error(error, view.failure,
                               "proxy response hook attempted an invalid edit");
            return view.failure;
        }
    }
    *downstream_status = view.status;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
