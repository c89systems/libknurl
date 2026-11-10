#include "proxy_ws_runtime.h"
#include "proxy_runtime_internal.h"
#include "proxy_ws_handshake.h"
#include "proxy_ws_wire.h"
#include "proxy_common.h"
#include "response_internal.h"
#include "channel_internal.h"

#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

enum ws_phase { WS_CONNECT, WS_SEND, WS_HEAD, WS_REJECTION, WS_TUNNEL };
struct knurl_proxy_ws {
    knurl_proxy_headers inbound;
    knurl_proxy_body_framer framer;
    knurl_watch *watch;
    knurl_timer *retry;
    curl_socket_t fd;
    char *opening;
    unsigned char *head;
    unsigned char *upload;
    size_t opening_length;
    size_t opening_offset;
    size_t head_length;
    size_t upload_length;
    size_t upload_offset;
    enum ws_phase phase;
    unsigned int ready;
    long retry_delay;
    int tls;
    int connected;
    int upstream_eof;
    int downstream_eof;
    int upstream_shutdown;
    int chunked;
    int fixed;
    int stopped;
    int head_wait;
};

static void retry_ready(knurl_timer *timer, void *data)
{
    struct proxy_exchange *x;
    (void)timer;
    x = data;
    x->ws->retry = NULL;
    knurl_proxy_schedule(x);
}

static knurl_status retry_later(struct proxy_exchange *x)
{
    knurl_status status;
    struct knurl_proxy_ws *s;
    s = x->ws;
    if (s->retry != NULL)
        return KNURL_OK;
    ++x->refs;
    status = knurl_loop_timer(x->loop, s->retry_delay, 0, retry_ready,
                              knurl_proxy_release, x, &s->retry, NULL);
    if (status != KNURL_OK)
        --x->refs;
    if (s->retry_delay < 1000)
        s->retry_delay = s->retry_delay > 500 ? 1000 : s->retry_delay * 2;
    return status;
}

static void upstream_event(knurl_watch *watch, unsigned int flags, void *data)
{
    struct proxy_exchange *x;
    (void)watch;
    x = data;
    x->ws->ready |= flags;
    knurl_proxy_schedule(x);
}

static void connected(CURL *easy, CURLcode result, void *data)
{
    struct proxy_exchange *x;
    (void)easy;
    x = data;
    if (result != CURLE_OK) {
        x->transfer = NULL;
        if (x->failure.status != KNURL_OK) {
            knurl_proxy_schedule(x);
            return;
        }
        knurl_proxy_record_failure(
            x, result == CURLE_OPERATION_TIMEDOUT ? KNURL_TIMEOUT : KNURL_IO,
            result == CURLE_OPERATION_TIMEDOUT ? 504 : 502,
            curl_easy_strerror(result));
        x->failure.source = KNURL_ERROR_BACKEND;
        x->failure.native_code = (long)result;
    } else
        x->ws->connected = 1;
    knurl_proxy_schedule(x);
}

void knurl_proxy_ws_stop(struct proxy_exchange *x)
{
    struct knurl_proxy_ws *s;
    s = x->ws;
    if (s == NULL || s->stopped)
        return;
    s->stopped = 1;
    if (s->watch != NULL) {
        knurl_watch_cancel(s->watch);
        s->watch = NULL;
    }
    if (s->retry != NULL) {
        knurl_timer_cancel(s->retry);
        s->retry = NULL;
    }
}

void knurl_proxy_ws_clear(struct proxy_exchange *x)
{
    struct knurl_proxy_ws *s;
    s = x->ws;
    if (s == NULL)
        return;
    knurl_proxy_headers_cleanup(&s->inbound);
    free(s->opening);
    free(s->head);
    free(s->upload);
    free(s);
    x->ws = NULL;
}

int knurl_proxy_ws_tunneled(const struct proxy_exchange *x)
{
    return x->ws != NULL && x->ws->phase == WS_TUNNEL;
}

