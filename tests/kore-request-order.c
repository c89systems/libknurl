#include "library.h"
#include "http.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                      \
    do {                                                                       \
        if (!(expression)) {                                                   \
            fprintf(stderr, "request order line %d: %s\n", __LINE__,           \
                    #expression);                                              \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static unsigned int handled[8];
static size_t count;

static int pending(struct http_request *request)
{
    if (count < sizeof(handled) / sizeof(handled[0]))
        handled[count] = (unsigned int)(request->path[1] - '0');
    ++count;
    return KORE_RESULT_RETRY;
}

static struct connection *queue(struct listener *listener, unsigned int id)
{
    struct connection *connection;
    int length;
    connection = kore_connection_new(listener);
    connection->fd = -1;
    connection->family = AF_INET;
    connection->state = CONN_STATE_ESTABLISHED;
    connection->proto = CONN_PROTO_HTTP;
    ++worker_active_connections;
    net_recv_queue(connection, 128, 0, http_header_recv);
    length = snprintf((char *)connection->rnb->buf, 128,
                      "GET /%u HTTP/1.1\r\nHost: localhost\r\n\r\n", id);
    if (length < 0 || length >= 128)
        fatal("cannot prepare ordering request");
    connection->rnb->s_off = (size_t)length;
    if (http_header_recv(connection->rnb) != KORE_RESULT_OK ||
        TAILQ_EMPTY(&connection->http_requests))
        fatal("cannot parse ordering request");
    return connection;
}

int main(void)
{
    struct kore_library_hooks hooks = {0};
    struct kore_native_callbacks callbacks = {0};
    struct kore_server *server;
    struct kore_domain *domain;
    struct kore_route *route;
    struct listener listener;
    struct connection *connections[4];
    struct http_request *oldest;
    size_t i;

    kore_library_init(&hooks);
    kore_quiet = 1;
    worker_max_connections = 32;
    http_request_limit = 16;
    net_init();
    kore_connection_init();
    http_init();
    server = kore_server_create("request-order");
    server->tls = 0;
    domain = kore_domain_new("*");
    CHECK(kore_domain_attach(domain, server));
    route = kore_route_create(domain, "^/[0-3]$", HANDLER_TYPE_DYNAMIC);
    CHECK(route != NULL);
    callbacks.http_request = pending;
    route->rcall = kore_runtime_native(&callbacks);
    memset(&listener, 0, sizeof(listener));
    listener.server = server;

    connections[0] = queue(&listener, 0);
    oldest = TAILQ_FIRST(&connections[0]->http_requests);
    http_process();
    CHECK(count == 1 && handled[0] == 0);
    /* A handler returning RETRY remains pending when newer requests arrive. */
    connections[1] = queue(&listener, 1);
    connections[2] = queue(&listener, 2);
    http_process();
    CHECK(count == 4 && handled[1] == 0 && handled[2] == 1 && handled[3] == 2);

    /* Waking a sleeper joins the tail after already pending/new requests. */
    http_request_sleep(oldest);
    connections[3] = queue(&listener, 3);
    http_request_wakeup(oldest);
    http_process();
    CHECK(count == 8 && handled[4] == 1 && handled[5] == 2 && handled[6] == 3 &&
          handled[7] == 0);

    for (i = 0; i < 4; ++i)
        kore_connection_remove(connections[i]);
    http_process();
    CHECK(http_request_count == 0 && worker_active_connections == 0);
    http_cleanup();
    kore_connection_cleanup();
    net_cleanup();
    kore_library_cleanup();
    return 0;
}
