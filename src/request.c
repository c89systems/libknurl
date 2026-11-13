#include "request_internal.h"
#include <knurl/kore.h>
#include <knurl/websocket.h>
#include <knurl/xml.h>
#include <knurl/lonejson.h>
#include <knurl/response.h>
#include <knurl/request.h>
#include <knurl/webdav.h>
#include <knurl/body.h>
#include <knurl/dispatch.h>
#include <knurl/channel.h>
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
    return request != NULL ? request->impl->server : NULL;
}

knurl_loop *knurl_request_loop(const knurl_request *request)
{
    return request != NULL ? knurl_server_loop(request->impl->server) : NULL;
}

knurl_http_method knurl_request_method(const knurl_request *request)
{
    return request != NULL ? request->impl->method : KNURL_HTTP_ANY;
}

#define VIEW_ACCESSOR(name, field)                                             \
    knurl_bytes knurl_request_##name(const knurl_request *request)             \
    {                                                                          \
        return request != NULL ? request->field : knurl_view(NULL);            \
    }
VIEW_ACCESSOR(method_token, impl->method_token)
VIEW_ACCESSOR(version, impl->version)
VIEW_ACCESSOR(target, impl->target)
VIEW_ACCESSOR(raw_path, impl->raw_path)
VIEW_ACCESSOR(path, impl->path)
VIEW_ACCESSOR(query, impl->query)
VIEW_ACCESSOR(authority, impl->authority)
VIEW_ACCESSOR(effective_address, impl->effective_address)
VIEW_ACCESSOR(correlation_id, impl->correlation_id)
#undef VIEW_ACCESSOR

size_t knurl_request_listener_index(const knurl_request *request)
{
    return request != NULL ? request->impl->listener : (size_t)-1;
}

size_t knurl_request_header_count(const knurl_request *request)
{
    return request != NULL ? request->impl->header_count : 0;
}

size_t knurl_request_query_count(const knurl_request *request)
{
    return request != NULL ? request->impl->query_count : 0;
}

size_t knurl_request_trailer_count(const knurl_request *request)
{
    return request != NULL && request->impl->outcome.body_complete
               ? request->impl->trailer_count
               : 0;
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
    return pair_at(request, request != NULL ? request->impl->headers : NULL,
                   knurl_request_header_count(request), index, out, error);
}

knurl_status knurl_request_trailer_at(const knurl_request *request,
                                      size_t index, knurl_header *out,
                                      knurl_error *error)
{
    if (out != NULL) {
        out->name = knurl_view(NULL);
        out->value = knurl_view(NULL);
    }
    if (request != NULL && !request->impl->outcome.body_complete)
        return knurl_fail(error, KNURL_STATE, "request_trailers",
                          "body trailers require completed input");
    return pair_at(request, request != NULL ? request->impl->trailers : NULL,
                   knurl_request_trailer_count(request), index, out, error);
}

