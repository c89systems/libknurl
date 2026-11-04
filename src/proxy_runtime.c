#include "proxy_runtime.h"
#include "proxy_runtime_internal.h"
#include "proxy_ws_runtime.h"
#include "proxy_route_internal.h"
#include "proxy_director.h"
#include "proxy_local.h"
#include "proxy_response.h"
#include "proxy_transport.h"
#include "proxy_upload.h"
#include "proxy_http_upstream.h"
#include "proxy_common.h"
#include "proxy_ws_handshake.h"
#include "server_internal.h"
#include "response_internal.h"
#include "channel_internal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <limits.h>

static void step(struct proxy_exchange *);

static void worker_unref(struct rotmos_proxy_worker *w)
{
    if (--w->refs == 0)
        free(w);
}

static void interim_free(struct proxy_interim *i)
{
    rotmos_proxy_headers_cleanup(&i->headers);
    free(i);
}

void rotmos_proxy_release(void *data)
{
    struct proxy_exchange *x;
    struct proxy_interim *i;
    x = data;
    if (--x->refs != 0)
        return;
    while ((i = x->interims) != NULL) {
        x->interims = i->next;
        interim_free(i);
    }
    rotmos_proxy_ws_clear(x);
    rotmos_proxy_http_upstream_cleanup(&x->upstream);
    rotmos_proxy_upload_cleanup(&x->upload);
    rotmos_proxy_local_cleanup(&x->local);
    curl_slist_free_all(x->request_headers);
    free(x->target);
    free(x->authority);
    free(x->input);
    worker_unref(x->worker);
    free(x);
}

void rotmos_proxy_progress(void *data, size_t count)
{
    struct proxy_exchange *x;
    x = data;
    if (count != 0)
        (void)rotmos_loop_now(x->loop, &x->progress, NULL);
}

static int xfer_progress(void *data, curl_off_t total_down, curl_off_t down,
                         curl_off_t total_up, curl_off_t up)
{
    struct proxy_exchange *x;
    (void)total_down;
    (void)total_up;
    x = data;
    if (down != x->downloaded || up != x->uploaded) {
        x->downloaded = down;
        x->uploaded = up;
        rotmos_proxy_progress(x, 1);
    }
    return 0;
}

static void upload_consumed(void *data, size_t count)
{
    rotmos_proxy_progress(data, count);
    rotmos_proxy_schedule(data);
}

void rotmos_proxy_record_failure(struct proxy_exchange *x, rotmos_status cause,
                                 int status, const char *message)
{
    if (x->failure.status != ROTMOS_OK)
        return;
    if (cause <= ROTMOS_EOF || cause > ROTMOS_BACKEND)
        cause = ROTMOS_BACKEND;
    (void)rotmos_fail(&x->failure, cause, "proxy", message);
    x->local_status = status;
}

static void stop_timer(struct proxy_exchange *x)
{
    if (x->timer != NULL) {
        rotmos_timer_cancel(x->timer);
        x->timer = NULL;
    }
}

static void request_closed(rotmos_request *request,
                           const rotmos_request_outcome *out, void *data)
{
    struct proxy_exchange *x;
    struct proxy_exchange **slot;
    (void)out;
    x = data;
    request->response->progress = NULL;
    request->response->progress_data = NULL;
    x->request = NULL;
    x->channel = NULL;
    stop_timer(x);
    rotmos_proxy_ws_stop(x);
    if (x->transfer != NULL) {
        rotmos_proxy_transfer_cancel(x->transfer);
        x->transfer = NULL;
    }
    slot = &x->worker->exchanges;
    while (*slot != NULL && *slot != x)
        slot = &(*slot)->next;
    if (*slot == x)
        *slot = x->next;
    --x->worker->active;
    rotmos_proxy_release(x);
}

static void turn(rotmos_loop *loop, void *data)
{
    struct proxy_exchange *x;
    (void)loop;
    x = data;
    x->queued = 0;
    if (x->request != NULL && !x->worker->closing)
        step(x);
}

void rotmos_proxy_schedule(struct proxy_exchange *x)
{
    rotmos_status status;
    if (x->request == NULL || x->worker->closing || x->queued)
        return;
    ++x->refs;
    status = rotmos_loop_defer(x->loop, turn, rotmos_proxy_release, x, NULL);
    if (status != ROTMOS_OK) {
        --x->refs;
        (void)rotmos_response_abort(x->request->response, status, NULL);
        return;
    }
    x->queued = 1;
}

