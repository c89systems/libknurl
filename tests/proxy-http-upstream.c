#include "proxy_http_upstream.h"

#include <arpa/inet.h>
#include "proxy-check.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>

static void pause_origin(long microseconds)
{
    struct timespec delay;
    delay.tv_sec = microseconds / 1000000L;
    delay.tv_nsec = (microseconds % 1000000L) * 1000L;
    (void)nanosleep(&delay, NULL);
}

struct stream_server {
    int listener;
    unsigned short port;
    pthread_t thread;
    volatile int send_second;
};

struct stream_observation {
    unsigned informational;
    unsigned final;
    unsigned body;
    unsigned trailers;
};

struct upload_source {
    size_t offset;
};

static size_t upload_read(char *buffer, size_t size, size_t count,
                          void *userdata)
{
    static const char payload[] = "abcdefgh";
    struct upload_source *source;
    size_t capacity;
    size_t amount;

    source = (struct upload_source *)userdata;
    capacity = size * count;
    amount = sizeof(payload) - 1u - source->offset;
    if (amount > capacity)
        amount = capacity;
    if (amount > 4u)
        amount = 4u;
    memcpy(buffer, payload + source->offset, amount);
    source->offset += amount;
    return amount;
}

static void ready(knurl_proxy_http_upstream *upstream,
                  knurl_proxy_http_event event, void *userdata);

static void send_all(int fd, const void *data, size_t length)
{
    const unsigned char *cursor;
    ssize_t sent;

    cursor = (const unsigned char *)data;
    while (length != 0u) {
        sent = send(fd, cursor, length, 0);
        PROXY_CHECK(sent > 0);
        cursor += (size_t)sent;
        length -= (size_t)sent;
    }
}

static void *serve(void *userdata)
{
    static const char start[] =
        "HTTP/1.1 103 Early Hints\r\nLink: </a.css>; rel=preload\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
        "Transfer-Encoding: chunked\r\nTrailer: X-Final\r\n"
        "Connection: close\r\n\r\n2000\r\n";
    static const char end[] = "\r\n0\r\nX-Final: yes\r\n\r\n";
    struct stream_server *server;
    struct timeval timeout;
    char request[2048];
    char body[8192];
    size_t used;
    ssize_t got;
    int fd;
    int spins;

    server = (struct stream_server *)userdata;
    fd = accept(server->listener, NULL, NULL);
    PROXY_CHECK(fd >= 0);
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    PROXY_CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout)) == 0);
    used = 0u;
    do {
        got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
        PROXY_CHECK(got > 0);
        used += (size_t)got;
        request[used] = '\0';
    } while (strstr(request, "\r\n\r\n") == NULL &&
             used < sizeof(request) - 1u);
    PROXY_CHECK(strstr(request, "GET /rewritten?q=1&q=2 HTTP/1.1\r\n") != NULL);
    send_all(fd, start, sizeof(start) - 1u);
    memset(body, 'A', sizeof(body));
    send_all(fd, body, sizeof(body));
    send_all(fd, "\r\n", 2u);
    for (spins = 0; spins < 500 && !server->send_second; ++spins)
        pause_origin(10000L);
    PROXY_CHECK(server->send_second);
    send_all(fd, "2000\r\n", 6u);
    memset(body, 'B', sizeof(body));
    send_all(fd, body, sizeof(body));
    send_all(fd, end, sizeof(end) - 1u);
    PROXY_CHECK(close(fd) == 0);
    return NULL;
}