knurl_status knurl_proxy_ws_start(struct proxy_exchange *x, const char *url,
                                  const knurl_proxy_headers *inbound,
                                  knurl_proxy_headers *outbound,
                                  knurl_error *error)
{
    struct knurl_proxy_ws *s;
    const knurl_proxy_route_data *r;
    knurl_status status;
    CURL *easy;
    const char *host;
    size_t i;
    r = x->route;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return KNURL_NOMEM;
    x->ws = s;
    s->fd = CURL_SOCKET_BAD;
    s->retry_delay = 25;
    s->tls = strncasecmp(url, "https://", 8) == 0;
    s->head = malloc(KNURL_PROXY_HEADER_BLOCK_LIMIT);
    s->upload = malloc(r->buffer_limit_bytes);
    x->upstream.body_storage = malloc(r->buffer_limit_bytes);
    x->upstream.body = x->upstream.body_storage;
    x->upstream.body_capacity = r->buffer_limit_bytes;
    if (s->head == NULL || s->upload == NULL || x->upstream.body == NULL)
        return KNURL_NOMEM;
    host = NULL;
    for (i = 0; i < inbound->count; ++i) {
        if (strcasecmp(inbound->fields[i].name, "host") == 0)
            host = inbound->fields[i].value;
        if (knurl_proxy_headers_add(&s->inbound, inbound->fields[i].name,
                                    inbound->fields[i].value) !=
            KNURL_PROXY_HEADER_OK)
            return KNURL_NOMEM;
    }
    if (knurl_proxy_headers_add(outbound, "X-Forwarded-For",
                                x->request->peer.address.data) !=
            KNURL_PROXY_HEADER_OK ||
        knurl_proxy_headers_add(outbound, "X-Forwarded-Proto",
                                x->request->tls ? "https" : "http") !=
            KNURL_PROXY_HEADER_OK ||
        knurl_proxy_headers_add(outbound, "X-Forwarded-Host", host) !=
            KNURL_PROXY_HEADER_OK)
        return KNURL_LIMIT;
    status =
        knurl_proxy_ws_wire_request(x->target, x->authority, outbound,
                                    &s->opening, &s->opening_length, error);
    if (status != KNURL_OK)
        return status;
    easy = curl_easy_init();
    if (easy == NULL)
        return KNURL_NOMEM;
    x->upstream.easy = easy;
    status = knurl_proxy_curl_set_ca(easy, r->tls_ca_pem, r->tls_ca_pem_length,
                                     error);
    if (status == KNURL_OK)
        status = knurl_proxy_curl_set_client_identity(
            easy, r->tls_client_cert_pem, r->tls_client_cert_pem_length,
            r->tls_client_key_pem, r->tls_client_key_pem_length, error);
    if (status == KNURL_OK &&
        (curl_easy_setopt(easy, CURLOPT_URL, url) != CURLE_OK ||
         curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                          r->connect_timeout_ms) != CURLE_OK ||
         curl_easy_setopt(easy, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2) !=
             CURLE_OK ||
         curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK))
        status = KNURL_BACKEND;
    if (status == KNURL_OK)
        status =
            knurl_proxy_transport_submit(x->worker->transport, easy, 1, 1,
                                         connected, x, &x->transfer, error);
    if (status != KNURL_OK) {
        curl_easy_cleanup(easy);
        x->upstream.easy = NULL;
    }
    return status;
}

static knurl_status io_result(struct proxy_exchange *x, CURLcode code,
                              size_t amount, int sending)
{
    if (code != CURLE_OK && code != CURLE_AGAIN) {
        if (x->failure.status != KNURL_OK)
            return x->failure.status;
        knurl_proxy_record_failure(x, KNURL_IO, 502, curl_easy_strerror(code));
        x->failure.source = KNURL_ERROR_BACKEND;
        x->failure.native_code = (long)code;
        return KNURL_IO;
    }
    if (amount != 0) {
        x->ws->retry_delay = 25;
        knurl_proxy_progress(x, amount);
    } else if (code == CURLE_AGAIN && x->ws->tls &&
               (sending || (x->ws->ready & KNURL_READABLE)))
        return retry_later(x);
    return KNURL_OK;
}