static void writable(rotmos_response *response, void *data)
{
    (void)response;
    rotmos_proxy_schedule(data);
}

static void channel_ready(rotmos_channel *channel, unsigned int flags,
                          void *data)
{
    (void)channel;
    (void)flags;
    rotmos_proxy_schedule(data);
}

static void channel_closed(rotmos_channel *channel,
                           const rotmos_channel_outcome *out, void *data)
{
    struct proxy_exchange *x;
    (void)channel;
    x = data;
    if (out->cause != ROTMOS_OK)
        rotmos_proxy_record_failure(x, out->cause, 502,
                                    "downstream channel closed");
    rotmos_proxy_schedule(x);
}

static long remaining(rotmos_u64 now, rotmos_u64 start, long duration)
{
    rotmos_u64 elapsed;
    size_t value;
    int order;
    (void)rotmos_u64_compare(now, start, &order, NULL);
    if (order < 0)
        return duration;
    elapsed.high = now.high - start.high - (now.low < start.low ? 1 : 0);
    elapsed.low = (now.low - start.low) & 0xffffffffUL;
    if (rotmos_u64_to_size(elapsed, &value, NULL) != ROTMOS_OK ||
        value >= (size_t)duration)
        return 0;
    return duration - (long)value;
}

static void check_timeout(rotmos_timer *timer, void *data);

static void arm_timer(struct proxy_exchange *x)
{
    rotmos_u64 now;
    long wait;
    long total;
    rotmos_status status;
    if (x->request == NULL || x->failed || x->worker->closing)
        return;
    (void)rotmos_loop_now(x->loop, &now, NULL);
    wait = remaining(now, x->progress, x->route->idle_timeout_ms);
    if (x->route->total_timeout_ms != 0) {
        total = remaining(now, x->started, x->route->total_timeout_ms);
        if (total < wait)
            wait = total;
    }
    if (wait == 0) {
        rotmos_proxy_record_failure(x, ROTMOS_TIMEOUT, 504,
                                    "proxy progress or total deadline expired");
        rotmos_proxy_schedule(x);
        return;
    }
    ++x->refs;
    status = rotmos_loop_timer(x->loop, wait, 0, check_timeout,
                               rotmos_proxy_release, x, &x->timer, NULL);
    if (status != ROTMOS_OK) {
        --x->refs;
        rotmos_proxy_record_failure(x, status, 500,
                                    "cannot register proxy deadline");
        rotmos_proxy_schedule(x);
    }
}

static void check_timeout(rotmos_timer *timer, void *data)
{
    struct proxy_exchange *x;
    (void)timer;
    x = data;
    x->timer = NULL;
    arm_timer(x);
}

void rotmos_proxy_upstream_ready(rotmos_proxy_http_upstream *upstream,
                                 rotmos_proxy_http_event event, void *data)
{
    struct proxy_exchange *x;
    struct proxy_interim *i;
    size_t at;
    x = data;
    rotmos_proxy_progress(x, 1);
    if (event == ROTMOS_PROXY_HTTP_INFORMATIONAL) {
        if (x->interim_count >= 16 ||
            upstream->response.headers.bytes > 131072UL - x->interim_bytes) {
            rotmos_proxy_record_failure(
                x, ROTMOS_LIMIT, 502,
                "upstream interim metadata exceeds budget");
            upstream->failed = 1;
        } else {
            i = calloc(1, sizeof(*i));
            if (i == NULL || rotmos_proxy_headers_sanitize_response(
                                 &upstream->response.headers, &i->headers) !=
                                 ROTMOS_PROXY_HEADER_OK) {
                if (i != NULL)
                    interim_free(i);
                rotmos_proxy_record_failure(
                    x, ROTMOS_NOMEM, 502,
                    "cannot retain upstream interim metadata");
                upstream->failed = 1;
            } else {
                i->status = upstream->response.status;
                for (at = 0; at < i->headers.count; ++at) {
                    i->views[at].name = rotmos_view(i->headers.fields[at].name);
                    i->views[at].value =
                        rotmos_view(i->headers.fields[at].value);
                }
                if (x->interim_tail != NULL)
                    x->interim_tail->next = i;
                else
                    x->interims = i;
                x->interim_tail = i;
                ++x->interim_count;
                x->interim_bytes += upstream->response.headers.bytes;
            }
        }
    } else if (event == ROTMOS_PROXY_HTTP_FINAL)
        x->final_ready = 1;
    rotmos_proxy_schedule(x);
}

