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
#include <unistd.h>

static void step(struct proxy_exchange *);

static void worker_unref(struct knurl_proxy_worker *w)
{
    if (--w->refs == 0)
        free(w);
}

static void interim_free(struct proxy_interim *i)
{
    knurl_proxy_headers_cleanup(&i->headers);
    free(i);
}

void knurl_proxy_release(void *data)
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
    knurl_proxy_ws_clear(x);
    knurl_proxy_http_upstream_cleanup(&x->upstream);
    knurl_proxy_upload_cleanup(&x->upload);
    knurl_proxy_local_cleanup(&x->local);
    curl_slist_free_all(x->request_headers);
    free(x->target);
    free(x->authority);
    free(x->input);
    worker_unref(x->worker);
    free(x);
}

void knurl_proxy_progress(void *data, size_t count)
{
    struct proxy_exchange *x;
    x = data;
    if (count != 0)
        (void)knurl_loop_now(x->loop, &x->progress, NULL);
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
        knurl_proxy_progress(x, 1);
    }
    return 0;
}

static void upload_consumed(void *data, size_t count)
{
    struct proxy_exchange *x = data;
    knurl_proxy_count(&x->accounting.upstream_bytes, count);
    knurl_proxy_progress(data, count);
    knurl_proxy_schedule(data);
}
void knurl_proxy_count(knurl_u64 *total, size_t count)
{
    knurl_u64 amount;
    (void)knurl_u64_from_size(count, &amount, NULL);
    (void)knurl_u64_add(*total, amount, total, NULL);
}
static void accounting_start(struct proxy_exchange *x)
{
    static unsigned long next_id;
    next_id = (next_id + 1) & 0xffffffffUL;
    if (!next_id)
        ++next_id;
    x->accounting.request_id.high = (unsigned long)getpid() & 0xffffffffUL;
    x->accounting.request_id.low = next_id;
    x->accounting.route = x->route->path;
    x->accounting_started = x->started;
    (void)knurl_loop_now(x->loop, &x->accounting_started, NULL);
    x->accounting_active = 1;
}

void knurl_proxy_record_failure(struct proxy_exchange *x, knurl_status cause,
                                int status, const char *message)
{
    if (x->failure.status != KNURL_OK)
        return;
    if (cause <= KNURL_EOF || cause > KNURL_BACKEND)
        cause = KNURL_BACKEND;
    (void)knurl_fail(&x->failure, cause, "proxy", message);
    x->local_status = status;
    if (x->accounting.error_category == NULL) {
        x->accounting.disconnect_side = status == 500 ? "worker" : "upstream";
        x->accounting.error_category = cause == KNURL_TIMEOUT ? "timeout"
                                       : status == 500        ? "internal"
                                       : knurl_proxy_ws_tunneled(x)
                                           ? "transport"
                                           : "upstream";
    }
    if (x->request != NULL &&
        !strcmp(x->accounting.disconnect_side, "upstream"))
        x->request->impl->disconnect_side = KNURL_DISCONNECT_UPSTREAM;
}

static void stop_timer(struct proxy_exchange *x)
{
    if (x->timer != NULL) {
        knurl_timer_cancel(x->timer);
        x->timer = NULL;
    }
}

static void request_closed(knurl_request *request,
                           const knurl_request_outcome *out, void *data)
{
    struct proxy_exchange *x;
    struct proxy_exchange **slot;
    x = data;
    if (x->accounting_active) {
        knurl_u64 now = x->accounting_started;
        (void)knurl_loop_now(x->loop, &now, NULL);
        if (now.high > x->accounting_started.high ||
            (now.high == x->accounting_started.high &&
             now.low >= x->accounting_started.low)) {
            x->accounting.duration_ms.low =
                (now.low - x->accounting_started.low) & 0xffffffffUL;
            x->accounting.duration_ms.high =
                (now.high - x->accounting_started.high -
                 (now.low < x->accounting_started.low)) &
                0xffffffffUL;
        }
        x->accounting.status =
            out->response.status ? out->response.status : x->local_status;
        x->accounting.cause =
            x->failure.status != KNURL_OK ? x->failure.status : out->cause;
        if (request->impl->server->impl->state == KNURL_SERVER_STOPPING &&
            out->cause != KNURL_OK) {
            x->accounting.disconnect_side = "worker";
            x->accounting.error_category = "shutdown";
            request->impl->disconnect_side = KNURL_DISCONNECT_HOST;
        } else if (x->accounting.error_category == NULL &&
                   out->cause != KNURL_OK) {
            x->accounting.disconnect_side = "downstream";
            x->accounting.error_category = "disconnect";
        }
        if (x->accounting.error_category == NULL) {
            x->accounting.disconnect_side = "none";
            x->accounting.error_category = "none";
        }
        if (!strcmp(x->accounting.disconnect_side, "upstream"))
            request->impl->disconnect_side = KNURL_DISCONNECT_UPSTREAM;
        else if (!strcmp(x->accounting.disconnect_side, "downstream"))
            request->impl->disconnect_side = KNURL_DISCONNECT_DOWNSTREAM;
        if (x->route->on_exchange != NULL)
            x->route->on_exchange(request, &x->accounting,
                                  x->route->on_exchange_userdata);
    }
    request->impl->response->impl->progress = NULL;
    request->impl->response->impl->progress_data = NULL;
    x->request = NULL;
    x->channel = NULL;
    stop_timer(x);
    knurl_proxy_ws_stop(x);
    if (x->transfer != NULL) {
        knurl_proxy_transfer_cancel(x->transfer);
        x->transfer = NULL;
    }
    slot = &x->worker->exchanges;
    while (*slot != NULL && *slot != x)
        slot = &(*slot)->next;
    if (*slot == x)
        *slot = x->next;
    --x->worker->active;
    knurl_proxy_release(x);
}