static knurl_status parse_head(struct proxy_exchange *x, int *complete)
{
    struct knurl_proxy_ws *s;
    knurl_proxy_headers headers;
    knurl_proxy_headers sanitized;
    knurl_proxy_ws_head_result parsed;
    knurl_proxy_http_event event;
    knurl_header views[KNURL_PROXY_HEADER_COUNT_LIMIT + 2];
    knurl_status status;
    unsigned code;
    size_t length;
    size_t pos;
    size_t end;
    size_t i;
    const char *reason;
    s = x->ws;
    *complete = 0;
    s->head_wait = 0;
    knurl_proxy_headers_init(&headers);
    knurl_proxy_headers_init(&sanitized);
    parsed = knurl_proxy_ws_wire_response_head(s->head, s->head_length, &length,
                                               &code, &headers, &reason);
    status = parsed == KNURL_PROXY_WS_HEAD_MORE ? KNURL_AGAIN : KNURL_INVALID;
    if (parsed != KNURL_PROXY_WS_HEAD_COMPLETE)
        goto done;
    if (code == 101) {
        s->head_wait = 1;
        if (x->interims != NULL) {
            status = KNURL_AGAIN;
            goto done;
        }
        if (!knurl_proxy_ws_response_valid(&s->inbound, code, &headers,
                                           &reason) ||
            knurl_proxy_headers_sanitize_response(&headers, &sanitized) !=
                KNURL_PROXY_HEADER_OK)
            goto done;
        for (i = 0; i < sanitized.count; ++i) {
            views[i].name = knurl_view(sanitized.fields[i].name);
            views[i].value = knurl_view(sanitized.fields[i].value);
        }
        views[i].name = knurl_view("Upgrade");
        views[i++].value = knurl_view("websocket");
        views[i].name = knurl_view("Connection");
        views[i++].value = knurl_view("Upgrade");
        status = knurl_channel_upgrade(x->channel, views, i, NULL);
        if (status != KNURL_OK)
            goto done;
        s->phase = WS_TUNNEL;
    } else {
        for (pos = 0; pos < length; pos = end + 2) {
            for (end = pos; end + 1 < length; ++end)
                if (s->head[end] == '\r' && s->head[end + 1] == '\n')
                    break;
            if (knurl_proxy_http_response_header(
                    &x->upstream.response, (const char *)s->head + pos,
                    end + 2 - pos, &event, &reason) != KNURL_PROXY_HEADER_OK) {
                status = KNURL_INVALID;
                goto done;
            }
        }
        if (event == KNURL_PROXY_HTTP_FINAL &&
            knurl_proxy_headers_sanitize_response(
                &x->upstream.response.headers, &x->upstream.outbound_headers) !=
                KNURL_PROXY_HEADER_OK) {
            status = KNURL_INVALID;
            goto done;
        }
        knurl_proxy_upstream_ready(&x->upstream, event, x);
        if (event == KNURL_PROXY_HTTP_FINAL) {
            s->phase = WS_REJECTION;
            s->chunked = x->upstream.response.chunked;
            s->fixed = x->upstream.response.has_content_length;
            if (s->chunked)
                knurl_proxy_body_framer_chunked(&s->framer);
            else if (s->fixed)
                knurl_proxy_body_framer_fixed(
                    &s->framer, x->upstream.response.content_length);
            if (!x->upstream.response.body_allowed ||
                (s->fixed && x->upstream.response.content_length == 0))
                x->up_done = 1;
        }
    }
    memmove(s->head, s->head + length, s->head_length - length);
    s->head_length -= length;
    *complete = 1;
    status = KNURL_OK;
done:
    knurl_proxy_headers_cleanup(&headers);
    knurl_proxy_headers_cleanup(&sanitized);
    return status;
}

