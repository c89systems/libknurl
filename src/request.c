#include "request_internal.h"
#include "response_internal.h"
#include "reply_internal.h"
#include <rotmos/loop.h>

#include <stdlib.h>
#include <string.h>

rotmos_bytes rotmos_view(const char *text)
{
    rotmos_bytes view;
    view.data = text;
    view.length = text != NULL ? strlen(text) : 0;
    return view;
}

rotmos_server *rotmos_request_server(const rotmos_request *request)
{
    return request != NULL ? request->server : NULL;
}

rotmos_loop *rotmos_request_loop(const rotmos_request *request)
{
    return request != NULL ? rotmos_server_loop(request->server) : NULL;
}

rotmos_http_method rotmos_request_method(const rotmos_request *request)
{
    return request != NULL ? request->method : ROTMOS_HTTP_ANY;
}

#define VIEW_ACCESSOR(name, field)                                             \
    rotmos_bytes rotmos_request_##name(const rotmos_request *request)          \
    {                                                                          \
        return request != NULL ? request->field : rotmos_view(NULL);           \
    }
VIEW_ACCESSOR(method_token, method_token)
VIEW_ACCESSOR(version, version)
VIEW_ACCESSOR(target, target)
VIEW_ACCESSOR(raw_path, raw_path)
VIEW_ACCESSOR(path, path)
VIEW_ACCESSOR(query, query)
VIEW_ACCESSOR(authority, authority)
#undef VIEW_ACCESSOR

size_t rotmos_request_listener_index(const rotmos_request *request)
{
    return request != NULL ? request->listener : (size_t)-1;
}

size_t rotmos_request_header_count(const rotmos_request *request)
{
    return request != NULL ? request->header_count : 0;
}

size_t rotmos_request_query_count(const rotmos_request *request)
{
    return request != NULL ? request->query_count : 0;
}

static rotmos_status pair_at(const rotmos_request *request,
                             const rotmos_header *pairs, size_t count,
                             size_t index, rotmos_header *out,
                             rotmos_error *error)
{
    if (out != NULL) {
        out->name = rotmos_view(NULL);
        out->value = rotmos_view(NULL);
    }
    if (request == NULL || out == NULL || index >= count)
        return rotmos_fail(
            error, ROTMOS_INVALID, "request_pair_at",
            "request, output and an index within the collection are required");
    *out = pairs[index];
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_request_header_at(const rotmos_request *request,
                                       size_t index, rotmos_header *out,
                                       rotmos_error *error)
{
    return pair_at(request, request != NULL ? request->headers : NULL,
                   rotmos_request_header_count(request), index, out, error);
}

rotmos_status rotmos_request_query_at(const rotmos_request *request,
                                      size_t index, rotmos_header *out,
                                      rotmos_error *error)
{
    return pair_at(request, request != NULL ? request->queries : NULL,
                   rotmos_request_query_count(request), index, out, error);
}

static int name_equal(rotmos_bytes value, const char *name, int fold)
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

static rotmos_status first(const rotmos_request *request,
                           const rotmos_header *pairs, size_t count,
                           const char *name, int fold, rotmos_bytes *out,
                           rotmos_error *error)
{
    size_t i;
    if (out != NULL)
        *out = rotmos_view(NULL);
    if (request == NULL || name == NULL || out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "request_first",
                           "request, name and output are required");
    for (i = 0; i < count; ++i) {
        if (name_equal(pairs[i].name, name, fold)) {
            *out = pairs[i].value;
            rotmos_error_clear(error);
            return ROTMOS_OK;
        }
    }
    rotmos_error_clear(error);
    return ROTMOS_EOF;
}

rotmos_status rotmos_request_header_first(const rotmos_request *request,
                                          const char *name, rotmos_bytes *out,
                                          rotmos_error *error)
{
    return first(request, request != NULL ? request->headers : NULL,
                 rotmos_request_header_count(request), name, 1, out, error);
}

rotmos_status rotmos_request_query_first(const rotmos_request *request,
                                         const char *name, rotmos_bytes *out,
                                         rotmos_error *error)
{
    return first(request, request != NULL ? request->queries : NULL,
                 rotmos_request_query_count(request), name, 0, out, error);
}

rotmos_status rotmos_request_path_param(const rotmos_request *request,
                                        const char *name, rotmos_bytes *out,
                                        rotmos_error *error)
{
    return first(request, request != NULL ? request->params : NULL,
                 request != NULL ? request->param_count : 0, name, 0, out,
                 error);
}

rotmos_endpoint rotmos_request_peer(const rotmos_request *request)
{
    rotmos_endpoint empty;
    empty.address = rotmos_view(NULL);
    empty.port = 0;
    return request != NULL ? request->peer : empty;
}

rotmos_endpoint rotmos_request_local(const rotmos_request *request)
{
    rotmos_endpoint empty;
    empty.address = rotmos_view(NULL);
    empty.port = 0;
    return request != NULL ? request->local : empty;
}

int rotmos_request_is_tls(const rotmos_request *request)
{
    return request != NULL && request->tls;
}

rotmos_status rotmos_request_content_length(const rotmos_request *request,
                                            int *present, rotmos_u64 *out,
                                            rotmos_error *error)
{
    if (request == NULL || present == NULL || out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "request_content_length",
                           "request, presence and length outputs are required");
    *present = request->has_length;
    *out = request->content_length;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_request_body_bytes(const rotmos_request *request,
                                        rotmos_bytes *out, rotmos_error *error)
{
    if (out != NULL)
        *out = rotmos_view(NULL);
    if (request == NULL || out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "request_body_bytes",
                           "request and output are required");
    if (!request->body_ready)
        return rotmos_fail(error, ROTMOS_STATE, "request_body_bytes",
                           "body is not a completed in-memory value");
    *out = request->body;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void *rotmos_request_userdata(const rotmos_request *request)
{
    return request != NULL ? request->userdata : NULL;
}

void rotmos_request_set_userdata(rotmos_request *request, void *userdata)
{
    if (request != NULL)
        request->userdata = userdata;
}

void rotmos_request_cleanup(rotmos_request *request)
{
    size_t i;
    if (request->reply != NULL) {
        request->reply->owner = NULL;
        (void)rotmos_reply_destroy(request->reply, NULL);
    }
    request->reply = NULL;
    rotmos_response_release(request->response);
    request->response = NULL;
    free((void *)request->path.data);
    for (i = 0; i < request->query_count; ++i) {
        free((void *)request->queries[i].name.data);
        free((void *)request->queries[i].value.data);
    }
    free(request->queries);
    free(request->params);
    free(request->headers);
}
