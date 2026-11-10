#include "request_internal.h"
#include "response_internal.h"
#include "reply_internal.h"
#include "body_internal.h"
#include <knurl/loop.h>

#include <stdlib.h>
#include <string.h>

knurl_bytes knurl_view(const char *text)
{
    knurl_bytes view;
    view.data = text;
    view.length = text != NULL ? strlen(text) : 0;
    return view;
}

knurl_server *knurl_request_server(const knurl_request *request)
{
    return request != NULL ? request->server : NULL;
}

knurl_loop *knurl_request_loop(const knurl_request *request)
{
    return request != NULL ? knurl_server_loop(request->server) : NULL;
}

knurl_http_method knurl_request_method(const knurl_request *request)
{
    return request != NULL ? request->method : KNURL_HTTP_ANY;
}

#define VIEW_ACCESSOR(name, field)                                             \
    knurl_bytes knurl_request_##name(const knurl_request *request)             \
    {                                                                          \
        return request != NULL ? request->field : knurl_view(NULL);            \
    }
VIEW_ACCESSOR(method_token, method_token)
VIEW_ACCESSOR(version, version)
VIEW_ACCESSOR(target, target)
VIEW_ACCESSOR(raw_path, raw_path)
VIEW_ACCESSOR(path, path)
VIEW_ACCESSOR(query, query)
VIEW_ACCESSOR(authority, authority)
VIEW_ACCESSOR(effective_address, effective_address)
VIEW_ACCESSOR(correlation_id, correlation_id)
#undef VIEW_ACCESSOR

size_t knurl_request_listener_index(const knurl_request *request)
{
    return request != NULL ? request->listener : (size_t)-1;
}

size_t knurl_request_header_count(const knurl_request *request)
{
    return request != NULL ? request->header_count : 0;
}

size_t knurl_request_query_count(const knurl_request *request)
{
    return request != NULL ? request->query_count : 0;
}

static knurl_status pair_at(const knurl_request *request,
                            const knurl_header *pairs, size_t count,
                            size_t index, knurl_header *out, knurl_error *error)
{
    if (out != NULL) {
        out->name = knurl_view(NULL);
        out->value = knurl_view(NULL);
    }
    if (request == NULL || out == NULL || index >= count)
        return knurl_fail(
            error, KNURL_INVALID, "request_pair_at",
            "request, output and an index within the collection are required");
    *out = pairs[index];
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_request_header_at(const knurl_request *request, size_t index,
                                     knurl_header *out, knurl_error *error)
{
    return pair_at(request, request != NULL ? request->headers : NULL,
                   knurl_request_header_count(request), index, out, error);
}

knurl_status knurl_request_query_at(const knurl_request *request, size_t index,
                                    knurl_header *out, knurl_error *error)
{
    return pair_at(request, request != NULL ? request->queries : NULL,
                   knurl_request_query_count(request), index, out, error);
}

static int name_equal(knurl_bytes value, const char *name, int fold)
{
    size_t i;
    unsigned char left;
    unsigned char right;
    if (value.length != strlen(name))
        return 0;
    for (i = 0; i < value.length; ++i) {
        left = ((const unsigned char *)value.data)[i];
        right = (unsigned char)name[i];
        if (fold) {
            if (left >= 'A' && left <= 'Z')
                left += 'a' - 'A';
            if (right >= 'A' && right <= 'Z')
                right += 'a' - 'A';
        }
        if (left != right)
            return 0;
    }
    return 1;
}

static knurl_status first(const knurl_request *request,
                          const knurl_header *pairs, size_t count,
                          const char *name, int fold, knurl_bytes *out,
                          knurl_error *error)
{
    size_t i;
    if (out != NULL)
        *out = knurl_view(NULL);
    if (request == NULL || name == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "request_first",
                          "request, name and output are required");
    for (i = 0; i < count; ++i) {
        if (name_equal(pairs[i].name, name, fold)) {
            *out = pairs[i].value;
            knurl_error_clear(error);
            return KNURL_OK;
        }
    }
    knurl_error_clear(error);
    return KNURL_EOF;
}

knurl_status knurl_request_header_first(const knurl_request *request,
                                        const char *name, knurl_bytes *out,
                                        knurl_error *error)
{
    return first(request, request != NULL ? request->headers : NULL,
                 knurl_request_header_count(request), name, 1, out, error);
}

knurl_status knurl_request_query_first(const knurl_request *request,
                                       const char *name, knurl_bytes *out,
                                       knurl_error *error)
{
    return first(request, request != NULL ? request->queries : NULL,
                 knurl_request_query_count(request), name, 0, out, error);
}

knurl_status knurl_request_path_param(const knurl_request *request,
                                      const char *name, knurl_bytes *out,
                                      knurl_error *error)
{
    return first(request, request != NULL ? request->params : NULL,
                 request != NULL ? request->param_count : 0, name, 0, out,
                 error);
}

knurl_endpoint knurl_request_peer(const knurl_request *request)
{
    knurl_endpoint empty;
    empty.address = knurl_view(NULL);
    empty.port = 0;
    return request != NULL ? request->peer : empty;
}

knurl_endpoint knurl_request_local(const knurl_request *request)
{
    knurl_endpoint empty;
    empty.address = knurl_view(NULL);
    empty.port = 0;
    return request != NULL ? request->local : empty;
}

int knurl_request_is_tls(const knurl_request *request)
{
    return request != NULL && request->tls;
}

knurl_status knurl_request_content_length(const knurl_request *request,
                                          int *present, knurl_u64 *out,
                                          knurl_error *error)
{
    if (request == NULL || present == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "request_content_length",
                          "request, presence and length outputs are required");
    *present = request->has_length;
    *out = request->content_length;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_request_body_bytes(const knurl_request *request,
                                      knurl_bytes *out, knurl_error *error)
{
    if (out != NULL)
        *out = knurl_view(NULL);
    if (request == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "request_body_bytes",
                          "request and output are required");
    if (!request->body_ready)
        return knurl_fail(error, KNURL_STATE, "request_body_bytes",
                          "body is not a completed in-memory value");
    *out = request->body;
    knurl_error_clear(error);
    return KNURL_OK;
}

void *knurl_request_userdata(const knurl_request *request)
{
    return request != NULL ? request->userdata : NULL;
}

void knurl_request_set_userdata(knurl_request *request, void *userdata)
{
    if (request != NULL)
        request->userdata = userdata;
}

void knurl_request_cleanup(knurl_request *request)
{
    size_t i;
    if (request->reply != NULL) {
        request->reply->owner = NULL;
        (void)knurl_reply_destroy(request->reply, NULL);
    }
    request->reply = NULL;
    knurl_response_release(request->response);
    request->response = NULL;
    if (request->codec_cleanup != NULL)
        request->codec_cleanup(request->codec_data);
    request->codec_data = NULL;
    request->codec_cleanup = NULL;
    knurl_body_release(request->body_store);
    request->body_store = NULL;
    free((void *)request->path.data);
    for (i = 0; i < request->query_count; ++i) {
        free((void *)request->queries[i].name.data);
        free((void *)request->queries[i].value.data);
    }
    free(request->queries);
    free(request->params);
    free(request->headers);
}