static void turn(knurl_loop *loop, void *data)
{
    struct proxy_exchange *x;
    (void)loop;
    x = data;
    x->queued = 0;
    if (x->request != NULL && !x->worker->closing)
        step(x);
}

void knurl_proxy_schedule(struct proxy_exchange *x)
{
    knurl_status status;
    if (x->request == NULL || x->worker->closing || x->queued)
        return;
    ++x->refs;
    status = knurl_loop_defer(x->loop, turn, knurl_proxy_release, x, NULL);
    if (status != KNURL_OK) {
        --x->refs;
        (void)knurl_response_abort(x->request->impl->response, status, NULL);
        return;
    }
    x->queued = 1;
}

static void writable(knurl_response *response, void *data)
{
    (void)response;
    knurl_proxy_schedule(data);
}

static void channel_ready(knurl_channel *channel, unsigned int flags,
                          void *data)
{
    (void)channel;
    (void)flags;
    knurl_proxy_schedule(data);
}

static void channel_closed(knurl_channel *channel,
                           const knurl_channel_outcome *out, void *data)
{
    struct proxy_exchange *x;
    (void)channel;
    x = data;
    if (out->cause != KNURL_OK && x->failure.status == KNURL_OK) {
        x->accounting.disconnect_side = "downstream";
        x->accounting.error_category = "disconnect";
        knurl_proxy_record_failure(x, out->cause, 502,
                                   "downstream channel closed");
    }
    knurl_proxy_schedule(x);
}

static long remaining(knurl_u64 now, knurl_u64 start, long duration)
{
    knurl_u64 elapsed;
    size_t value;
    int order;
    (void)knurl_u64_compare(now, start, &order, NULL);
    if (order < 0)
        return duration;
    elapsed.high = now.high - start.high - (now.low < start.low ? 1 : 0);
    elapsed.low = (now.low - start.low) & 0xffffffffUL;
    if (knurl_u64_to_size(elapsed, &value, NULL) != KNURL_OK ||
        value >= (size_t)duration)
        return 0;
    return duration - (long)value;
}

static void check_timeout(knurl_timer *timer, void *data);

static void arm_timer(struct proxy_exchange *x)
{
    knurl_u64 now;
    long wait;
    long total;
    knurl_status status;
    if (x->request == NULL || x->failed || x->worker->closing)
        return;
    (void)knurl_loop_now(x->loop, &now, NULL);
    wait = remaining(now, x->progress, x->route->idle_timeout_ms);
    if (x->route->total_timeout_ms != 0) {
        total = remaining(now, x->started, x->route->total_timeout_ms);
        if (total < wait)
            wait = total;
    }
    if (wait == 0) {
        knurl_proxy_record_failure(x, KNURL_TIMEOUT, 504,
                                   "proxy progress or total deadline expired");
        knurl_proxy_schedule(x);
        return;
    }
    ++x->refs;
    status = knurl_loop_timer(x->loop, wait, 0, check_timeout,
                              knurl_proxy_release, x, &x->timer, NULL);
    if (status != KNURL_OK) {
        --x->refs;
        knurl_proxy_record_failure(x, status, 500,
                                   "cannot register proxy deadline");
        knurl_proxy_schedule(x);
    }
}

