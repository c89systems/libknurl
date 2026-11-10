#include "proxy_transport.h"
#include "proxy_common.h"
#include "loop_internal.h"

#include <limits.h>
#include <stdlib.h>

struct proxy_pool;
struct proxy_watch {
    struct proxy_watch *next;
    struct proxy_pool *pool;
    knurl_watch *watch;
    curl_socket_t fd;
    int retired;
    int unlinked;
    int handed_off;
};
struct knurl_proxy_transfer {
    knurl_proxy_transfer *next;
    struct proxy_pool *pool;
    CURL *easy;
    knurl_proxy_done_fn done;
    void *userdata;
    int active;
    int raw;
    int completed;
};
struct proxy_pool {
    knurl_proxy_transport *owner;
    CURLM *multi;
    knurl_timer *timer;
    struct proxy_watch *watches;
    knurl_proxy_transfer *transfers;
    int running;
};
struct knurl_proxy_transport {
    knurl_loop *loop;
    struct proxy_pool pools[2];
    size_t active;
    size_t limit;
    int closing;
    int driving;
};

static void drive(struct proxy_pool *, curl_socket_t, int);

static void retired(void *data)
{
    struct proxy_watch *w;
    w = data;
    w->retired = 1;
    if (w->unlinked)
        free(w);
}

static void unlink_watch(struct proxy_watch *w)
{
    struct proxy_watch **slot;
    slot = &w->pool->watches;
    while (*slot != NULL && *slot != w)
        slot = &(*slot)->next;
    if (*slot == w)
        *slot = w->next;
    w->unlinked = 1;
    if (w->watch != NULL) {
        knurl_watch_cancel(w->watch);
        w->watch = NULL;
    }
    if (w->retired)
        free(w);
}

static void ready(knurl_watch *watch, unsigned int flags, void *data)
{
    struct proxy_watch *w;
    int select;
    (void)watch;
    w = data;
    if (w->handed_off || w->pool->owner->closing)
        return;
    select = 0;
    if (flags & (KNURL_READABLE | KNURL_READ_CLOSED))
        select |= CURL_CSELECT_IN;
    if (flags & KNURL_WRITABLE)
        select |= CURL_CSELECT_OUT;
    if (flags & KNURL_EVENT_ERROR)
        select |= CURL_CSELECT_ERR;
    drive(w->pool, w->fd, select);
}

static int socket_changed(CURL *easy, curl_socket_t fd, int what, void *data,
                          void *assigned)
{
    struct proxy_pool *pool;
    struct proxy_watch *w;
    unsigned int interest;
    knurl_status status;
    (void)easy;
    pool = data;
    w = assigned;
    if (what == CURL_POLL_REMOVE) {
        (void)curl_multi_assign(pool->multi, fd, NULL);
        if (w != NULL)
            unlink_watch(w);
        return 0;
    }
    if (pool->owner->closing)
        return 0;
    interest = (what & CURL_POLL_IN ? KNURL_READABLE : 0) |
               (what & CURL_POLL_OUT ? KNURL_WRITABLE : 0);
    if (w != NULL) {
        if (w->handed_off)
            return 0;
        return knurl_watch_update(w->watch, interest, NULL) == KNURL_OK ? 0
                                                                        : -1;
    }
    w = calloc(1, sizeof(*w));
    if (w == NULL)
        return -1;
    w->pool = pool;
    w->fd = fd;
    status = knurl_loop_watch(pool->owner->loop, (int)fd, interest, ready,
                              retired, w, &w->watch, NULL);
    if (status != KNURL_OK) {
        free(w);
        return -1;
    }
    w->next = pool->watches;
    pool->watches = w;
    if (curl_multi_assign(pool->multi, fd, w) != CURLM_OK) {
        unlink_watch(w);
        return -1;
    }
    return 0;
}

static void timeout(knurl_timer *timer, void *data)
{
    struct proxy_pool *pool;
    (void)timer;
    pool = data;
    pool->timer = NULL;
    drive(pool, CURL_SOCKET_TIMEOUT, 0);
}