static size_t rejection_body(void *data, const unsigned char *bytes,
                             size_t length)
{
    struct proxy_exchange *x;
    const char *reason;
    x = data;
    if (length > x->upstream.body_capacity - x->upstream.body_length)
        length = x->upstream.body_capacity - x->upstream.body_length;
    if (knurl_proxy_http_response_body(&x->upstream.response, length,
                                       &reason) != KNURL_PROXY_HEADER_OK) {
        knurl_proxy_record_failure(x, KNURL_INVALID, 502, reason);
        return 0;
    }
    memcpy(x->upstream.body + x->upstream.body_length, bytes, length);
    x->upstream.body_length += length;
    return length;
}

static int rejection_trailer(void *data, const char *name, const char *value)
{
    struct proxy_exchange *x;
    x = data;
    return knurl_proxy_headers_add(&x->upstream.response.trailers, name,
                                   value) == KNURL_PROXY_HEADER_OK;
}

static knurl_status rejection_step(struct proxy_exchange *x)
{
    struct knurl_proxy_ws *s;
    knurl_proxy_frame_result frame;
    CURLcode code;
    size_t amount;
    size_t consumed;
    const char *reason;
    knurl_status status;
    s = x->ws;
    if (x->up_done || x->upstream.body_length != x->upstream.body_offset)
        return KNURL_OK;
    if (s->head_length != 0) {
        if (s->fixed || s->chunked) {
            frame = knurl_proxy_body_framer_feed(
                &s->framer, s->head, s->head_length, &consumed, rejection_body,
                rejection_trailer, x);
            if (frame == KNURL_PROXY_FRAME_INVALID)
                return KNURL_INVALID;
            if (frame == KNURL_PROXY_FRAME_COMPLETE)
                x->up_done = 1;
        } else
            consumed = rejection_body(x, s->head, s->head_length);
        memmove(s->head, s->head + consumed, s->head_length - consumed);
        s->head_length -= consumed;
        if (x->up_done &&
            knurl_proxy_http_response_finish(&x->upstream.response, &reason) !=
                KNURL_PROXY_HEADER_OK)
            return KNURL_INVALID;
        knurl_proxy_schedule(x);
        return KNURL_OK;
    }
    if (s->retry != NULL)
        return KNURL_OK;
    amount = 0;
    code = curl_easy_recv(x->upstream.easy, s->head,
                          x->route->buffer_limit_bytes <
                                  KNURL_PROXY_HEADER_BLOCK_LIMIT
                              ? x->route->buffer_limit_bytes
                              : KNURL_PROXY_HEADER_BLOCK_LIMIT,
                          &amount);
    status = io_result(x, code, amount, 0);
    if (status != KNURL_OK)
        return status;
    s->head_length = amount;
    if (code == CURLE_OK && amount == 0) {
        if (s->fixed || s->chunked)
            return KNURL_INVALID;
        if (knurl_proxy_http_response_finish(&x->upstream.response, &reason) !=
            KNURL_PROXY_HEADER_OK)
            return KNURL_INVALID;
        x->up_done = 1;
    }
    if (code == CURLE_AGAIN &&
        (s->ready & (KNURL_READ_CLOSED | KNURL_EVENT_ERROR)))
        return KNURL_IO;
    if (amount != 0 || x->up_done)
        knurl_proxy_schedule(x);
    return KNURL_OK;
}