static void upstream_done(CURL *easy, CURLcode result, void *data)
{
    struct proxy_exchange *x;
    const char *reason;
    int first;
    (void)easy;
    x = data;
    x->transfer = NULL;
    x->up_done = 1;
    reason = NULL;
    first = x->failure.status == ROTMOS_OK;
    if (rotmos_proxy_http_upstream_finish(&x->upstream, result, &reason) !=
        ROTMOS_PROXY_HEADER_OK) {
        rotmos_proxy_record_failure(
            x, result == CURLE_OPERATION_TIMEDOUT ? ROTMOS_TIMEOUT : ROTMOS_IO,
            result == CURLE_OPERATION_TIMEDOUT ? 504 : 502,
            result != CURLE_OK ? curl_easy_strerror(result) : reason);
        if (first && result != CURLE_OK) {
            x->failure.source = ROTMOS_ERROR_BACKEND;
            x->failure.native_code = (long)result;
        }
    }
    rotmos_proxy_schedule(x);
}

static rotmos_u64 wide(uint64_t value)
{
    rotmos_u64 result;
    result.high = (unsigned long)(value >> 32);
    result.low = (unsigned long)(value & 0xffffffffUL);
    return result;
}

static rotmos_status prepare_local(struct proxy_exchange *x)
{
    rotmos_response *r;
    size_t i;
    rotmos_status status;
    r = x->request->response;
    if (!x->failed) {
        x->failed = 1;
        stop_timer(x);
        rotmos_proxy_ws_stop(x);
        if (x->transfer != NULL) {
            rotmos_proxy_transfer_cancel(x->transfer);
            x->transfer = NULL;
        }
        (void)rotmos_channel_interest(x->channel, 0, NULL);
        if (rotmos_proxy_ws_tunneled(x) || rotmos_response_is_committed(r) ||
            (r->inflight && (r->head == NULL || !r->head->interim))) {
            (void)rotmos_response_abort(r, x->failure.status, NULL);
            return ROTMOS_STATE;
        }
        if (rotmos_response_replace_uncommitted(r, NULL) != ROTMOS_OK) {
            (void)rotmos_response_abort(r, x->failure.status, NULL);
            return ROTMOS_STATE;
        }
        if (x->local_status == 502 || x->local_status == 504)
            rotmos_proxy_local_error(x->route, &x->failure, x->local_status,
                                     &x->local);
        else {
            rotmos_proxy_local_init(&x->local);
            if (rotmos_proxy_local_respond(
                    &x->local, x->local_status, x->failure.message,
                    strlen(x->failure.message), NULL) != ROTMOS_OK) {
                (void)rotmos_response_abort(r, ROTMOS_NOMEM, NULL);
                return ROTMOS_NOMEM;
            }
        }
    }
    if (r->phase == ROTMOS_RESPONSE_ABORTED)
        return ROTMOS_STATE;
    if (!x->local_prepared) {
        status = rotmos_response_status(r, x->local.status, NULL);
        for (i = 0; status == ROTMOS_OK && i < x->local.headers.count; ++i)
            status = rotmos_response_header_add(
                r, x->local.headers.fields[i].name,
                x->local.headers.fields[i].value, NULL);
        if (status == ROTMOS_OK)
            status = rotmos_response_known_length(
                r, wide((uint64_t)x->local.body_length), NULL);
        if (status != ROTMOS_OK)
            return status;
        r->close_connection = 1;
        x->local_prepared = 1;
    }
    return ROTMOS_OK;
}

static rotmos_status local_step(struct proxy_exchange *x)
{
    rotmos_status status;
    rotmos_response *r;
    size_t accepted;
    r = x->request->response;
    status = prepare_local(x);
    if (status != ROTMOS_OK)
        return status;
    if (!x->local_begun) {
        status = rotmos_response_begin(r, NULL);
        if (status != ROTMOS_OK)
            return status;
        x->local_begun = 1;
    }
    if (!r->suppressed && x->local_offset != x->local.body_length) {
        status = rotmos_response_write(r, x->local.body + x->local_offset,
                                       x->local.body_length - x->local_offset,
                                       &accepted, NULL);
        if (status != ROTMOS_OK)
            return status;
        x->local_offset += accepted;
        if (x->local_offset != x->local.body_length) {
            rotmos_proxy_schedule(x);
            return ROTMOS_OK;
        }
    }
    return rotmos_response_end(r, NULL, 0, NULL);
}