static void start_server(struct stream_server *server, void *(*handler)(void *))
{
    struct sockaddr_in address;
    socklen_t length;

    memset(server, 0, sizeof(*server));
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    PROXY_CHECK(server->listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    PROXY_CHECK(bind(server->listener, (struct sockaddr *)&address,
                     sizeof(address)) == 0);
    PROXY_CHECK(listen(server->listener, 1) == 0);
    length = sizeof(address);
    PROXY_CHECK(getsockname(server->listener, (struct sockaddr *)&address,
                            &length) == 0);
    server->port = ntohs(address.sin_port);
    PROXY_CHECK(pthread_create(&server->thread, NULL, handler, server) == 0);
}

static void *serve_upload(void *userdata)
{
    static const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                                "Connection: close\r\n\r\nok";
    struct stream_server *server;
    struct timeval timeout;
    char request[2048];
    char *body;
    size_t used;
    size_t body_length;
    ssize_t got;
    int fd;

    server = (struct stream_server *)userdata;
    fd = accept(server->listener, NULL, NULL);
    PROXY_CHECK(fd >= 0);
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    PROXY_CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout)) == 0);
    used = 0u;
    body = NULL;
    while (body == NULL && used < sizeof(request) - 1u) {
        got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
        PROXY_CHECK(got > 0);
        used += (size_t)got;
        request[used] = '\0';
        body = strstr(request, "\r\n\r\n");
    }
    PROXY_CHECK(body != NULL);
    PROXY_CHECK(strstr(request, "GET /upload HTTP/1.1\r\n") != NULL);
    PROXY_CHECK(strstr(request, "Content-Length: 8\r\n") != NULL);
    body += 4;
    body_length = used - (size_t)(body - request);
    while (body_length < 8u && used < sizeof(request) - 1u) {
        got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
        PROXY_CHECK(got > 0);
        used += (size_t)got;
        body_length += (size_t)got;
    }
    PROXY_CHECK(body_length == 8u && memcmp(body, "abcdefgh", 8u) == 0);
    send_all(fd, reply, sizeof(reply) - 1u);
    PROXY_CHECK(close(fd) == 0);
    return NULL;
}

static void test_framed_get_upload(void)
{
    struct stream_server server;
    struct stream_observation observation;
    struct upload_source source;
    knurl_proxy_http_upload upload;
    knurl_proxy_http_upstream upstream;
    knurl_error error;
    char url[128];
    CURL *easy;
    CURLcode result;

    start_server(&server, serve_upload);
    memset(&observation, 0, sizeof(observation));
    memset(&source, 0, sizeof(source));
    memset(&upload, 0, sizeof(upload));
    upload.read = upload_read;
    upload.userdata = &source;
    upload.content_length = 8u;
    upload.known_length = 1;
    PROXY_CHECK(snprintf(url, sizeof(url), "http://127.0.0.1:%u/base",
                         (unsigned)server.port) > 0);
    easy = curl_easy_init();
    PROXY_CHECK(easy != NULL);
    upload.content_length = (uint64_t)-1;
    PROXY_CHECK(knurl_proxy_http_upstream_init(
                    &upstream, easy, url, "/upload", "GET", NULL, 8192u, 5000L,
                    10000L, &upload, ready, &observation,
                    &error) == KNURL_INVALID);
    upload.content_length = 8u;
    PROXY_CHECK(knurl_proxy_http_upstream_init(
                    &upstream, easy, url, "/upload", "GET", NULL, 8192u, 5000L,
                    10000L, &upload, ready, &observation, &error) == KNURL_OK);
    result = curl_easy_perform(easy);
    PROXY_CHECK(result == CURLE_OK);
    PROXY_CHECK(source.offset == 8u);
    PROXY_CHECK(upstream.response.status == 200);
    PROXY_CHECK(observation.final == 1u && observation.body == 1u);
    PROXY_CHECK(knurl_proxy_http_upstream_finish(&upstream, result, NULL) ==
                KNURL_PROXY_HEADER_OK);
    knurl_proxy_http_upstream_cleanup(&upstream);
    curl_easy_cleanup(easy);
    PROXY_CHECK(pthread_join(server.thread, NULL) == 0);
    PROXY_CHECK(close(server.listener) == 0);
}