static knurl_status tunnel_step(struct proxy_exchange *x)
{
    struct knurl_proxy_ws *s;
    knurl_status status;
    CURLcode code;
    size_t amount;
    size_t held;
    s = x->ws;
    if (s->upload_offset == s->upload_length && !s->downstream_eof &&
        !s->upstream_eof) {
        s->upload_offset = s->upload_length = 0;
        status = knurl_channel_read(
            x->channel, s->upload, x->route->buffer_limit_bytes, &amount, NULL);
        if (status == KNURL_EOF)
            s->downstream_eof = 1;
        else if (status != KNURL_OK && status != KNURL_AGAIN)
            return status;
        else if (status == KNURL_OK) {
            s->upload_length = amount;
            knurl_proxy_schedule(x);
        }
    }
    if (s->upload_offset < s->upload_length && s->retry == NULL) {
        amount = 0;
        code = curl_easy_send(x->upstream.easy, s->upload + s->upload_offset,
                              s->upload_length - s->upload_offset, &amount);
        status = io_result(x, code, amount, 1);
        if (status != KNURL_OK)
            return status;
        s->upload_offset += amount;
        knurl_proxy_count(&x->accounting.upstream_bytes, amount);
        if (amount != 0)
            knurl_proxy_schedule(x);
    }
    if (s->downstream_eof && s->upload_offset == s->upload_length &&
        !s->upstream_shutdown) {
        /* Connect-only exposes a borrowed socket, but no TLS close-notify API.
         * Drain the accepted upload before shutting the TCP write half. */
        if (shutdown(s->fd, SHUT_WR) != 0 && errno != ENOTCONN)
            return KNURL_IO;
        s->upstream_shutdown = 1;
    }
    held = x->upstream.body_length - x->upstream.body_offset;
    if (held != 0) {
        status = knurl_channel_write(x->channel,
                                     x->upstream.body + x->upstream.body_offset,
                                     held, &amount, NULL);
        if (status != KNURL_OK && status != KNURL_AGAIN)
            return status;
        if (status == KNURL_OK) {
            knurl_proxy_http_upstream_consume(&x->upstream, amount);
            knurl_proxy_count(&x->accounting.downstream_bytes, amount);
            knurl_proxy_schedule(x);
        }
        return KNURL_OK;
    }
    if (s->head_length != 0) {
        amount = s->head_length < x->upstream.body_capacity
                     ? s->head_length
                     : x->upstream.body_capacity;
        memcpy(x->upstream.body, s->head, amount);
        x->upstream.body_length = amount;
        memmove(s->head, s->head + amount, s->head_length - amount);
        s->head_length -= amount;
        knurl_proxy_schedule(x);
    } else if (!s->upstream_eof && s->retry == NULL) {
        amount = 0;
        code = curl_easy_recv(x->upstream.easy, x->upstream.body,
                              x->upstream.body_capacity, &amount);
        status = io_result(x, code, amount, 0);
        if (status != KNURL_OK)
            return status;
        x->upstream.body_length = amount;
        if ((code == CURLE_OK && amount == 0) ||
            (code == CURLE_AGAIN &&
             (s->ready & (KNURL_READ_CLOSED | KNURL_EVENT_ERROR)))) {
            s->upstream_eof = 1;
            if (x->accounting.error_category == NULL) {
                x->accounting.disconnect_side = "upstream";
                x->accounting.error_category =
                    code == CURLE_AGAIN && (s->ready & KNURL_EVENT_ERROR)
                        ? "transport"
                        : "none";
            }
        }
        if (amount != 0 || s->upstream_eof)
            knurl_proxy_schedule(x);
    }
    if (s->upstream_eof && x->upstream.body_length == x->upstream.body_offset &&
        s->upload_offset == s->upload_length) {
        if (knurl_channel_queued_bytes(x->channel) == 0)
            return knurl_channel_close(x->channel, KNURL_OK, NULL);
    }
    return KNURL_OK;
}