static rotmos_status upload_step(struct proxy_exchange *x)
{
    rotmos_proxy_frame_result frame;
    rotmos_status status;
    size_t consumed;
    size_t count;
    unsigned int budget;
    if (!x->has_upload || x->upload.complete || x->up_done)
        return ROTMOS_OK;
    budget = 64;
    while (!rotmos_proxy_upload_full(&x->upload) && budget-- != 0) {
        if (x->input_offset == x->input_length) {
            x->input_offset = x->input_length = 0;
            status =
                rotmos_channel_read(x->channel, x->input,
                                    x->route->buffer_limit_bytes, &count, NULL);
            if (status == ROTMOS_AGAIN)
                return ROTMOS_OK;
            if (status != ROTMOS_OK)
                return status == ROTMOS_EOF ? ROTMOS_INVALID : status;
            x->input_length = count;
            rotmos_proxy_progress(x, count);
        }
        consumed = 0;
        frame = rotmos_proxy_upload_feed(&x->upload, x->input + x->input_offset,
                                         x->input_length - x->input_offset,
                                         &consumed);
        x->input_offset += consumed;
        if (frame == ROTMOS_PROXY_FRAME_INVALID)
            return x->upload.failure != ROTMOS_OK ? x->upload.failure
                                                  : ROTMOS_INVALID;
        if (frame == ROTMOS_PROXY_FRAME_COMPLETE)
            return rotmos_channel_body_complete(
                x->channel, x->input + x->input_offset,
                x->input_length - x->input_offset, NULL);
        if (frame == ROTMOS_PROXY_FRAME_PAUSED)
            return ROTMOS_OK;
    }
    if (budget == (unsigned int)-1)
        rotmos_proxy_schedule(x);
    return ROTMOS_OK;
}

static rotmos_status final_headers(struct proxy_exchange *x)
{
    rotmos_response *r;
    const rotmos_proxy_http_response *up;
    rotmos_status status;
    size_t i;
    int code;
    int suppressed;
    r = x->request->response;
    up = &x->upstream.response;
    if (!x->final_prepared) {
        status = rotmos_proxy_response_apply(
            up, &x->upstream.outbound_headers, x->route->modify_response,
            x->route->modify_response_userdata, &code, NULL);
        if (status != ROTMOS_OK)
            return status;
        status = rotmos_response_status(r, code, NULL);
        for (i = 0;
             status == ROTMOS_OK && i < x->upstream.outbound_headers.count; ++i)
            status = rotmos_response_header_add(
                r, x->upstream.outbound_headers.fields[i].name,
                x->upstream.outbound_headers.fields[i].value, NULL);
        suppressed = x->request->method == ROTMOS_HTTP_HEAD || code == 204 ||
                     code == 205 || code == 304;
        if (status == ROTMOS_OK && suppressed && up->has_content_length &&
            code != 204 && code != 205)
            status =
                rotmos_response_known_length(r, wide(up->content_length), NULL);
        if (status == ROTMOS_OK && !suppressed)
            status = rotmos_response_trailers_enable(r, NULL, 0, NULL);
        if (status != ROTMOS_OK)
            return status;
        r->close_connection = 1;
        x->final_prepared = 1;
    }
    status = rotmos_response_begin(r, NULL);
    if (status == ROTMOS_OK)
        x->begun = 1;
    return status;
}

static rotmos_status relay_step(struct proxy_exchange *x)
{
    struct proxy_interim *i;
    rotmos_response *r;
    rotmos_status status;
    unsigned char *body;
    rotmos_header *trailers;
    size_t count;
    size_t accepted;
    size_t at;
    r = x->request->response;
    while ((i = x->interims) != NULL) {
        status = rotmos_response_interim(r, i->status, i->views,
                                         i->headers.count, NULL);
        if (status != ROTMOS_OK)
            return status;
        x->interims = i->next;
        if (x->interims == NULL)
            x->interim_tail = NULL;
        interim_free(i);
    }
    if (!x->final_ready)
        return ROTMOS_OK;
    if (!x->begun) {
        status = final_headers(x);
        if (status != ROTMOS_OK)
            return status;
    }
    body = rotmos_proxy_http_upstream_body(&x->upstream, &count);
    if (count != 0) {
        accepted = count;
        if (!r->suppressed) {
            status = rotmos_response_write(r, body, count, &accepted, NULL);
            if (status != ROTMOS_OK)
                return status;
        }
        rotmos_proxy_http_upstream_consume(&x->upstream, accepted);
        if (accepted != count) {
            rotmos_proxy_schedule(x);
            return ROTMOS_OK;
        }
    }
    if (!x->up_done)
        return ROTMOS_OK;
    count = r->suppressed ? 0 : x->upstream.response.trailers.count;
    trailers = count != 0 ? malloc(count * sizeof(*trailers)) : NULL;
    if (count != 0 && trailers == NULL)
        return ROTMOS_NOMEM;
    for (at = 0; at < count; ++at) {
        trailers[at].name =
            rotmos_view(x->upstream.response.trailers.fields[at].name);
        trailers[at].value =
            rotmos_view(x->upstream.response.trailers.fields[at].value);
    }
    status = rotmos_response_end(r, trailers, count, NULL);
    free(trailers);
    if (status == ROTMOS_OK)
        x->ended = 1;
    return status;
}

