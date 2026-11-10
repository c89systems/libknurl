#include "proxy_response.h"

#include "proxy_common.h"

#include <string.h>

int knurl_proxy_response_status(const knurl_proxy_response *response)
{
    return response != NULL ? response->status : 0;
}

size_t knurl_proxy_response_header_count(const knurl_proxy_response *response)
{
    return response != NULL && response->headers != NULL
               ? response->headers->count
               : 0u;
}

knurl_status
knurl_proxy_response_header_at(const knurl_proxy_response *response,
                               size_t index, const char **name,
                               const char **value)
{
    if (name != NULL)
        *name = NULL;
    if (value != NULL)
        *value = NULL;
    if (response == NULL || response->headers == NULL || name == NULL ||
        value == NULL || index >= response->headers->count)
        return KNURL_INVALID;
    *name = response->headers->fields[index].name;
    *value = response->headers->fields[index].value;
    return KNURL_OK;
}

knurl_status knurl_proxy_response_set_status(knurl_proxy_response *response,
                                             int status, knurl_error *error)
{
    int body_allowed;

    if (response == NULL || response->upstream == NULL) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy response view is required");
        return KNURL_INVALID;
    }
    body_allowed = !response->upstream->head_request && status != 204 &&
                   status != 205 && status != 304;
    if (status < 200 || status > 599 || status == 101 ||
        body_allowed != response->upstream->body_allowed) {
        response->failure = KNURL_INVALID;
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy response status conflicts with streamed body");
        return KNURL_INVALID;
    }
    response->status = status;
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status
knurl_proxy_response_header_status(knurl_proxy_header_status status,
                                   knurl_error *error)
{
    if (status == KNURL_PROXY_HEADER_OK) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (status == KNURL_PROXY_HEADER_NOMEM) {
        knurl_proxy_error(error, KNURL_NOMEM,
                          "failed to allocate proxy response header");
        return KNURL_NOMEM;
    }
    knurl_proxy_error(error, KNURL_INVALID,
                      "proxy response header is invalid or exceeds limits");
    return KNURL_INVALID;
}

knurl_status knurl_proxy_response_add_header(knurl_proxy_response *response,
                                             const char *name,
                                             const char *value,
                                             knurl_error *error)
{
    knurl_status status;

    if (response == NULL || response->headers == NULL ||
        !knurl_proxy_response_header_editable(name)) {
        if (response != NULL)
            response->failure = KNURL_INVALID;
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy response cannot edit transport headers");
        return KNURL_INVALID;
    }
    status = knurl_proxy_response_header_status(
        knurl_proxy_headers_add(response->headers, name, value), error);
    if (status != KNURL_OK)
        response->failure = status;
    return status;
}

knurl_status knurl_proxy_response_remove_header(knurl_proxy_response *response,
                                                const char *name,
                                                knurl_error *error)
{
    if (response == NULL || response->headers == NULL ||
        !knurl_proxy_response_header_editable(name)) {
        if (response != NULL)
            response->failure = KNURL_INVALID;
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy response cannot edit transport headers");
        return KNURL_INVALID;
    }
    knurl_proxy_headers_remove(response->headers, name);
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_proxy_response_set_header(knurl_proxy_response *response,
                                             const char *name,
                                             const char *value,
                                             knurl_error *error)
{
    knurl_status status;

    if (response == NULL || response->headers == NULL ||
        !knurl_proxy_response_header_editable(name)) {
        if (response != NULL)
            response->failure = KNURL_INVALID;
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy response cannot edit transport headers");
        return KNURL_INVALID;
    }
    status = knurl_proxy_response_header_status(
        knurl_proxy_headers_set(response->headers, name, value), error);
    if (status != KNURL_OK)
        response->failure = status;
    return status;
}

knurl_status knurl_proxy_response_apply(
    const knurl_proxy_http_response *upstream, knurl_proxy_headers *sanitized,
    knurl_proxy_modify_response_fn modify, void *userdata,
    int *downstream_status, knurl_error *error)
{
    knurl_proxy_response view;
    knurl_status status;

    if (upstream == NULL || sanitized == NULL || downstream_status == NULL ||
        upstream->status < 200 || upstream->status > 599 ||
        upstream->status == 101) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "proxy response requires final non-upgrade metadata");
        return KNURL_INVALID;
    }
    view.upstream = upstream;
    view.headers = sanitized;
    view.status = upstream->status;
    view.failure = KNURL_OK;
    if (modify != NULL) {
        status = modify(&view, userdata, error);
        if (status != KNURL_OK)
            return status;
        if (view.failure != KNURL_OK) {
            knurl_proxy_error(error, view.failure,
                              "proxy response hook attempted an invalid edit");
            return view.failure;
        }
    }
    *downstream_status = view.status;
    knurl_error_clear(error);
    return KNURL_OK;
}