static void *serve_head(void *userdata)
{
    static const char response[] =
        "HTTP/1.1 304 Not Modified\r\nContent-Length: 123\r\n"
        "Connection: close\r\n\r\n";
    struct stream_server *server;
    char request[1024];
    size_t used;
    ssize_t got;
    int fd;

    server = (struct stream_server *)userdata;
    fd = accept(server->listener, NULL, NULL);
    PROXY_CHECK(fd >= 0);
    used = 0u;
    do {
        got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
        PROXY_CHECK(got > 0);
        used += (size_t)got;
        request[used] = '\0';
    } while (strstr(request, "\r\n\r\n") == NULL &&
             used < sizeof(request) - 1u);
    PROXY_CHECK(strstr(request, "HEAD /head HTTP/1.1\r\n") != NULL);
    send_all(fd, response, sizeof(response) - 1u);
    PROXY_CHECK(close(fd) == 0);
    return NULL;
}

static void test_head(void)
{
    struct stream_server server;
    struct stream_observation observation;
    knurl_proxy_http_upstream upstream;
    knurl_error error;
    const char *reason;
    char url[128];
    CURL *easy;
    CURLcode result;
    size_t length;

    start_server(&server, serve_head);
    memset(&observation, 0, sizeof(observation));
    PROXY_CHECK(snprintf(url, sizeof(url), "http://127.0.0.1:%u/original",
                         (unsigned)server.port) > 0);
    easy = curl_easy_init();
    PROXY_CHECK(easy != NULL);
    PROXY_CHECK(knurl_proxy_http_upstream_init(
                    &upstream, easy, url, "/head", "HEAD", NULL, 8192u, 5000L,
                    10000L, NULL, ready, &observation, &error) == KNURL_OK);
    PROXY_CHECK(upstream.body_storage == NULL);
    result = curl_easy_perform(easy);
    PROXY_CHECK(result == CURLE_OK);
    PROXY_CHECK(observation.final == 1u && observation.body == 0u);
    PROXY_CHECK(upstream.response.status == 304 &&
                !upstream.response.body_allowed);
    PROXY_CHECK(upstream.response.content_length == 123u);
    PROXY_CHECK(upstream.body_storage == NULL);
    PROXY_CHECK(knurl_proxy_http_upstream_body(&upstream, &length) == NULL &&
                length == 0u);
    reason = NULL;
    PROXY_CHECK(knurl_proxy_http_upstream_finish(&upstream, result, &reason) ==
                KNURL_PROXY_HEADER_OK);
    knurl_proxy_http_upstream_cleanup(&upstream);
    curl_easy_cleanup(easy);
    PROXY_CHECK(pthread_join(server.thread, NULL) == 0);
    PROXY_CHECK(close(server.listener) == 0);
}

static void *serve_head_upload(void *userdata)
{
    static const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 123\r\n"
                                   "Connection: keep-alive\r\n\r\n";
    struct stream_server *server;
    struct timeval timeout;
    char request[1024];
    char *body;
    size_t used;
    size_t body_length;
    ssize_t got;
    int fd;

    server = (struct stream_server *)userdata;
    fd = accept(server->listener, NULL, NULL);
    PROXY_CHECK(fd >= 0);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    PROXY_CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout)) == 0);
    used = 0u;
    body = NULL;
    while (body == NULL && used < sizeof(request) - 1u) {
        got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
        PROXY_CHECK(got > 0);
        used += (size_t)got;
        request[used] = '\0';
        body = strstr(request, "\r\n\r\n");
    }
    PROXY_CHECK(body != NULL);
    PROXY_CHECK(strstr(request, "HEAD /head-upload HTTP/1.1\r\n") != NULL);
    PROXY_CHECK(strstr(request, "Content-Length: 8\r\n") != NULL);
    body += 4;
    body_length = used - (size_t)(body - request);
    while (body_length < 8u && used < sizeof(request) - 1u) {
        got = recv(fd, request + used, sizeof(request) - used - 1u, 0);
        PROXY_CHECK(got > 0);
        used += (size_t)got;
        body_length += (size_t)got;
    }
    PROXY_CHECK(body_length == 8u && memcmp(body, "abcdefgh", 8u) == 0);
    send_all(fd, response, sizeof(response) - 1u);
    /* A correct HEAD transfer completes without waiting for representation
     * bytes or the origin closing its keep-alive connection. */
    pause_origin(200000L);
    PROXY_CHECK(close(fd) == 0);
    return NULL;
}