static void step(struct proxy_exchange *x)
{
    rotmos_status status;
    unsigned int interest;
    size_t held;
    int mask;
    int resume;
    int interim_pending;
    if (x->failure.status != ROTMOS_OK) {
        status = local_step(x);
        if (status != ROTMOS_OK && status != ROTMOS_AGAIN)
            (void)rotmos_response_abort(x->request->response, x->failure.status,
                                        NULL);
        return;
    }
    if (x->ended)
        return;
    status = x->ws != NULL ? rotmos_proxy_ws_step(x) : upload_step(x);
    if (status != ROTMOS_OK) {
        rotmos_proxy_record_failure(
            x, status,
            x->ws != NULL ? 502 : (status == ROTMOS_LIMIT ? 413 : 400),
            x->ws != NULL ? "WebSocket upstream transfer or framing failed"
                          : "invalid, oversized or truncated proxy upload");
        rotmos_proxy_schedule(x);
        return;
    }
    if (rotmos_proxy_ws_tunneled(x))
        return;
    held = x->upstream.body_length - x->upstream.body_offset;
    interim_pending = x->interims != NULL;
    status = relay_step(x);
    if (status != ROTMOS_OK && status != ROTMOS_AGAIN) {
        rotmos_proxy_record_failure(x, status, 502,
                                    "upstream response cannot be relayed");
        rotmos_proxy_schedule(x);
        return;
    }
    if (x->ws != NULL) {
        if (held > x->upstream.body_length - x->upstream.body_offset ||
            (interim_pending && x->interims == NULL))
            rotmos_proxy_schedule(x);
        return;
    }
    if (!x->up_done && x->transfer != NULL) {
        (void)rotmos_proxy_http_upstream_body(&x->upstream, &held);
        resume = (x->upstream.paused && held == 0) ||
                 (x->has_upload && rotmos_proxy_upload_can_resume(&x->upload));
        if (resume) {
            mask = 0;
            if (x->upstream.paused && held != 0)
                mask |= CURLPAUSE_RECV;
            else
                x->upstream.paused = 0;
            if (x->has_upload && x->upload.curl_paused) {
                if (rotmos_proxy_upload_can_resume(&x->upload))
                    rotmos_proxy_upload_resumed(&x->upload);
                else
                    mask |= CURLPAUSE_SEND;
            }
            if (curl_easy_pause(x->upstream.easy, mask) != CURLE_OK) {
                rotmos_proxy_record_failure(
                    x, ROTMOS_IO, 502,
                    "cannot resume duplex upstream transfer");
                rotmos_proxy_schedule(x);
            }
        }
    }
    interest = x->has_upload && !x->upload.complete && !x->up_done &&
                       !rotmos_proxy_upload_full(&x->upload)
                   ? ROTMOS_READABLE
                   : 0;
    (void)rotmos_channel_interest(x->channel, interest, NULL);
}

static int add_header(struct proxy_exchange *x, const char *name,
                      const char *value)
{
    char *line;
    struct curl_slist *next;
    size_t count;
    count = strlen(name) + strlen(value) + 3;
    line = malloc(count);
    if (line == NULL)
        return 0;
    (void)snprintf(line, count, "%s: %s", name, value);
    next = curl_slist_append(x->request_headers, line);
    free(line);
    if (next == NULL)
        return 0;
    x->request_headers = next;
    return 1;
}

