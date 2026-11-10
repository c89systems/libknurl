#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include "proxy_transport.h"
#include "proxy_http_upstream.h"
#include "proxy-check.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct fixture {
    knurl_server *server;
    knurl_proxy_transport *transport;
    knurl_proxy_transfer *transfer;
    knurl_proxy_http_upstream upstream;
    knurl_timer *drain;
    knurl_timer *watchdog;
    char url[128];
    size_t received;
    int final;
    int done;
    int cancel_callbacks;
    int failed;
    int started;
};
struct producer {
    size_t position;
};

static knurl_status produce(void *buffer, size_t capacity, size_t *count,
                            void *data, knurl_error *error)
{
    struct producer *p;
    (void)error;
    p = data;
    *count = 131072 - p->position;
    if (*count == 0)
        return KNURL_EOF;
    if (*count > capacity)
        *count = capacity;
    memset(buffer, 'x', *count);
    p->position += *count;
    return KNURL_OK;
}

static void producer_close(void *data, knurl_status status)
{
    (void)status;
    free(data);
}

static knurl_status route(knurl_request *request, knurl_reply *reply,
                          void *data, knurl_error *error)
{
    struct producer *p;
    knurl_source *source;
    knurl_source_config config;
    knurl_status status;
    (void)request;
    (void)data;
    p = calloc(1, sizeof(*p));
    if (p == NULL)
        return KNURL_NOMEM;
    knurl_source_config_init(&config);
    config.read = produce;
    config.close = producer_close;
    config.userdata = p;
    status = knurl_source_new(&config, &source, error);
    if (status != KNURL_OK) {
        free(p);
        return status;
    }
    status = knurl_reply_source_take(reply, 200, "application/octet-stream",
                                     source, error);
    if (status != KNURL_OK)
        (void)knurl_source_destroy(source, NULL);
    return status;
}

static void cancelled(CURL *easy, CURLcode result, void *data)
{
    struct fixture *f;
    (void)easy;
    (void)result;
    f = data;
    ++f->cancel_callbacks;
}

static void complete(struct fixture *f)
{
    CURL *easy;
    knurl_proxy_transfer *transfer;
    PROXY_CHECK(f->received == 131072 && f->final == 1 &&
                knurl_proxy_transport_active(f->transport) == 0);
    easy = curl_easy_init();
    PROXY_CHECK(easy != NULL &&
                curl_easy_setopt(easy, CURLOPT_URL, f->url) == CURLE_OK);
    PROXY_CHECK(knurl_proxy_transport_submit(f->transport, easy, 1, 0,
                                             cancelled, f, &transfer,
                                             NULL) == KNURL_OK);
    PROXY_CHECK(knurl_proxy_transport_active(f->transport) == 1);
    knurl_proxy_transfer_cancel(transfer);
    PROXY_CHECK(knurl_proxy_transport_active(f->transport) == 0 &&
                !f->cancel_callbacks);
    PROXY_CHECK(knurl_server_request_stop(f->server, NULL) == KNURL_OK);
}

static void drain(knurl_timer *timer, void *data)
{
    struct fixture *f;
    unsigned char *body;
    size_t count;
    size_t i;
    (void)timer;
    f = data;
    f->drain = NULL;
    body = knurl_proxy_http_upstream_body(&f->upstream, &count);
    if (count > 2048)
        count = 2048;
    for (i = 0; i < count; ++i)
        PROXY_CHECK(body[i] == 'x');
    f->received += count;
    knurl_proxy_http_upstream_consume(&f->upstream, count);
    PROXY_CHECK(f->received <= 131072);
    (void)knurl_proxy_http_upstream_body(&f->upstream, &count);
    if (count != 0)
        PROXY_CHECK(knurl_loop_timer(knurl_server_loop(f->server), 1, 0, drain,
                                     NULL, f, &f->drain, NULL) == KNURL_OK);
    else if (f->done)
        complete(f);
    else
        PROXY_CHECK(knurl_proxy_http_upstream_resume(&f->upstream, NULL) ==
                    KNURL_OK);
}

static void upstream_ready(knurl_proxy_http_upstream *upstream,
                           knurl_proxy_http_event event, void *data)
{
    struct fixture *f;
    size_t count;
    f = data;
    if (event == KNURL_PROXY_HTTP_FINAL) {
        PROXY_CHECK(upstream->response.status == 200);
        ++f->final;
    }
    if (event == KNURL_PROXY_HTTP_MORE) {
        (void)knurl_proxy_http_upstream_body(upstream, &count);
        PROXY_CHECK(count != 0 && count <= upstream->body_capacity &&
                    upstream->body_capacity == CURL_MAX_WRITE_SIZE);
        if (f->drain == NULL)
            PROXY_CHECK(knurl_loop_timer(knurl_server_loop(f->server), 1, 0,
                                         drain, NULL, f, &f->drain,
                                         NULL) == KNURL_OK);
    }
}