static void test_head_upload(void)
{
    struct stream_server server;
    struct stream_observation observation;
    struct upload_source source;
    knurl_proxy_http_upload upload;
    knurl_proxy_http_upstream upstream;
    knurl_error error;
    char url[128];
    CURL *easy;
    CURLcode result;

    start_server(&server, serve_head_upload);
    memset(&observation, 0, sizeof(observation));
    memset(&source, 0, sizeof(source));
    memset(&upload, 0, sizeof(upload));
    upload.read = upload_read;
    upload.userdata = &source;
    upload.content_length = 8u;
    upload.known_length = 1;
    PROXY_CHECK(snprintf(url, sizeof(url), "http://127.0.0.1:%u/base",
                         (unsigned)server.port) > 0);
    easy = curl_easy_init();
    PROXY_CHECK(easy != NULL);
    PROXY_CHECK(knurl_proxy_http_upstream_init(
                    &upstream, easy, url, "/head-upload", "HEAD", NULL, 8192u,
                    5000L, 1000L, &upload, ready, &observation,
                    &error) == KNURL_OK);
    result = curl_easy_perform(easy);
    PROXY_CHECK(result == CURLE_WRITE_ERROR);
    PROXY_CHECK(source.offset == 8u);
    PROXY_CHECK(observation.final == 1u && observation.body == 0u);
    PROXY_CHECK(upstream.response.status == 200 &&
                !upstream.response.body_allowed);
    PROXY_CHECK(upstream.response.content_length == 123u);
    PROXY_CHECK(knurl_proxy_http_upstream_finish(&upstream, result, NULL) ==
                KNURL_PROXY_HEADER_OK);
    knurl_proxy_http_upstream_cleanup(&upstream);
    curl_easy_cleanup(easy);
    PROXY_CHECK(pthread_join(server.thread, NULL) == 0);
    PROXY_CHECK(close(server.listener) == 0);
}

static void ready(knurl_proxy_http_upstream *upstream,
                  knurl_proxy_http_event event, void *userdata)
{
    struct stream_observation *observation;

    (void)upstream;
    observation = (struct stream_observation *)userdata;
    if (event == KNURL_PROXY_HTTP_INFORMATIONAL)
        observation->informational++;
    else if (event == KNURL_PROXY_HTTP_FINAL)
        observation->final++;
    else if (event == KNURL_PROXY_HTTP_TRAILERS)
        observation->trailers++;
    else
        observation->body++;
}