static void check_timeout(knurl_timer *timer, void *data)
{
    struct proxy_exchange *x;
    (void)timer;
    x = data;
    x->timer = NULL;
    arm_timer(x);
}

void knurl_proxy_upstream_ready(knurl_proxy_http_upstream *upstream,
                                knurl_proxy_http_event event, void *data)
{
    struct proxy_exchange *x;
    struct proxy_interim *i;
    size_t at;
    x = data;
    knurl_proxy_progress(x, 1);
    if (event == KNURL_PROXY_HTTP_INFORMATIONAL) {
        if (x->interim_count >= 16 ||
            upstream->response.headers.bytes > 131072UL - x->interim_bytes) {
            knurl_proxy_record_failure(
                x, KNURL_LIMIT, 502,
                "upstream interim metadata exceeds budget");
            upstream->failed = 1;
        } else {
            i = calloc(1, sizeof(*i));
            if (i == NULL || knurl_proxy_headers_sanitize_response(
                                 &upstream->response.headers, &i->headers) !=
                                 KNURL_PROXY_HEADER_OK) {
                if (i != NULL)
                    interim_free(i);
                knurl_proxy_record_failure(
                    x, KNURL_NOMEM, 502,
                    "cannot retain upstream interim metadata");
                upstream->failed = 1;
            } else {
                i->status = upstream->response.status;
                for (at = 0; at < i->headers.count; ++at) {
                    i->views[at].name = knurl_view(i->headers.fields[at].name);
                    i->views[at].value =
                        knurl_view(i->headers.fields[at].value);
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
    } else if (event == KNURL_PROXY_HTTP_FINAL)
        x->final_ready = 1;
    knurl_proxy_schedule(x);
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
    first = x->failure.status == KNURL_OK;
    if (knurl_proxy_http_upstream_finish(&x->upstream, result, &reason) !=
        KNURL_PROXY_HEADER_OK) {
        knurl_proxy_record_failure(
            x, result == CURLE_OPERATION_TIMEDOUT ? KNURL_TIMEOUT : KNURL_IO,
            result == CURLE_OPERATION_TIMEDOUT ? 504 : 502,
            result != CURLE_OK
                ? (x->upstream_error[0] ? x->upstream_error
                                        : curl_easy_strerror(result))
                : reason);
        if (first && result != CURLE_OK) {
            x->failure.source = KNURL_ERROR_BACKEND;
            x->failure.native_code = (long)result;
        }
    }
    knurl_proxy_schedule(x);
}

static knurl_u64 wide(uint64_t value)
{
    knurl_u64 result;
    result.high = (unsigned long)(value >> 32);
    result.low = (unsigned long)(value & 0xffffffffUL);
    return result;
}

static knurl_status prepare_local(struct proxy_exchange *x)
{
    knurl_response *r;
    size_t i;
    knurl_status status;
    r = x->request->impl->response;
    if (!x->failed) {
        x->failed = 1;
        stop_timer(x);
        knurl_proxy_ws_stop(x);
        if (x->transfer != NULL) {
            knurl_proxy_transfer_cancel(x->transfer);
            x->transfer = NULL;
        }
        (void)knurl_channel_interest(x->channel, 0, NULL);
        if (knurl_proxy_ws_tunneled(x) || knurl_response_is_committed(r) ||
            (r->impl->inflight &&
             (r->impl->head == NULL || !r->impl->head->interim))) {
            (void)knurl_response_abort(r, x->failure.status, NULL);
            return KNURL_STATE;
        }
        if (knurl_response_replace_uncommitted(r, NULL) != KNURL_OK) {
            (void)knurl_response_abort(r, x->failure.status, NULL);
            return KNURL_STATE;
        }
        if (x->local_status == 502 || x->local_status == 504)
            knurl_proxy_local_error(x->route, &x->failure, x->local_status,
                                    &x->local);
        else {
            knurl_proxy_local_init(&x->local);
            if (knurl_proxy_local_respond(
                    &x->local, x->local_status, x->failure.message,
                    strlen(x->failure.message), NULL) != KNURL_OK) {
                (void)knurl_response_abort(r, KNURL_NOMEM, NULL);
                return KNURL_NOMEM;
            }
        }
    }
    if (r->impl->phase == KNURL_RESPONSE_ABORTED)
        return KNURL_STATE;
    if (!x->local_prepared) {
        status = knurl_response_status(r, x->local.status, NULL);
        for (i = 0; status == KNURL_OK && i < x->local.headers.count; ++i)
            status = knurl_response_header_add(
                r, x->local.headers.fields[i].name,
                x->local.headers.fields[i].value, NULL);
        if (status == KNURL_OK)
            status = knurl_response_known_length(
                r, wide((uint64_t)x->local.body_length), NULL);
        if (status != KNURL_OK)
            return status;
        r->impl->close_connection = 1;
        x->local_prepared = 1;
    }
    return KNURL_OK;
}

static knurl_status local_step(struct proxy_exchange *x)
{
    knurl_status status;
    knurl_response *r;
    size_t accepted;
    r = x->request->impl->response;
    status = prepare_local(x);
    if (status != KNURL_OK)
        return status;
    if (!x->local_begun) {
        status = knurl_response_begin(r, NULL);
        if (status != KNURL_OK)
            return status;
        x->local_begun = 1;
    }
    if (!r->impl->suppressed && x->local_offset != x->local.body_length) {
        status = knurl_response_write(r, x->local.body + x->local_offset,
                                      x->local.body_length - x->local_offset,
                                      &accepted, NULL);
        if (status != KNURL_OK)
            return status;
        x->local_offset += accepted;
        if (x->local_offset != x->local.body_length) {
            knurl_proxy_schedule(x);
            return KNURL_OK;
        }
    }
    return knurl_response_end(r, NULL, 0, NULL);
}

static knurl_status upload_step(struct proxy_exchange *x)
{
    knurl_http_frame_result frame;
    knurl_status status;
    size_t consumed;
    size_t count;
    unsigned int budget;
    if (!x->has_upload || x->upload.complete || x->up_done)
        return KNURL_OK;
    budget = 64;
    while (!knurl_proxy_upload_full(&x->upload) && budget-- != 0) {
        if (x->input_offset == x->input_length) {
            x->input_offset = x->input_length = 0;
            status =
                knurl_channel_read(x->channel, x->input,
                                   x->route->buffer_limit_bytes, &count, NULL);
            if (status == KNURL_AGAIN)
                return KNURL_OK;
            if (status != KNURL_OK)
                return status == KNURL_EOF ? KNURL_INVALID : status;
            x->input_length = count;
            knurl_proxy_progress(x, count);
        }
        consumed = 0;
        frame = knurl_proxy_upload_feed(&x->upload, x->input + x->input_offset,
                                        x->input_length - x->input_offset,
                                        &consumed);
        x->input_offset += consumed;
        if (frame == KNURL_HTTP_FRAME_INVALID)
            return x->upload.failure != KNURL_OK ? x->upload.failure
                                                 : KNURL_INVALID;
        if (frame == KNURL_HTTP_FRAME_COMPLETE)
            return knurl_channel_body_complete(
                x->channel, x->input + x->input_offset,
                x->input_length - x->input_offset, NULL);
        if (frame == KNURL_HTTP_FRAME_PAUSED)
            return KNURL_OK;
    }
    if (budget == (unsigned int)-1)
        knurl_proxy_schedule(x);
    return KNURL_OK;
}

static knurl_status final_headers(struct proxy_exchange *x)
{
    knurl_response *r;
    const knurl_proxy_http_response *up;
    knurl_status status;
    size_t i;
    int code;
    int suppressed;
    r = x->request->impl->response;
    up = &x->upstream.response;
    if (!x->final_prepared) {
        status = knurl_proxy_response_apply(
            up, &x->upstream.outbound_headers, x->route->modify_response,
            x->route->modify_response_userdata, &code, NULL);
        if (status != KNURL_OK)
            return status;
        status = knurl_response_status(r, code, NULL);
        for (i = 0;
             status == KNURL_OK && i < x->upstream.outbound_headers.count; ++i)
            status = knurl_response_header_add(
                r, x->upstream.outbound_headers.fields[i].name,
                x->upstream.outbound_headers.fields[i].value, NULL);
        suppressed = x->request->impl->method == KNURL_HTTP_HEAD ||
                     code == 204 || code == 205 || code == 304;
        if (status == KNURL_OK && suppressed && up->has_content_length &&
            code != 204 && code != 205)
            status =
                knurl_response_known_length(r, wide(up->content_length), NULL);
        if (status == KNURL_OK && !suppressed)
            status = knurl_response_trailers_enable(r, NULL, 0, NULL);
        if (status != KNURL_OK)
            return status;
        r->impl->close_connection = 1;
        x->final_prepared = 1;
    }
    status = knurl_response_begin(r, NULL);
    if (status == KNURL_OK)
        x->begun = 1;
    return status;
}

static knurl_status relay_step(struct proxy_exchange *x)
{
    struct proxy_interim *i;
    knurl_response *r;
    knurl_status status;
    unsigned char *body;
    knurl_header *trailers;
    size_t count;
    size_t accepted;
    size_t at;
    r = x->request->impl->response;
    while ((i = x->interims) != NULL) {
        status = knurl_response_interim(r, i->status, i->views,
                                        i->headers.count, NULL);
        if (status != KNURL_OK)
            return status;
        x->interims = i->next;
        if (x->interims == NULL)
            x->interim_tail = NULL;
        interim_free(i);
    }
    if (!x->final_ready)
        return KNURL_OK;
    if (!x->begun) {
        status = final_headers(x);
        if (status != KNURL_OK)
            return status;
    }
    body = knurl_proxy_http_upstream_body(&x->upstream, &count);
    if (count != 0) {
        accepted = count;
        if (!r->impl->suppressed) {
            status = knurl_response_write(r, body, count, &accepted, NULL);
            if (status != KNURL_OK)
                return status;
            knurl_proxy_count(&x->accounting.downstream_bytes, accepted);
        }
        knurl_proxy_http_upstream_consume(&x->upstream, accepted);
        if (accepted != count) {
            knurl_proxy_schedule(x);
            return KNURL_OK;
        }
    }
    if (!x->up_done)
        return KNURL_OK;
    count = r->impl->suppressed ? 0 : x->upstream.response.trailers.count;
    trailers = count != 0 ? malloc(count * sizeof(*trailers)) : NULL;
    if (count != 0 && trailers == NULL)
        return KNURL_NOMEM;
    for (at = 0; at < count; ++at) {
        trailers[at].name =
            knurl_view(x->upstream.response.trailers.fields[at].name);
        trailers[at].value =
            knurl_view(x->upstream.response.trailers.fields[at].value);
    }
    status = knurl_response_end(r, trailers, count, NULL);
    free(trailers);
    if (status == KNURL_OK)
        x->ended = 1;
    return status;
}

static void step(struct proxy_exchange *x)
{
    knurl_status status;
    unsigned int interest;
    size_t held;
    int mask;
    int resume;
    int interim_pending;
    if (x->failure.status != KNURL_OK) {
        status = local_step(x);
        if (status != KNURL_OK && status != KNURL_AGAIN)
            (void)knurl_response_abort(x->request->impl->response,
                                       x->failure.status, NULL);
        return;
    }
    if (x->ended)
        return;
    status = x->ws != NULL ? knurl_proxy_ws_step(x) : upload_step(x);
    if (status != KNURL_OK) {
        knurl_proxy_record_failure(
            x, status,
            x->ws != NULL ? 502 : (status == KNURL_LIMIT ? 413 : 400),
            x->ws != NULL ? "WebSocket upstream transfer or framing failed"
                          : "invalid, oversized or truncated proxy upload");
        knurl_proxy_schedule(x);
        return;
    }
    if (knurl_proxy_ws_tunneled(x))
        return;
    held = x->upstream.body_length - x->upstream.body_offset;
    interim_pending = x->interims != NULL;
    status = relay_step(x);
    if (status != KNURL_OK && status != KNURL_AGAIN) {
        knurl_proxy_record_failure(x, status, 502,
                                   "upstream response cannot be relayed");
        knurl_proxy_schedule(x);
        return;
    }
    if (x->ws != NULL) {
        if (held > x->upstream.body_length - x->upstream.body_offset ||
            (interim_pending && x->interims == NULL))
            knurl_proxy_schedule(x);
        return;
    }
    if (!x->up_done && x->transfer != NULL) {
        (void)knurl_proxy_http_upstream_body(&x->upstream, &held);
        resume = (x->upstream.paused && held == 0) ||
                 (x->has_upload && knurl_proxy_upload_can_resume(&x->upload));
        if (resume) {
            mask = 0;
            if (x->upstream.paused && held != 0)
                mask |= CURLPAUSE_RECV;
            else
                x->upstream.paused = 0;
            if (x->has_upload && x->upload.curl_paused) {
                if (knurl_proxy_upload_can_resume(&x->upload))
                    knurl_proxy_upload_resumed(&x->upload);
                else
                    mask |= CURLPAUSE_SEND;
            }
            if (curl_easy_pause(x->upstream.easy, mask) != CURLE_OK) {
                knurl_proxy_record_failure(
                    x, KNURL_IO, 502, "cannot resume duplex upstream transfer");
                knurl_proxy_schedule(x);
            }
        }
    }
    interest = x->has_upload && !x->upload.complete && !x->up_done &&
                       !knurl_proxy_upload_full(&x->upload)
                   ? KNURL_READABLE
                   : 0;
    (void)knurl_channel_interest(x->channel, interest, NULL);
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

static knurl_status simple_reply(knurl_request *request,
                                 knurl_request_plan *plan,
                                 knurl_proxy_local_response *local, int code,
                                 const char *message, knurl_error *error)
{
    knurl_status status;
    knurl_bytes body;
    knurl_reply *reply;
    size_t i;
    status = knurl_reply_new(&reply, error);
    if (status != KNURL_OK)
        return status;
    if (local != NULL) {
        code = local->status;
        body.data = local->body;
        body.length = local->body_length;
        for (i = 0; i < local->headers.count; ++i) {
            status =
                knurl_reply_header_add(reply, local->headers.fields[i].name,
                                       local->headers.fields[i].value, error);
            if (status != KNURL_OK)
                break;
        }
    } else
        body = knurl_view(message);
    request->impl->response->impl->close_connection = 1;
    if (status == KNURL_OK) {
        knurl_bytes empty = {NULL, 0};
        status = knurl_reply_bytes(reply, code, "application/octet-stream",
                                   local != NULL ? empty : body, error);
        if (status == KNURL_OK) {
            free(reply->impl->content_type);
            reply->impl->content_type = NULL;
            if (local != NULL) {
                reply->impl->body = local->body;
                reply->impl->body_length = local->body_length;
                local->body = NULL;
                local->body_length = 0;
            }
            status =
                knurl_response_submit(request->impl->response, reply, error);
        }
    }
    (void)knurl_reply_destroy(reply, NULL);
    if (status == KNURL_OK) {
        request->impl->response->impl->complete_reply = 1;
        request->impl->response->impl->phase = KNURL_RESPONSE_FINISHING;
        plan->disposition = KNURL_REQUEST_REPLY;
    }
    return status;
}

static knurl_status worker_get(knurl_server *server,
                               struct knurl_proxy_worker **out,
                               knurl_error *error)
{
    struct knurl_proxy_worker *w;
    knurl_status status;
    if (server->impl->proxy != NULL) {
        *out = server->impl->proxy;
        return KNURL_OK;
    }
    w = calloc(1, sizeof(*w));
    if (w == NULL)
        return KNURL_NOMEM;
    status = knurl_proxy_transport_new(
        server->impl->loop, server->impl->config.proxy.active_exchanges,
        server->impl->config.proxy.idle_connections, &w->transport, error);
    if (status != KNURL_OK) {
        free(w);
        return status;
    }
    w->refs = 1;
    *out = server->impl->proxy = w;
    return KNURL_OK;
}

knurl_status knurl_proxy_dispatch(knurl_request *request,
                                  knurl_request_plan *plan, void *data,
                                  knurl_error *error)
{
    knurl_proxy_route_data *route;
    struct knurl_proxy_worker *w;
    struct proxy_exchange *x;
    knurl_proxy_headers inbound;
    knurl_proxy_headers sanitized;
    knurl_proxy_request_head head;
    knurl_proxy_inbound input;
    knurl_proxy_outbound directed;
    knurl_proxy_local_response local;
    knurl_proxy_http_upload upload;
    knurl_channel_config channel;
    knurl_status status;
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
    knurl_proxy_headers_init(&inbound);
    knurl_proxy_headers_init(&sanitized);
    knurl_proxy_local_init(&local);
    memset(&directed, 0, sizeof(directed));
    status = KNURL_OK;
    for (i = 0; i < request->impl->header_count; ++i)
        if (knurl_proxy_headers_add(&inbound,
                                    request->impl->headers[i].name.data,
                                    request->impl->headers[i].value.data) !=
            KNURL_PROXY_HEADER_OK) {
            status = KNURL_INVALID;
            break;
        }
    if (status != KNURL_OK ||
        memcmp(request->impl->version.data, "HTTP/1.1", 8) != 0 ||
        knurl_proxy_validate_path(request->impl->raw_path.data, error) !=
            KNURL_OK ||
        knurl_proxy_request_head_parse(&inbound, &head, NULL) !=
            KNURL_PROXY_HEADER_OK) {
        status = simple_reply(request, plan, NULL, 400,
                              "invalid proxy request\n", error);
        goto finished;
    }
    body_limit =
        ((uint64_t)
             request->impl->server->impl->config.limits.hard_body_bytes.high
         << 32) |
        request->impl->server->impl->config.limits.hard_body_bytes.low;
    if (body_limit != 0u && head.has_content_length &&
        head.content_length > body_limit) {
        status =
            simple_reply(request, plan, NULL, 413,
                         "proxy request body exceeds server limit\n", error);
        goto finished;
    }
    if (head.websocket_upgrade &&
        (!knurl_proxy_ws_request_valid(request->impl->method, &inbound, NULL) ||
         head.chunked || head.has_content_length)) {
        status = simple_reply(request, plan, NULL, 400,
                              "invalid proxy WebSocket handshake\n", error);
        goto finished;
    }
    if (route->preflight != NULL) {
        input.method = request->impl->method;
        input.path = request->impl->raw_path.data;
        input.query = request->impl->query.data;
        input.host = head.host;
        input.headers = &inbound;
        input.matched_request = request;
        input.websocket = head.websocket_upgrade;
        status =
            route->preflight(&input, &local, route->preflight_userdata, error);
        if (status != KNURL_OK || local.failure != KNURL_OK ||
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
    status = worker_get(request->impl->server, &w, error);
    if (status != KNURL_OK)
        goto finished;
    if (w->closing ||
        w->active >=
            request->impl->server->impl->config.proxy.active_exchanges) {
        status = simple_reply(request, plan, NULL, 503,
                              "proxy worker exchange limit reached\n", error);
        goto finished;
    }
    x = calloc(1, sizeof(*x));
    if (x == NULL) {
        status = KNURL_NOMEM;
        goto finished;
    }
    x->request = request;
    x->loop = request->impl->server->impl->loop;
    x->worker = w;
    x->route = route;
    x->refs = 1;
    x->next = w->exchanges;
    w->exchanges = x;
    ++w->active;
    ++w->refs;
    (void)knurl_loop_now(x->loop, &x->started, NULL);
    x->progress = x->started;
    x->accounting.kind = head.websocket_upgrade ? "websocket" : "http";
    plan->on_close = request_closed;
    plan->userdata = x;
    knurl_channel_config_init(&channel);
    channel.on_event = channel_ready;
    channel.on_close = channel_closed;
    channel.userdata = x;
    status = knurl_request_takeover(request, &channel, &x->channel, error);
    if (status != KNURL_OK)
        goto finished;
    plan->disposition = KNURL_REQUEST_TAKEOVER;
    status = knurl_response_callbacks(request->impl->response, writable, NULL,
                                      x, error);
    request->impl->response->impl->progress = knurl_proxy_progress;
    request->impl->response->impl->progress_data = x;
    if (status == KNURL_OK &&
        knurl_proxy_headers_sanitize_request(&inbound, &sanitized) !=
            KNURL_PROXY_HEADER_OK)
        status = KNURL_INVALID;
    if (status == KNURL_OK)
        status = knurl_proxy_director_prepare(
            route, request->impl->method, request->impl->raw_path.data,
            request->impl->query.data, head.host, head.websocket_upgrade,
            request, &inbound, &sanitized, &directed, &x->target, &x->authority,
            error);
    if (status != KNURL_OK)
        goto exchange_failed;
    url = route->targets[directed.target_index];
    x->accounting.upstream = url;
    if (head.websocket_upgrade) {
        status =
            knurl_proxy_ws_start(x, url, &inbound, &directed.headers, error);
        if (status != KNURL_OK)
            goto exchange_failed;
        accounting_start(x);
        arm_timer(x);
        knurl_proxy_schedule(x);
        goto finished;
    }
    if (!add_header(x, "Host", x->authority) ||
        !add_header(x, "X-Forwarded-For", request->impl->peer.address.data) ||
        !add_header(x, "X-Forwarded-Proto",
                    request->impl->tls ? "https" : "http") ||
        !add_header(x, "X-Forwarded-Host", head.host)) {
        status = KNURL_NOMEM;
        goto exchange_failed;
    }
    for (i = 0; i < directed.headers.count; ++i)
        if (!add_header(x, directed.headers.fields[i].name,
                        directed.headers.fields[i].value)) {
            status = KNURL_NOMEM;
            goto exchange_failed;
        }
    x->has_upload =
        head.chunked ||
        (head.has_content_length &&
         !(directed.method == KNURL_HTTP_HEAD && head.content_length == 0));
    if (directed.method == KNURL_HTTP_HEAD && head.has_content_length &&
        head.content_length == 0 && !add_header(x, "Content-Length", "0")) {
        status = KNURL_NOMEM;
        goto exchange_failed;
    }
    memset(&upload, 0, sizeof(upload));
    if (x->has_upload) {
        status = knurl_proxy_upload_init(&x->upload, route->buffer_limit_bytes,
                                         head.chunked, head.content_length,
                                         &inbound, error);
        if (status != KNURL_OK)
            goto exchange_failed;
        x->upload.body_limit = body_limit;
        x->input = malloc(route->buffer_limit_bytes);
        if (x->input == NULL) {
            status = KNURL_NOMEM;
            goto exchange_failed;
        }
        x->upload.on_consume = upload_consumed;
        x->upload.consume_userdata = x;
        upload.read = knurl_proxy_upload_read;
        upload.userdata = &x->upload;
        upload.known_length = !head.chunked;
        upload.content_length = head.content_length;
        if (head.has_trailers)
            upload.trailers = knurl_proxy_upload_curl_trailers;
        next = curl_slist_append(x->request_headers, "Expect:");
        if (next == NULL) {
            status = KNURL_NOMEM;
            goto exchange_failed;
        }
        x->request_headers = next;
    }
    if (!x->has_upload || x->upload.complete) {
        status = knurl_channel_body_complete(x->channel, NULL, 0, error);
        if (status != KNURL_OK)
            goto exchange_failed;
    }
    if (head.has_trailers)
        for (i = 0; i < inbound.count; ++i)
            if (strcasecmp(inbound.fields[i].name, "trailer") == 0 &&
                !add_header(x, "Trailer", inbound.fields[i].value)) {
                status = KNURL_NOMEM;
                goto exchange_failed;
            }
    easy = curl_easy_init();
    if (easy == NULL) {
        status = KNURL_NOMEM;
        goto exchange_failed;
    }
    status = curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, x->upstream_error) ==
                     CURLE_OK
                 ? KNURL_OK
                 : KNURL_BACKEND;
    if (status == KNURL_OK)
        status = knurl_proxy_curl_set_ca(easy, route->tls_ca_pem,
                                         route->tls_ca_pem_length, error);
    if (status == KNURL_OK)
        status = knurl_proxy_curl_set_client_identity(
            easy, route->tls_client_cert_pem, route->tls_client_cert_pem_length,
            route->tls_client_key_pem, route->tls_client_key_pem_length, error);
    if (status == KNURL_OK)
        status = knurl_proxy_http_upstream_init(
            &x->upstream, easy, url, x->target, methods[directed.method],
            x->request_headers, route->buffer_limit_bytes,
            route->connect_timeout_ms, route->total_timeout_ms,
            x->has_upload ? &upload : NULL, knurl_proxy_upstream_ready, x,
            error);
    if (status == KNURL_OK &&
        (curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, xfer_progress) !=
             CURLE_OK ||
         curl_easy_setopt(easy, CURLOPT_XFERINFODATA, x) != CURLE_OK ||
         curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L) != CURLE_OK))
        status = KNURL_BACKEND;
    force_http1 = route->upstream_http_version == KNURL_PROXY_HTTP_1_1 ||
                  head.has_trailers;
    if (status == KNURL_OK)
        status =
            knurl_proxy_transport_submit(w->transport, easy, force_http1, 0,
                                         upstream_done, x, &x->transfer, error);
    if (status != KNURL_OK) {
        curl_easy_cleanup(easy);
        goto exchange_failed;
    }
    accounting_start(x);
    if (head.expect_continue && x->channel->impl->receive_length == 0) {
        status = knurl_response_interim(request->impl->response, 100, NULL, 0,
                                        error);
        if (status != KNURL_OK)
            goto exchange_failed;
    }
    arm_timer(x);
    knurl_proxy_schedule(x);
    status = KNURL_OK;
    goto finished;
exchange_failed:
    knurl_proxy_record_failure(
        x, status,
        status == KNURL_INVALID ? 400 : (status == KNURL_NOMEM ? 500 : 502),
        "proxy setup or rewrite failed");
    knurl_proxy_schedule(x);
    status = KNURL_OK;
finished:
    knurl_proxy_director_cleanup(&directed);
    knurl_proxy_headers_cleanup(&inbound);
    knurl_proxy_headers_cleanup(&sanitized);
    knurl_proxy_local_cleanup(&local);
    return status;
}

void knurl_proxy_worker_clear(knurl_server *server)
{
    struct knurl_proxy_worker *w;
    struct proxy_exchange *x;
    w = server->impl->proxy;
    if (w == NULL)
        return;
    server->impl->proxy = NULL;
    w->closing = 1;
    for (x = w->exchanges; x != NULL; x = x->next) {
        stop_timer(x);
        knurl_proxy_ws_stop(x);
        if (x->transfer != NULL) {
            knurl_proxy_transfer_cancel(x->transfer);
            x->transfer = NULL;
        }
    }
    (void)knurl_proxy_transport_destroy(w->transport, NULL);
    w->transport = NULL;
    worker_unref(w);
}