static int timer_changed(CURLM *multi, long milliseconds, void *data)
{
    struct proxy_pool *pool;
    (void)multi;
    pool = data;
    if (pool->timer != NULL) {
        knurl_timer_cancel(pool->timer);
        pool->timer = NULL;
    }
    if (milliseconds < 0 || pool->owner->closing)
        return 0;
    return knurl_loop_timer(pool->owner->loop, milliseconds, 0, timeout, NULL,
                            pool, &pool->timer, NULL) == KNURL_OK
               ? 0
               : -1;
}

static void unlink_transfer(knurl_proxy_transfer *transfer)
{
    knurl_proxy_transfer **slot;
    slot = &transfer->pool->transfers;
    while (*slot != NULL && *slot != transfer)
        slot = &(*slot)->next;
    if (*slot == transfer)
        *slot = transfer->next;
}

static void drive(struct proxy_pool *pool, curl_socket_t fd, int flags)
{
    knurl_proxy_transfer *t;
    CURLMsg *message;
    CURLMcode code;
    int pending;
    if (pool->owner->closing)
        return;
    ++pool->owner->driving;
    code = curl_multi_socket_action(pool->multi, fd, flags, &pool->running);
    while ((message = curl_multi_info_read(pool->multi, &pending)) != NULL) {
        if (message->msg != CURLMSG_DONE)
            continue;
        t = NULL;
        (void)curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &t);
        if (t == NULL || !t->active)
            continue;
        if (t->raw && message->data.result == CURLE_OK) {
            t->completed = 1;
            t->done(t->easy, CURLE_OK, t->userdata);
            continue;
        }
        t->active = 0;
        unlink_transfer(t);
        --pool->owner->active;
        (void)curl_multi_remove_handle(pool->multi, t->easy);
        t->done(t->easy, message->data.result, t->userdata);
        curl_easy_cleanup(t->easy);
        free(t);
    }
    if (code != CURLM_OK) {
        while ((t = pool->transfers) != NULL) {
            t->active = 0;
            unlink_transfer(t);
            --pool->owner->active;
            (void)curl_multi_remove_handle(pool->multi, t->easy);
            t->done(t->easy, CURLE_FAILED_INIT, t->userdata);
            curl_easy_cleanup(t->easy);
            free(t);
        }
    }
    --pool->owner->driving;
}