knurl_status knurl_proxy_ws_step(struct proxy_exchange *x)
{
    struct knurl_proxy_ws *s;
    knurl_status status;
    CURLcode code;
    size_t amount;
    unsigned int interest;
    unsigned int downstream;
    int complete;
    s = x->ws;
    if (s->stopped || x->ended)
        return KNURL_OK;
    status = KNURL_OK;
    if (s->phase == WS_CONNECT) {
        if (!s->connected)
            return KNURL_OK;
        status = knurl_proxy_transfer_handoff(x->transfer, &s->fd, NULL);
        if (status != KNURL_OK)
            return status;
        ++x->refs;
        status = knurl_loop_watch(x->loop, (int)s->fd, 0, upstream_event,
                                  knurl_proxy_release, x, &s->watch, NULL);
        if (status != KNURL_OK) {
            --x->refs;
            return status;
        }
        s->phase = WS_SEND;
    }
    if (s->phase == WS_SEND && s->retry == NULL) {
        amount = 0;
        code = curl_easy_send(x->upstream.easy, s->opening + s->opening_offset,
                              s->opening_length - s->opening_offset, &amount);
        status = io_result(x, code, amount, 1);
        s->opening_offset += amount;
        if (s->opening_offset == s->opening_length) {
            free(s->opening);
            s->opening = NULL;
            s->phase = WS_HEAD;
        }
        if (amount != 0)
            knurl_proxy_schedule(x);
    }
    if (status == KNURL_OK && s->phase == WS_HEAD) {
        complete = 0;
        status = parse_head(x, &complete);
        if (status == KNURL_AGAIN &&
            s->head_length < KNURL_PROXY_HEADER_BLOCK_LIMIT &&
            s->retry == NULL && x->interims == NULL && !s->head_wait) {
            amount = 0;
            code = curl_easy_recv(
                x->upstream.easy, s->head + s->head_length,
                KNURL_PROXY_HEADER_BLOCK_LIMIT - s->head_length <
                        x->route->buffer_limit_bytes
                    ? KNURL_PROXY_HEADER_BLOCK_LIMIT - s->head_length
                    : x->route->buffer_limit_bytes,
                &amount);
            status = io_result(x, code, amount, 0);
            s->head_length += amount;
            if ((code == CURLE_OK && amount == 0) ||
                (code == CURLE_AGAIN &&
                 (s->ready & (KNURL_READ_CLOSED | KNURL_EVENT_ERROR))))
                status = KNURL_IO;
            if (amount != 0)
                knurl_proxy_schedule(x);
        }
        if (complete)
            knurl_proxy_schedule(x);
    }
    if (status == KNURL_AGAIN)
        status = KNURL_OK;
    if (status == KNURL_OK && s->phase == WS_REJECTION)
        status = rejection_step(x);
    if (status == KNURL_OK && s->phase == WS_TUNNEL)
        status = tunnel_step(x);
    if (status != KNURL_OK)
        return status;
    interest = downstream = 0;
    if (s->retry == NULL && !x->up_done) {
        if (s->phase == WS_SEND ||
            (s->phase == WS_TUNNEL && s->upload_offset < s->upload_length))
            interest |= KNURL_WRITABLE;
        if ((s->phase == WS_HEAD && !s->head_wait) ||
            ((s->phase == WS_REJECTION || s->phase == WS_TUNNEL) &&
             !s->upstream_eof && s->head_length == 0 &&
             x->upstream.body_length == x->upstream.body_offset))
            interest |= KNURL_READABLE;
    }
    if (s->phase == WS_TUNNEL) {
        if (!s->downstream_eof && !s->upstream_eof &&
            s->upload_offset == s->upload_length)
            downstream |= KNURL_READABLE;
        if (x->upstream.body_length != x->upstream.body_offset ||
            s->upstream_eof)
            downstream |= KNURL_WRITABLE;
    }
    s->ready = 0;
    status = knurl_watch_update(s->watch, interest, NULL);
    if (status == KNURL_OK &&
        knurl_channel_state(x->channel) != KNURL_CHANNEL_CLOSED)
        status = knurl_channel_interest(x->channel, downstream, NULL);
    return status;
}