static rotmos_status simple_reply(rotmos_request *request,
                                  rotmos_request_plan *plan,
                                  rotmos_proxy_local_response *local, int code,
                                  const char *message, rotmos_error *error)
{
    rotmos_status status;
    rotmos_bytes body;
    rotmos_reply *reply;
    size_t i;
    status = rotmos_reply_new(&reply, error);
    if (status != ROTMOS_OK)
        return status;
    if (local != NULL) {
        code = local->status;
        body.data = local->body;
        body.length = local->body_length;
        for (i = 0; i < local->headers.count; ++i) {
            status =
                rotmos_reply_header_add(reply, local->headers.fields[i].name,
                                        local->headers.fields[i].value, error);
            if (status != ROTMOS_OK)
                break;
        }
    } else
        body = rotmos_view(message);
    request->response->close_connection = 1;
    if (status == ROTMOS_OK) {
        rotmos_bytes empty = {NULL, 0};
        status = rotmos_reply_bytes(reply, code, "application/octet-stream",
                                    local != NULL ? empty : body, error);
        if (status == ROTMOS_OK) {
            free(reply->content_type);
            reply->content_type = NULL;
            if (local != NULL) {
                reply->body = local->body;
                reply->body_length = local->body_length;
                local->body = NULL;
                local->body_length = 0;
            }
            status = rotmos_response_submit(request->response, reply, error);
        }
    }
    (void)rotmos_reply_destroy(reply, NULL);
    if (status == ROTMOS_OK) {
        request->response->complete_reply = 1;
        request->response->phase = ROTMOS_RESPONSE_FINISHING;
        plan->disposition = ROTMOS_REQUEST_REPLY;
    }
    return status;
}

static rotmos_status worker_get(rotmos_server *server,
                                struct rotmos_proxy_worker **out,
                                rotmos_error *error)
{
    struct rotmos_proxy_worker *w;
    rotmos_status status;
    if (server->proxy != NULL) {
        *out = server->proxy;
        return ROTMOS_OK;
    }
    w = calloc(1, sizeof(*w));
    if (w == NULL)
        return ROTMOS_NOMEM;
    status = rotmos_proxy_transport_new(
        server->loop, server->config.proxy.active_exchanges,
        server->config.proxy.idle_connections, &w->transport, error);
    if (status != ROTMOS_OK) {
        free(w);
        return status;
    }
    w->refs = 1;
    *out = server->proxy = w;
    return ROTMOS_OK;
}