static void done(CURL *easy, CURLcode code, void *data)
{
    struct fixture *f;
    size_t count;
    f = data;
    PROXY_CHECK(easy == f->upstream.easy && code == CURLE_OK && !f->done++);
    PROXY_CHECK(knurl_proxy_transport_destroy(f->transport, NULL) ==
                KNURL_STATE);
    PROXY_CHECK(knurl_proxy_http_upstream_finish(&f->upstream, code, NULL) ==
                KNURL_PROXY_HEADER_OK);
    f->transfer = NULL;
    (void)knurl_proxy_http_upstream_body(&f->upstream, &count);
    if (count == 0)
        complete(f);
}

static void watchdog(knurl_timer *timer, void *data)
{
    struct fixture *f;
    (void)timer;
    f = data;
    f->watchdog = NULL;
    f->failed = 1;
    (void)knurl_server_request_stop(f->server, NULL);
}

static knurl_status ready(knurl_server *server, const knurl_process_context *p,
                          void *data, knurl_error *error)
{
    struct fixture *f;
    knurl_bound_listener listener;
    CURL *easy;
    CURL *other;
    knurl_proxy_transfer *rejected;
    (void)p;
    f = data;
    f->server = server;
    PROXY_CHECK(knurl_server_listener_at(server, 0, &listener, error) ==
                KNURL_OK);
    (void)snprintf(f->url, sizeof(f->url), "http://127.0.0.1:%u/bytes",
                   listener.port);
    PROXY_CHECK(knurl_proxy_transport_new(knurl_server_loop(server), 1, 4,
                                          &f->transport, error) == KNURL_OK);
    easy = curl_easy_init();
    PROXY_CHECK(easy != NULL);
    PROXY_CHECK(knurl_proxy_http_upstream_init(
                    &f->upstream, easy, f->url, "/bytes", "GET", NULL, 8192,
                    1000, 2000, NULL, upstream_ready, f, error) == KNURL_OK);
    PROXY_CHECK(knurl_proxy_transport_submit(f->transport, easy, 0, 0, done, f,
                                             &f->transfer, error) == KNURL_OK);
    other = curl_easy_init();
    PROXY_CHECK(other != NULL);
    PROXY_CHECK(knurl_proxy_transport_submit(f->transport, other, 1, 1,
                                             cancelled, f, &rejected,
                                             error) == KNURL_LIMIT &&
                rejected == NULL);
    curl_easy_cleanup(other);
    PROXY_CHECK(knurl_loop_timer(knurl_server_loop(server), 3000, 0, watchdog,
                                 NULL, f, &f->watchdog, error) == KNURL_OK);
    ++f->started;
    return KNURL_OK;
}

static knurl_status stopping(knurl_server *server,
                             const knurl_process_context *p, void *data,
                             knurl_error *error)
{
    struct fixture *f;
    (void)server;
    (void)p;
    f = data;
    if (f->drain != NULL)
        knurl_timer_cancel(f->drain);
    if (f->watchdog != NULL)
        knurl_timer_cancel(f->watchdog);
    PROXY_CHECK(knurl_proxy_transport_destroy(f->transport, error) == KNURL_OK);
    f->transport = NULL;
    knurl_proxy_http_upstream_cleanup(&f->upstream);
    return KNURL_OK;
}

int main(void)
{
    struct fixture f;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config declaration;
    knurl_server *server;
    knurl_error error;
    knurl_run_result result;
    memset(&f, 0, sizeof(f));
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_parent_stopping = stopping;
    config.callbacks.userdata = &f;
    PROXY_CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    knurl_route_config_init(&declaration);
    declaration.path = "/bytes";
    declaration.handler = route;
    PROXY_CHECK(knurl_router_route(knurl_server_router(server), &declaration,
                                   NULL, &error) == KNURL_OK);
    PROXY_CHECK(knurl_server_run(server, &result, &error) == KNURL_OK);
    PROXY_CHECK(!f.failed && f.started == 1 && f.done == 1 &&
                !f.cancel_callbacks);
    PROXY_CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    return 0;
}