knurl_status knurl_request_query_at(const knurl_request *request, size_t index,
                                    knurl_header *out, knurl_error *error)
{
    return pair_at(request, request != NULL ? request->impl->queries : NULL,
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
    return first(request, request != NULL ? request->impl->headers : NULL,
                 knurl_request_header_count(request), name, 1, out, error);
}

knurl_status knurl_request_query_first(const knurl_request *request,
                                       const char *name, knurl_bytes *out,
                                       knurl_error *error)
{
    return first(request, request != NULL ? request->impl->queries : NULL,
                 knurl_request_query_count(request), name, 0, out, error);
}

knurl_status knurl_request_trailer_first(const knurl_request *request,
                                         const char *name, knurl_bytes *out,
                                         knurl_error *error)
{
    if (out != NULL)
        *out = knurl_view(NULL);
    if (request != NULL && !request->impl->outcome.body_complete)
        return knurl_fail(error, KNURL_STATE, "request_trailers",
                          "body trailers require completed input");
    return first(request, request != NULL ? request->impl->trailers : NULL,
                 knurl_request_trailer_count(request), name, 1, out, error);
}

knurl_status knurl_request_path_param(const knurl_request *request,
                                      const char *name, knurl_bytes *out,
                                      knurl_error *error)
{
    return first(request, request != NULL ? request->impl->params : NULL,
                 request != NULL ? request->impl->param_count : 0, name, 0, out,
                 error);
}

knurl_endpoint knurl_request_peer(const knurl_request *request)
{
    knurl_endpoint empty;
    empty.address = knurl_view(NULL);
    empty.port = 0;
    return request != NULL ? request->impl->peer : empty;
}

knurl_endpoint knurl_request_local(const knurl_request *request)
{
    knurl_endpoint empty;
    empty.address = knurl_view(NULL);
    empty.port = 0;
    return request != NULL ? request->impl->local : empty;
}

int knurl_request_is_tls(const knurl_request *request)
{
    return request != NULL && request->impl->tls;
}

knurl_status knurl_request_content_length(const knurl_request *request,
                                          int *present, knurl_u64 *out,
                                          knurl_error *error)
{
    if (request == NULL || present == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "request_content_length",
                          "request, presence and length outputs are required");
    *present = request->impl->has_length;
    *out = request->impl->content_length;
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
    if (!request->impl->body_ready)
        return knurl_fail(error, KNURL_STATE, "request_body_bytes",
                          "body is not a completed in-memory value");
    *out = request->impl->body;
    knurl_error_clear(error);
    return KNURL_OK;
}

void *knurl_request_userdata(const knurl_request *request)
{
    return request != NULL ? request->impl->userdata : NULL;
}

void knurl_request_set_userdata(knurl_request *request, void *userdata)
{
    if (request != NULL)
        request->impl->userdata = userdata;
}

void knurl_request_cleanup(knurl_request *request)
{
    size_t i;
    if (request->impl->reply != NULL) {
        request->impl->reply->impl->owner = NULL;
        (void)knurl_reply_destroy(request->impl->reply, NULL);
    }
    request->impl->reply = NULL;
    knurl_response_release(request->impl->response);
    request->impl->response = NULL;
    if (request->impl->codec_cleanup != NULL)
        request->impl->codec_cleanup(request->impl->codec_data);
    request->impl->codec_data = NULL;
    request->impl->codec_cleanup = NULL;
    knurl_body_release(request->impl->body_store);
    request->impl->body_store = NULL;
    free((void *)request->impl->path.data);
    for (i = 0; i < request->impl->query_count; ++i) {
        free((void *)request->impl->queries[i].name.data);
        free((void *)request->impl->queries[i].value.data);
    }
    free(request->impl->queries);
    free(request->impl->params);
    free(request->impl->headers);
    for (i = 0; i < request->impl->trailer_count; ++i)
        free((void *)request->impl->trailers[i].name.data);
    free(request->impl->trailers);
}

void knurl_request_receiver_init(knurl_request *receiver)
{
    static const knurl_request methods = {knurl_request_query_long,
                                          knurl_request_param_long,
                                          knurl_request_query_u64,
                                          knurl_request_param_u64,
                                          knurl_request_query_boolean,
                                          knurl_request_param_boolean,
                                          knurl_kore_request,
                                          knurl_websocket_accept,
                                          knurl_request_xml_into,
                                          knurl_request_json_into,
                                          knurl_request_json_validate,
                                          knurl_request_json_array_each,
                                          knurl_request_response,
                                          knurl_request_server,
                                          knurl_request_loop,
                                          knurl_request_method,
                                          knurl_request_method_token,
                                          knurl_request_version,
                                          knurl_request_target,
                                          knurl_request_raw_path,
                                          knurl_request_path,
                                          knurl_request_query,
                                          knurl_request_authority,
                                          knurl_request_listener_index,
                                          knurl_request_header_count,
                                          knurl_request_trailer_count,
                                          knurl_request_trailer_at,
                                          knurl_request_trailer_first,
                                          knurl_request_header_at,
                                          knurl_request_header_first,
                                          knurl_request_query_count,
                                          knurl_request_query_at,
                                          knurl_request_query_first,
                                          knurl_request_path_param,
                                          knurl_request_peer,
                                          knurl_request_local,
                                          knurl_request_effective_address,
                                          knurl_request_correlation_id,
                                          knurl_request_event,
                                          knurl_request_is_tls,
                                          knurl_request_content_length,
                                          knurl_request_body_bytes,
                                          knurl_request_userdata,
                                          knurl_request_set_userdata,
                                          knurl_webdav_request_principal,
                                          knurl_request_body,
                                          knurl_request_body_materialize,
                                          knurl_request_body_copy,
                                          knurl_request_body_spill,
                                          knurl_request_body_resume,
                                          knurl_request_takeover,
                                          NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_request_impl *)receiver;
}