rotmos_status rotmos_proxy_dispatch(rotmos_request *request,
                                    rotmos_request_plan *plan, void *data,
                                    rotmos_error *error)
{
    rotmos_proxy_route_data *route;
    struct rotmos_proxy_worker *w;
    struct proxy_exchange *x;
    rotmos_proxy_headers inbound;
    rotmos_proxy_headers sanitized;
    rotmos_proxy_request_head head;
    rotmos_proxy_inbound input;
    rotmos_proxy_outbound directed;
    rotmos_proxy_local_response local;
    rotmos_proxy_http_upload upload;
    rotmos_channel_config channel;
    rotmos_status status;
    const char *url;
    static const char *const methods[] = {
        "",     "GET",     "POST",     "PUT",   "PATCH", "DELETE",
        "HEAD", "OPTIONS", "PROPFIND", "MKCOL", "COPY",  "MOVE"};
    CURL *easy;
    struct curl_slist *next;
    size_t i;
    uint64_t body_limit;
    int force_http1;
    route = data;
    rotmos_proxy_headers_init(&inbound);
    rotmos_proxy_headers_init(&sanitized);
    rotmos_proxy_local_init(&local);
    memset(&directed, 0, sizeof(directed));
    status = ROTMOS_OK;
    for (i = 0; i < request->header_count; ++i)
        if (rotmos_proxy_headers_add(&inbound, request->headers[i].name.data,
                                     request->headers[i].value.data) !=
            ROTMOS_PROXY_HEADER_OK) {
            status = ROTMOS_INVALID;
            break;
        }
    if (status != ROTMOS_OK ||
        memcmp(request->version.data, "HTTP/1.1", 8) != 0 ||
        rotmos_proxy_validate_path(request->raw_path.data, error) !=
            ROTMOS_OK ||
        rotmos_proxy_request_head_parse(&inbound, &head, NULL) !=
            ROTMOS_PROXY_HEADER_OK) {
        status = simple_reply(request, plan, NULL, 400,
                              "invalid proxy request\n", error);
        goto finished;
    }
    body_limit =
        ((uint64_t)request->server->config.limits.hard_body_bytes.high << 32) |
        request->server->config.limits.hard_body_bytes.low;
    if (body_limit != 0u && head.has_content_length &&
        head.content_length > body_limit) {
        status =
            simple_reply(request, plan, NULL, 413,
                         "proxy request body exceeds server limit\n", error);
        goto finished;
    }
    if (head.websocket_upgrade &&
        (!rotmos_proxy_ws_request_valid(request->method, &inbound, NULL) ||
         head.chunked || head.has_content_length)) {
        status = simple_reply(request, plan, NULL, 400,
                              "invalid proxy WebSocket handshake\n", error);
        goto finished;
    }
    if (route->preflight != NULL) {
        input.method = request->method;
        input.path = request->raw_path.data;
        input.query = request->query.data;
        input.host = head.host;
        input.headers = &inbound;
        input.matched_request = request;
        input.websocket = head.websocket_upgrade;
        status =
            route->preflight(&input, &local, route->preflight_userdata, error);
        if (status != ROTMOS_OK || local.failure != ROTMOS_OK ||
            (!local.status && local.headers.count)) {
            status = simple_reply(request, plan, NULL, 500,
                                  "proxy preflight failed\n", error);
            goto finished;
        }
        if (local.status != 0) {
            status = simple_reply(request, plan, &local, 0, NULL, error);
            goto finished;
        }
    }
    status = worker_get(request->server, &w, error);
    if (status != ROTMOS_OK)
        goto finished;
    if (w->closing ||
        w->active >= request->server->config.proxy.active_exchanges) {
        status = simple_reply(request, plan, NULL, 503,
                              "proxy worker exchange limit reached\n", error);
        goto finished;
    }
    x = calloc(1, sizeof(*x));
    if (x == NULL) {
        status = ROTMOS_NOMEM;
        goto finished;
    }
    x->request = request;
    x->loop = request->server->loop;
    x->worker = w;
    x->route = route;
    x->refs = 1;
    x->next = w->exchanges;
    w->exchanges = x;
    ++w->active;
    ++w->refs;
    (void)rotmos_loop_now(x->loop, &x->started, NULL);
    x->progress = x->started;
    plan->on_close = request_closed;
    plan->userdata = x;
    rotmos_channel_config_init(&channel);
    channel.on_event = channel_ready;
    channel.on_close = channel_closed;
    channel.userdata = x;
    status = rotmos_request_takeover(request, &channel, &x->channel, error);
    if (status != ROTMOS_OK)
        goto finished;
    plan->disposition = ROTMOS_REQUEST_TAKEOVER;
    status =
        rotmos_response_callbacks(request->response, writable, NULL, x, error);
    request->response->progress = rotmos_proxy_progress;
    request->response->progress_data = x;
    if (status == ROTMOS_OK &&
        rotmos_proxy_headers_sanitize_request(&inbound, &sanitized) !=
            ROTMOS_PROXY_HEADER_OK)
        status = ROTMOS_INVALID;
    if (status == ROTMOS_OK)
        status = rotmos_proxy_director_prepare(
            route, request->method, request->raw_path.data, request->query.data,
            head.host, head.websocket_upgrade, request, &inbound, &sanitized,
            &directed, &x->target, &x->authority, error);
    if (status != ROTMOS_OK)
        goto exchange_failed;
    url = route->targets[directed.target_index];
    if (head.websocket_upgrade) {
        status =
            rotmos_proxy_ws_start(x, url, &inbound, &directed.headers, error);
        if (status != ROTMOS_OK)
            goto exchange_failed;
        arm_timer(x);
        rotmos_proxy_schedule(x);
        goto finished;
    }
    if (!add_header(x, "Host", x->authority) ||
        !add_header(x, "X-Forwarded-For", request->peer.address.data) ||
        !add_header(x, "X-Forwarded-Proto", request->tls ? "https" : "http") ||
        !add_header(x, "X-Forwarded-Host", head.host)) {
        status = ROTMOS_NOMEM;
        goto exchange_failed;
    }
    for (i = 0; i < directed.headers.count; ++i)
        if (!add_header(x, directed.headers.fields[i].name,
                        directed.headers.fields[i].value)) {
            status = ROTMOS_NOMEM;
            goto exchange_failed;
        }
    x->has_upload =
        head.chunked ||
        (head.has_content_length &&
         !(directed.method == ROTMOS_HTTP_HEAD && head.content_length == 0));
    if (directed.method == ROTMOS_HTTP_HEAD && head.has_content_length &&
        head.content_length == 0 && !add_header(x, "Content-Length", "0")) {
        status = ROTMOS_NOMEM;
        goto exchange_failed;
    }
    memset(&upload, 0, sizeof(upload));
    if (x->has_upload) {
        status = rotmos_proxy_upload_init(&x->upload, route->buffer_limit_bytes,
                                          head.chunked, head.content_length,
                                          &inbound, error);
        if (status != ROTMOS_OK)
            goto exchange_failed;
        x->upload.body_limit = body_limit;
        x->input = malloc(route->buffer_limit_bytes);
        if (x->input == NULL) {
            status = ROTMOS_NOMEM;
            goto exchange_failed;
        }
        x->upload.on_consume = upload_consumed;
        x->upload.consume_userdata = x;
        upload.read = rotmos_proxy_upload_read;
        upload.userdata = &x->upload;
        upload.known_length = !head.chunked;
        upload.content_length = head.content_length;
        if (head.has_trailers)
            upload.trailers = rotmos_proxy_upload_curl_trailers;
        next = curl_slist_append(x->request_headers, "Expect:");
        if (next == NULL) {
            status = ROTMOS_NOMEM;
            goto exchange_failed;
        }
        x->request_headers = next;
    }
    if (!x->has_upload || x->upload.complete) {
        status = rotmos_channel_body_complete(x->channel, NULL, 0, error);
        if (status != ROTMOS_OK)
            goto exchange_failed;
    }
    if (head.has_trailers)
        for (i = 0; i < inbound.count; ++i)
            if (strcasecmp(inbound.fields[i].name, "trailer") == 0 &&
                !add_header(x, "Trailer", inbound.fields[i].value)) {
                status = ROTMOS_NOMEM;
                goto exchange_failed;
            }
    easy = curl_easy_init();
    if (easy == NULL) {
        status = ROTMOS_NOMEM;
        goto exchange_failed;
    }
    status = rotmos_proxy_curl_set_ca(easy, route->tls_ca_pem,
                                      route->tls_ca_pem_length, error);
    if (status == ROTMOS_OK)
        status = rotmos_proxy_curl_set_client_identity(
            easy, route->tls_client_cert_pem, route->tls_client_cert_pem_length,
            route->tls_client_key_pem, route->tls_client_key_pem_length, error);
    if (status == ROTMOS_OK)
        status = rotmos_proxy_http_upstream_init(
            &x->upstream, easy, url, x->target, methods[directed.method],
            x->request_headers, route->buffer_limit_bytes,
            route->connect_timeout_ms, route->total_timeout_ms,
            x->has_upload ? &upload : NULL, rotmos_proxy_upstream_ready, x,
            error);
    if (status == ROTMOS_OK &&
        (curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, xfer_progress) !=
             CURLE_OK ||
         curl_easy_setopt(easy, CURLOPT_XFERINFODATA, x) != CURLE_OK ||
         curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L) != CURLE_OK))
        status = ROTMOS_BACKEND;
    force_http1 = route->upstream_http_version == ROTMOS_PROXY_HTTP_1_1 ||
                  head.has_trailers;
    if (status == ROTMOS_OK)
        status = rotmos_proxy_transport_submit(w->transport, easy, force_http1,
                                               0, upstream_done, x,
                                               &x->transfer, error);
    if (status != ROTMOS_OK) {
        curl_easy_cleanup(easy);
        goto exchange_failed;
    }
    if (head.expect_continue && x->channel->receive_length == 0) {
        status =
            rotmos_response_interim(request->response, 100, NULL, 0, error);
        if (status != ROTMOS_OK)
            goto exchange_failed;
    }
    arm_timer(x);
    rotmos_proxy_schedule(x);
    status = ROTMOS_OK;
    goto finished;
exchange_failed:
    rotmos_proxy_record_failure(
        x, status,
        status == ROTMOS_INVALID ? 400 : (status == ROTMOS_NOMEM ? 500 : 502),
        "proxy setup or rewrite failed");
    rotmos_proxy_schedule(x);
    status = ROTMOS_OK;
finished:
    rotmos_proxy_director_cleanup(&directed);
    rotmos_proxy_headers_cleanup(&inbound);
    rotmos_proxy_headers_cleanup(&sanitized);
    rotmos_proxy_local_cleanup(&local);
    return status;
}

void rotmos_proxy_worker_clear(rotmos_server *server)
{
    struct rotmos_proxy_worker *w;
    struct proxy_exchange *x;
    w = server->proxy;
    if (w == NULL)
        return;
    server->proxy = NULL;
    w->closing = 1;
    for (x = w->exchanges; x != NULL; x = x->next) {
        stop_timer(x);
        rotmos_proxy_ws_stop(x);
        if (x->transfer != NULL) {
            rotmos_proxy_transfer_cancel(x->transfer);
            x->transfer = NULL;
        }
    }
    (void)rotmos_proxy_transport_destroy(w->transport, NULL);
    w->transport = NULL;
    worker_unref(w);
}
