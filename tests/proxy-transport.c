#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include "proxy_transport.h"
#include "proxy_http_upstream.h"
#include "proxy-check.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct fixture {
    rotmos_server *server;
    rotmos_proxy_transport *transport;
    rotmos_proxy_transfer *transfer;
    rotmos_proxy_http_upstream upstream;
    rotmos_timer *drain;
    rotmos_timer *watchdog;
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

static rotmos_status produce(void *buffer, size_t capacity, size_t *count,
                             void *data, rotmos_error *error)
{
    struct producer *p;
    (void)error;
    p = data;
    *count = 131072 - p->position;
    if (*count == 0)
        return ROTMOS_EOF;
    if (*count > capacity)
        *count = capacity;
    memset(buffer, 'x', *count);
    p->position += *count;
    return ROTMOS_OK;
}

static void producer_close(void *data, rotmos_status status)
{
    (void)status;
    free(data);
}

static rotmos_status route(rotmos_request *request, rotmos_reply *reply,
                           void *data, rotmos_error *error)
{
    struct producer *p;
    rotmos_source *source;
    rotmos_source_config config;
    rotmos_status status;
    (void)request;
    (void)data;
    p = calloc(1, sizeof(*p));
    if (p == NULL)
        return ROTMOS_NOMEM;
    rotmos_source_config_init(&config);
    config.read = produce;
    config.close = producer_close;
    config.userdata = p;
    status = rotmos_source_new(&config, &source, error);
    if (status != ROTMOS_OK) {
        free(p);
        return status;
    }
    status = rotmos_reply_source_take(reply, 200, "application/octet-stream",
                                      source, error);
    if (status != ROTMOS_OK)
        (void)rotmos_source_destroy(source, NULL);
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
    rotmos_proxy_transfer *transfer;
    PROXY_CHECK(f->received == 131072 && f->final == 1 &&
                rotmos_proxy_transport_active(f->transport) == 0);
    easy = curl_easy_init();
    PROXY_CHECK(easy != NULL &&
                curl_easy_setopt(easy, CURLOPT_URL, f->url) == CURLE_OK);
    PROXY_CHECK(rotmos_proxy_transport_submit(f->transport, easy, 1, 0,
                                              cancelled, f, &transfer,
                                              NULL) == ROTMOS_OK);
    PROXY_CHECK(rotmos_proxy_transport_active(f->transport) == 1);
    rotmos_proxy_transfer_cancel(transfer);
    PROXY_CHECK(rotmos_proxy_transport_active(f->transport) == 0 &&
                !f->cancel_callbacks);
    PROXY_CHECK(rotmos_server_request_stop(f->server, NULL) == ROTMOS_OK);
}

static void drain(rotmos_timer *timer, void *data)
{
    struct fixture *f;
    unsigned char *body;
    size_t count;
    size_t i;
    (void)timer;
    f = data;
    f->drain = NULL;
    body = rotmos_proxy_http_upstream_body(&f->upstream, &count);
    if (count > 2048)
        count = 2048;
    for (i = 0; i < count; ++i)
        PROXY_CHECK(body[i] == 'x');
    f->received += count;
    rotmos_proxy_http_upstream_consume(&f->upstream, count);
    PROXY_CHECK(f->received <= 131072);
    (void)rotmos_proxy_http_upstream_body(&f->upstream, &count);
    if (count != 0)
        PROXY_CHECK(rotmos_loop_timer(rotmos_server_loop(f->server), 1, 0,
                                      drain, NULL, f, &f->drain,
                                      NULL) == ROTMOS_OK);
    else if (f->done)
        complete(f);
    else
        PROXY_CHECK(rotmos_proxy_http_upstream_resume(&f->upstream, NULL) ==
                    ROTMOS_OK);
}

static void upstream_ready(rotmos_proxy_http_upstream *upstream,
                           rotmos_proxy_http_event event, void *data)
{
    struct fixture *f;
    size_t count;
    f = data;
    if (event == ROTMOS_PROXY_HTTP_FINAL) {
        PROXY_CHECK(upstream->response.status == 200);
        ++f->final;
    }
    if (event == ROTMOS_PROXY_HTTP_MORE) {
        (void)rotmos_proxy_http_upstream_body(upstream, &count);
        PROXY_CHECK(count != 0 && count <= upstream->body_capacity &&
                    upstream->body_capacity == CURL_MAX_WRITE_SIZE);
        if (f->drain == NULL)
            PROXY_CHECK(rotmos_loop_timer(rotmos_server_loop(f->server), 1, 0,
                                          drain, NULL, f, &f->drain,
                                          NULL) == ROTMOS_OK);
    }
}