int main(void)
{
    struct stream_server server;
    struct stream_observation observation;
    knurl_proxy_http_upstream upstream;
    knurl_error error;
    const unsigned char *body;
    const char *reason;
    char url[128];
    CURLM *multi;
    CURL *easy;
    CURLMsg *message;
    size_t length;
    size_t total_received;
    size_t first_length;
    size_t partial;
    size_t i;
    int running;
    int messages;
    int spins;

    test_framed_get_upload();

    start_server(&server, serve);
    memset(&observation, 0, sizeof(observation));
    PROXY_CHECK(snprintf(url, sizeof(url), "http://127.0.0.1:%u/original",
                         (unsigned)server.port) > 0);
    easy = curl_easy_init();
    multi = curl_multi_init();
    PROXY_CHECK(easy != NULL && multi != NULL);
    PROXY_CHECK(knurl_proxy_http_upstream_init(
                    &upstream, easy, url, "/rewritten?q=1&q=2", "GET", NULL,
                    8192u, 5000L, 10000L, NULL, ready, &observation,
                    &error) == KNURL_OK);
    PROXY_CHECK(upstream.body_storage == NULL);
    PROXY_CHECK(curl_multi_add_handle(multi, easy) == CURLM_OK);
    running = 1;
    for (spins = 0; spins < 500 && observation.body == 0u; ++spins) {
        PROXY_CHECK(curl_multi_perform(multi, &running) == CURLM_OK);
        PROXY_CHECK(curl_multi_poll(multi, NULL, 0u, 10, NULL) == CURLM_OK);
    }
    PROXY_CHECK(observation.informational == 1u);
    PROXY_CHECK(observation.final == 1u && observation.body == 1u);
    PROXY_CHECK(upstream.body_storage != NULL);
    PROXY_CHECK(running == 1);
    body = knurl_proxy_http_upstream_body(&upstream, &length);
    PROXY_CHECK(body != NULL && length != 0u && length <= 8192u);
    for (i = 0u; i < length; ++i)
        PROXY_CHECK(body[i] == 'A');
    total_received = length;
    first_length = length;
    server.send_second = 1;
    for (spins = 0; spins < 500 && !upstream.paused; ++spins) {
        PROXY_CHECK(curl_multi_perform(multi, &running) == CURLM_OK);
        PROXY_CHECK(curl_multi_poll(multi, NULL, 0u, 10, NULL) == CURLM_OK);
    }
    PROXY_CHECK(upstream.paused && running == 1);
    PROXY_CHECK(upstream.body_capacity <= 16384u);
    partial = first_length / 2u;
    knurl_proxy_http_upstream_consume(&upstream, partial);
    body = knurl_proxy_http_upstream_body(&upstream, &length);
    PROXY_CHECK(length == first_length - partial && body[0] == 'A');
    PROXY_CHECK(knurl_proxy_http_upstream_resume(&upstream, &error) ==
                KNURL_STATE);
    knurl_proxy_http_upstream_consume(&upstream, length);
    PROXY_CHECK(knurl_proxy_http_upstream_resume(&upstream, &error) ==
                KNURL_OK);
    for (spins = 0; spins < 500 && (running || total_received < 16384u);
         ++spins) {
        PROXY_CHECK(curl_multi_perform(multi, &running) == CURLM_OK);
        body = knurl_proxy_http_upstream_body(&upstream, &length);
        if (body != NULL) {
            PROXY_CHECK(total_received + length <= 16384u);
            for (i = 0u; i < length; ++i)
                PROXY_CHECK(body[i] ==
                            (total_received + i < 8192u ? 'A' : 'B'));
            total_received += length;
            knurl_proxy_http_upstream_consume(&upstream, length);
            if (upstream.paused)
                PROXY_CHECK(knurl_proxy_http_upstream_resume(
                                &upstream, &error) == KNURL_OK);
        }
        PROXY_CHECK(curl_multi_poll(multi, NULL, 0u, 10, NULL) == CURLM_OK);
    }
    PROXY_CHECK(total_received == 16384u && observation.body >= 2u);
    for (spins = 0; spins < 500 && running; ++spins) {
        PROXY_CHECK(curl_multi_perform(multi, &running) == CURLM_OK);
        PROXY_CHECK(curl_multi_poll(multi, NULL, 0u, 10, NULL) == CURLM_OK);
    }
    PROXY_CHECK(running == 0);
    message = curl_multi_info_read(multi, &messages);
    PROXY_CHECK(message != NULL && message->msg == CURLMSG_DONE &&
                message->data.result == CURLE_OK);
    PROXY_CHECK(upstream.response.body_bytes == 16384u);
    PROXY_CHECK(upstream.response.trailers.count == 1u);
    PROXY_CHECK(upstream.outbound_headers.count == 3u);
    reason = NULL;
    PROXY_CHECK(
        knurl_proxy_http_upstream_finish(&upstream, message->data.result,
                                         &reason) == KNURL_PROXY_HEADER_OK);
    PROXY_CHECK(observation.trailers == 1u);
    PROXY_CHECK(curl_multi_remove_handle(multi, easy) == CURLM_OK);
    knurl_proxy_http_upstream_cleanup(&upstream);
    curl_easy_cleanup(easy);
    curl_multi_cleanup(multi);
    PROXY_CHECK(pthread_join(server.thread, NULL) == 0);
    PROXY_CHECK(close(server.listener) == 0);
    test_head();
    test_head_upload();
    return 0;
}