knurl_status knurl_proxy_transport_new(knurl_loop *loop, size_t active,
                                       size_t idle, knurl_proxy_transport **out,
                                       knurl_error *error)
{
    knurl_proxy_transport *engine;
    const curl_version_info_data *version;
    struct proxy_pool *pool;
    size_t i;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (out == NULL || active > LONG_MAX || idle > LONG_MAX)
        return knurl_fail(error, KNURL_INVALID, "proxy_transport",
                          "invalid output or pool limits");
    status = knurl_loop_owner(loop, error);
    if (status != KNURL_OK)
        return status;
    version = curl_version_info(CURLVERSION_NOW);
    if (version == NULL || !(version->features & CURL_VERSION_ASYNCHDNS) ||
        !(version->features & CURL_VERSION_HTTP2))
        return knurl_fail(error, KNURL_NOT_SUPPORTED, "proxy_transport",
                          "asynchronous DNS and HTTP/2 curl are required");
    engine = calloc(1, sizeof(*engine));
    if (engine == NULL)
        return knurl_fail(error, KNURL_NOMEM, "proxy_transport",
                          "cannot allocate transport");
    engine->loop = loop;
    engine->limit = active == 0 ? 16 : active;
    if (idle == 0)
        idle = 4;
    for (i = 0; i < 2; ++i) {
        pool = &engine->pools[i];
        pool->owner = engine;
        pool->multi = curl_multi_init();
        if (pool->multi == NULL ||
            curl_multi_setopt(pool->multi, CURLMOPT_SOCKETFUNCTION,
                              socket_changed) != CURLM_OK ||
            curl_multi_setopt(pool->multi, CURLMOPT_SOCKETDATA, pool) !=
                CURLM_OK ||
            curl_multi_setopt(pool->multi, CURLMOPT_TIMERFUNCTION,
                              timer_changed) != CURLM_OK ||
            curl_multi_setopt(pool->multi, CURLMOPT_TIMERDATA, pool) !=
                CURLM_OK ||
            curl_multi_setopt(pool->multi, CURLMOPT_PIPELINING,
                              CURLPIPE_NOTHING) != CURLM_OK ||
            curl_multi_setopt(pool->multi, CURLMOPT_MAX_CONCURRENT_STREAMS,
                              1L) != CURLM_OK ||
            curl_multi_setopt(pool->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                              (long)engine->limit) != CURLM_OK ||
            curl_multi_setopt(pool->multi, CURLMOPT_MAXCONNECTS, (long)idle) !=
                CURLM_OK) {
            (void)knurl_proxy_transport_destroy(engine, NULL);
            return knurl_fail(error, KNURL_BACKEND, "proxy_transport",
                              "cannot initialize curl pools");
        }
    }
    *out = engine;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_proxy_transport_submit(knurl_proxy_transport *engine,
                                          CURL *easy, int force_http1, int raw,
                                          knurl_proxy_done_fn done, void *data,
                                          knurl_proxy_transfer **out,
                                          knurl_error *error)
{
    knurl_proxy_transfer *t;
    struct proxy_pool *pool;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (engine == NULL || easy == NULL || done == NULL || out == NULL ||
        (force_http1 != 0 && force_http1 != 1) || (raw != 0 && raw != 1))
        return knurl_fail(
            error, KNURL_INVALID, "proxy_submit",
            "provide transport, easy, flags, callback and output");
    status = knurl_loop_owner(engine->loop, error);
    if (status != KNURL_OK)
        return status;
    if (engine->closing)
        return knurl_fail(error, KNURL_STATE, "proxy_submit",
                          "transport is closing");
    if (engine->active >= engine->limit)
        return knurl_fail(error, KNURL_LIMIT, "proxy_submit",
                          "shared HTTP/WebSocket admission limit reached");
    pool = &engine->pools[raw || force_http1 ? 1 : 0];
    t = calloc(1, sizeof(*t));
    if (t == NULL)
        return knurl_fail(error, KNURL_NOMEM, "proxy_submit",
                          "cannot allocate transfer");
    if (!knurl_proxy_curl_configure_http(easy, raw || force_http1) ||
        (raw &&
         (curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L) != CURLE_OK ||
          curl_easy_setopt(easy, CURLOPT_SSL_ENABLE_ALPN, 0L) != CURLE_OK ||
          curl_easy_setopt(easy, CURLOPT_FRESH_CONNECT, 1L) != CURLE_OK)) ||
        curl_easy_setopt(easy, CURLOPT_PRIVATE, t) != CURLE_OK) {
        free(t);
        return knurl_fail(error, KNURL_BACKEND, "proxy_submit",
                          "cannot configure transfer");
    }
    t->pool = pool;
    t->easy = easy;
    t->done = done;
    t->userdata = data;
    t->raw = raw;
    t->active = 1;
    t->next = pool->transfers;
    pool->transfers = t;
    ++engine->active;
    if (curl_multi_add_handle(pool->multi, easy) != CURLM_OK) {
        t->active = 0;
        unlink_transfer(t);
        --engine->active;
        (void)curl_easy_setopt(easy, CURLOPT_PRIVATE, NULL);
        free(t);
        return knurl_fail(error, KNURL_BACKEND, "proxy_submit",
                          "cannot submit transfer");
    }
    *out = t;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_proxy_transfer_cancel(knurl_proxy_transfer *t)
{
    if (t == NULL || !t->active)
        return;
    t->active = 0;
    unlink_transfer(t);
    --t->pool->owner->active;
    (void)curl_easy_setopt(t->easy, CURLOPT_FORBID_REUSE, 1L);
    (void)curl_multi_remove_handle(t->pool->multi, t->easy);
    curl_easy_cleanup(t->easy);
    free(t);
}

knurl_status knurl_proxy_transfer_handoff(knurl_proxy_transfer *t,
                                          curl_socket_t *out,
                                          knurl_error *error)
{
    struct proxy_watch *w;
    curl_socket_t fd;
    if (out != NULL)
        *out = CURL_SOCKET_BAD;
    if (t == NULL || out == NULL || !t->active || !t->raw || !t->completed ||
        curl_easy_getinfo(t->easy, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK ||
        fd == CURL_SOCKET_BAD)
        return knurl_fail(error, KNURL_STATE, "proxy_handoff",
                          "connect-only socket is not ready");
    for (w = t->pool->watches; w != NULL; w = w->next)
        if (w->fd == fd) {
            w->handed_off = 1;
            if (w->watch != NULL) {
                knurl_watch_cancel(w->watch);
                w->watch = NULL;
            }
            break;
        }
    *out = fd;
    knurl_error_clear(error);
    return KNURL_OK;
}

size_t knurl_proxy_transport_active(const knurl_proxy_transport *engine)
{
    return engine != NULL ? engine->active : 0;
}

knurl_status knurl_proxy_transport_destroy(knurl_proxy_transport *engine,
                                           knurl_error *error)
{
    struct proxy_pool *pool;
    size_t i;
    knurl_status status;
    if (engine == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    status = knurl_loop_owner(engine->loop, error);
    if (status != KNURL_OK)
        return status;
    if (engine->driving)
        return knurl_fail(error, KNURL_STATE, "proxy_destroy",
                          "defer destruction until transport callback returns");
    engine->closing = 1;
    for (i = 0; i < 2; ++i) {
        pool = &engine->pools[i];
        while (pool->transfers != NULL)
            knurl_proxy_transfer_cancel(pool->transfers);
        if (pool->timer != NULL)
            knurl_timer_cancel(pool->timer);
        if (pool->multi != NULL)
            (void)curl_multi_cleanup(pool->multi);
        while (pool->watches != NULL)
            unlink_watch(pool->watches);
    }
    free(engine);
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_proxy_curl_set_ca(CURL *easy, char *pem, size_t pem_length,
                                     knurl_error *error)
{
    struct curl_blob blob;

    if (easy == NULL || (pem == NULL && pem_length != 0u) ||
        (pem != NULL && pem_length == 0u)) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid proxy TLS CA bundle configuration");
        return KNURL_INVALID;
    }
    if (pem == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    blob.data = pem;
    blob.len = pem_length;
    blob.flags = CURL_BLOB_COPY;
    if (curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &blob) != CURLE_OK) {
        knurl_proxy_error(error, KNURL_BACKEND,
                          "failed to configure proxy TLS CA bundle");
        return KNURL_BACKEND;
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_proxy_curl_set_client_identity(CURL *easy, char *cert_pem,
                                                  size_t cert_length,
                                                  char *key_pem,
                                                  size_t key_length,
                                                  knurl_error *error)
{
    struct curl_blob blob;

    if (easy == NULL || (cert_pem == NULL) != (key_pem == NULL) ||
        (cert_pem == NULL && (cert_length != 0u || key_length != 0u)) ||
        (cert_pem != NULL && (cert_length == 0u || key_length == 0u))) {
        knurl_proxy_error(error, KNURL_INVALID,
                          "invalid proxy TLS client identity configuration");
        return KNURL_INVALID;
    }
    if (cert_pem == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    blob.data = cert_pem;
    blob.len = cert_length;
    blob.flags = CURL_BLOB_COPY;
    if (curl_easy_setopt(easy, CURLOPT_SSLCERT_BLOB, &blob) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_SSLCERTTYPE, "PEM") != CURLE_OK) {
        knurl_proxy_error(error, KNURL_BACKEND,
                          "failed to configure proxy TLS client certificate");
        return KNURL_BACKEND;
    }
    blob.data = key_pem;
    blob.len = key_length;
    if (curl_easy_setopt(easy, CURLOPT_SSLKEY_BLOB, &blob) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_SSLKEYTYPE, "PEM") != CURLE_OK) {
        knurl_proxy_error(error, KNURL_BACKEND,
                          "failed to configure proxy TLS client key");
        return KNURL_BACKEND;
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

int knurl_proxy_curl_configure_http(CURL *easy, int force_http1)
{
    if (easy == NULL || (force_http1 != 0 && force_http1 != 1))
        return 0;
    return curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
                            force_http1 ? CURL_HTTP_VERSION_1_1
                                        : CURL_HTTP_VERSION_2TLS) == CURLE_OK &&
           (force_http1 ||
            curl_easy_setopt(easy, CURLOPT_SSLVERSION,
                             CURL_SSLVERSION_TLSv1_2) == CURLE_OK) &&
           curl_easy_setopt(easy, CURLOPT_NOPROXY, "*") == CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_HTTP_CONTENT_DECODING, 0L) ==
               CURLE_OK;
}