static void done(CURL *easy, CURLcode code, void *data)
{
    struct fixture *f;
    size_t count;
    f = data;
    PROXY_CHECK(easy == f->upstream.easy && code == CURLE_OK && !f->done++);
    PROXY_CHECK(rotmos_proxy_transport_destroy(f->transport, NULL) ==
                ROTMOS_STATE);
    PROXY_CHECK(rotmos_proxy_http_upstream_finish(&f->upstream, code, NULL) ==
                ROTMOS_PROXY_HEADER_OK);
    f->transfer = NULL;
    (void)rotmos_proxy_http_upstream_body(&f->upstream, &count);
    if (count == 0)
        complete(f);
}

static void watchdog(rotmos_timer *timer, void *data)
{
    struct fixture *f;
    (void)timer;
    f = data;
    f->watchdog = NULL;
    f->failed = 1;
    (void)rotmos_server_request_stop(f->server, NULL);
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *p, void *data,
                           rotmos_error *error)
{
    struct fixture *f;
    rotmos_bound_listener listener;
    CURL *easy;
    CURL *other;
    rotmos_proxy_transfer *rejected;
    (void)p;
    f = data;
    f->server = server;
    PROXY_CHECK(rotmos_server_listener_at(server, 0, &listener, error) ==
                ROTMOS_OK);
    (void)snprintf(f->url, sizeof(f->url), "http://127.0.0.1:%u/bytes",
                   listener.port);
    PROXY_CHECK(rotmos_proxy_transport_new(rotmos_server_loop(server), 1, 4,
                                           &f->transport, error) == ROTMOS_OK);
    easy = curl_easy_init();
    PROXY_CHECK(easy != NULL);
    PROXY_CHECK(rotmos_proxy_http_upstream_init(
                    &f->upstream, easy, f->url, "/bytes", "GET", NULL, 8192,
                    1000, 2000, NULL, upstream_ready, f, error) == ROTMOS_OK);
    PROXY_CHECK(rotmos_proxy_transport_submit(f->transport, easy, 0, 0, done, f,
                                              &f->transfer,
                                              error) == ROTMOS_OK);
    other = curl_easy_init();
    PROXY_CHECK(other != NULL);
    PROXY_CHECK(rotmos_proxy_transport_submit(f->transport, other, 1, 1,
                                              cancelled, f, &rejected,
                                              error) == ROTMOS_LIMIT &&
                rejected == NULL);
    curl_easy_cleanup(other);
    PROXY_CHECK(rotmos_loop_timer(rotmos_server_loop(server), 3000, 0, watchdog,
                                  NULL, f, &f->watchdog, error) == ROTMOS_OK);
    ++f->started;
    return ROTMOS_OK;
}

static rotmos_status stopping(rotmos_server *server,
                              const rotmos_process_context *p, void *data,
                              rotmos_error *error)
{
    struct fixture *f;
    (void)server;
    (void)p;
    f = data;
    if (f->drain != NULL)
        rotmos_timer_cancel(f->drain);
    if (f->watchdog != NULL)
        rotmos_timer_cancel(f->watchdog);
    PROXY_CHECK(rotmos_proxy_transport_destroy(f->transport, error) ==
                ROTMOS_OK);
    f->transport = NULL;
    rotmos_proxy_http_upstream_cleanup(&f->upstream);
    return ROTMOS_OK;
}

int main(void)
{
    struct fixture f;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_route_config declaration;
    rotmos_server *server;
    rotmos_error error;
    rotmos_run_result result;
    memset(&f, 0, sizeof(f));
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_parent_stopping = stopping;
    config.callbacks.userdata = &f;
    PROXY_CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    rotmos_route_config_init(&declaration);
    declaration.path = "/bytes";
    declaration.handler = route;
    PROXY_CHECK(rotmos_router_route(rotmos_server_router(server), &declaration,
                                    NULL, &error) == ROTMOS_OK);
    PROXY_CHECK(rotmos_server_run(server, &result, &error) == ROTMOS_OK);
    PROXY_CHECK(!f.failed && f.started == 1 && f.done == 1 &&
                !f.cancel_callbacks);
    PROXY_CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    return 0;
}
