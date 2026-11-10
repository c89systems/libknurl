#define _POSIX_C_SOURCE 200809L
#include "vectis-services.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "Vectis services %d: %s\n", __LINE__, #t);         \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    struct reference_app app;
    struct reference_proxy_binding proxy[2];
    struct reference_upload_binding upload[3];
    struct reference_websocket_binding websocket;
    struct reference_webdav_binding dav, dav_hard;
    knurl_asset_provider *packed;
    pthread_t owner;
    char directory[96], cache[128], file[128];
    int origin, progress[2], first_response[2], failed, opened, finished,
        closed, ws_opened, ws_closed;
    unsigned int native_done, cancelled;
    int acquired, released, owner_read;
    int packed_opened, packed_closed, provider_closed;
    pid_t child, upstream;
    unsigned short port;
    int admission, hard_limit;
    int proxy_reports;
};
static int write_all(int fd, const void *bytes, size_t size)
{
    const char *p = bytes;
    ssize_t n;
    while (size) {
        n = write(fd, p, size);
        if (n <= 0)
            return 1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}
static void packed_retired(void *data)
{
    struct fixture *f = data;
    ++f->packed_closed;
}
static void provider_closed(void *data)
{
    struct fixture *f = data;
    ++f->provider_closed;
}
static knurl_status packed_open(const char *path, knurl_asset **out, void *data,
                                knurl_error *error)
{
    struct fixture *f = data;
    knurl_asset_info info;
    knurl_bytes bytes;
    if (strcmp(path, "/item"))
        return KNURL_EOF;
    memset(&info, 0, sizeof(info));
    info.kind = KNURL_ASSET_FILE;
    info.size.low = 4;
    info.etag = "\"packed\"";
    bytes.data = "pack";
    bytes.length = 4;
    ++f->packed_opened;
    return knurl_asset_memory(&info, bytes, packed_retired, f, out, error);
}
static int read_all(int fd, void *bytes, size_t size)
{
    char *p = bytes;
    ssize_t n;
    while (size) {
        n = read(fd, p, size);
        if (n <= 0)
            return 1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}
static int progress(int fd)
{
    struct pollfd p;
    char byte;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, 2000) == 1 && read(fd, &byte, 1) == 1 ? 0 : 1;
}
static int remove_tree(const char *path)
{
    struct stat info;
    DIR *dir;
    struct dirent *entry;
    char *child;
    size_t n;
    int failed = 0;
    if (lstat(path, &info) != 0)
        return 1;
    if (!S_ISDIR(info.st_mode))
        return unlink(path) != 0;
    dir = opendir(path);
    if (!dir)
        return 1;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        n = strlen(path) + strlen(entry->d_name) + 2;
        child = malloc(n);
        if (!child) {
            failed = 1;
            break;
        }
        (void)snprintf(child, n, "%s/%s", path, entry->d_name);
        failed = remove_tree(child);
        free(child);
        if (failed)
            break;
    }
    closedir(dir);
    return failed || rmdir(path) != 0;
}
static int connect_to(unsigned short port)
{
    struct sockaddr_in a;
    struct timeval timeout;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
        close(fd);
        return -1;
    }
    return fd;
}
static int response(int fd, int code, const char *body, const char *header)
{
    char bytes[8192];
    size_t used = 0;
    ssize_t n;
    char *payload, *length, *end;
    unsigned long content_length;
    while (used < sizeof(bytes) - 1) {
        n = read(fd, bytes + used, sizeof(bytes) - 1 - used);
        if (n < 0) {
            if (code == 413 && errno == ECONNRESET)
                break;
            fprintf(stderr, "response %d: read failed with errno %d\n", code,
                    errno);
            return 1;
        }
        if (!n)
            break;
        used += (size_t)n;
    }
    bytes[used] = 0;
    CHECK(used >= 12);
    if (atoi(bytes + 9) != code)
        fprintf(stderr, "expected %d, got %s\n", code, bytes);
    CHECK(atoi(bytes + 9) == code);
    payload = strstr(bytes, "\r\n\r\n");
    CHECK(payload != NULL);
    if (code == 413) {
        /* A rejected body can leave unread input and cause a terminal reset.
         * Require every declared response byte before accepting that close.
         */
        length = strstr(bytes, "\r\ncontent-length: ");
        CHECK(length != NULL && length < payload);
        length += sizeof("\r\ncontent-length: ") - 1;
        errno = 0;
        content_length = strtoul(length, &end, 10);
        CHECK(end > length && end <= payload && end[0] == '\r' &&
              end[1] == '\n' && errno != ERANGE);
        CHECK(content_length ==
              (unsigned long)(used - (size_t)(payload + 4 - bytes)));
    }
    CHECK(body == NULL || strstr(payload + 4, body) != NULL);
    CHECK(header == NULL || strstr(bytes, header) != NULL);
    return 0;
}
static int reset_response(const char *wire, int expected, int success)
{
    int pair[2], result;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    /* Closing with unread request bytes reproduces the terminal reset without
     * relying on TCP packet timing or a sleeping server process.
     */
    CHECK(!write_all(pair[0], "request", 7));
    CHECK(!write_all(pair[1], wire, strlen(wire)));
    CHECK(!close(pair[1]));
    result = response(pair[0], expected, NULL, NULL);
    CHECK(!close(pair[0]));
    CHECK((result == 0) == success);
    return 0;
}
static int exchange(unsigned short port, const char *method, const char *path,
                    const char *body, const char *headers, int code,
                    const char *expected, const char *required)
{
    char head[1024];
    int fd = connect_to(port);
    size_t size = strlen(body);
    CHECK(fd >= 0);
    (void)snprintf(head, sizeof(head),
                   "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: "
                   "close\r\nContent-Length: %lu\r\n%s\r\n",
                   method, path, (unsigned long)size, headers);
    CHECK(!write_all(fd, head, strlen(head)) && !write_all(fd, body, size));
    CHECK(!response(fd, code, expected, required));
    CHECK(!close(fd));
    return 0;
}
static reference_status preflight(const reference_proxy_inbound *in,
                                  reference_proxy_local_response *out,
                                  void *data, reference_error *error)
{
    (void)data;
    if (!strcmp(reference_proxy_inbound_path_param(in, "id"), "deny"))
        return reference_proxy_local_respond(out, 403, "denied", 6, error);
    return REFERENCE_OK;
}
static reference_status rewrite(const reference_proxy_inbound *in,
                                reference_proxy_outbound *out, void *data,
                                reference_error *error)
{
    (void)data;
    if (strcmp(reference_proxy_inbound_host(in), "localhost") ||
        strcmp(reference_proxy_inbound_query(in), "raw=%2f") ||
        reference_proxy_inbound_websocket(in) ||
        reference_proxy_inbound_method(in) != KNURL_HTTP_GET)
        return REFERENCE_STATE;
#define EDIT(call)                                                             \
    do {                                                                       \
        reference_status s = (call);                                           \
        if (s != REFERENCE_OK)                                                 \
            return s;                                                          \
    } while (0)
    EDIT(reference_proxy_outbound_select_target(out, 0, error));
    EDIT(reference_proxy_outbound_set_method(out, KNURL_HTTP_GET, error));
    EDIT(reference_proxy_outbound_set_path(out, "/upstream", error));
    EDIT(reference_proxy_outbound_set_query(out, "raw=%2f", error));
    EDIT(reference_proxy_outbound_set_host(out, "origin.test", error));
    EDIT(reference_proxy_outbound_add_header(out, "X-Policy", "first", error));
    EDIT(reference_proxy_outbound_set_header(out, "X-Policy", "retained",
                                             error));
    EDIT(reference_proxy_outbound_add_header(out, "X-Remove", "yes", error));
    EDIT(reference_proxy_outbound_remove_header(out, "X-Remove", error));
    return REFERENCE_OK;
}
static reference_status modify(reference_proxy_response *out, void *data,
                               reference_error *error)
{
    (void)data;
    if (reference_proxy_response_status(out) != 201)
        return REFERENCE_STATE;
    EDIT(reference_proxy_response_set_status(out, 202, error));
    EDIT(reference_proxy_response_add_header(out, "X-Discard", "x", error));
    EDIT(reference_proxy_response_remove_header(out, "X-Discard", error));
    EDIT(reference_proxy_response_set_header(out, "X-Adapter", "yes", error));
    return REFERENCE_OK;
}
static reference_status gateway(const reference_error *cause, int code,
                                reference_proxy_local_response *out, void *data,
                                reference_error *error)
{
    (void)data;
    if (cause->code == REFERENCE_OK || code != 502)
        return REFERENCE_STATE;
    return reference_proxy_local_respond(out, 529, "gateway", 7, error);
}
#undef EDIT
static reference_status upload_open(struct reference_app *app,
                                    struct reference_request *r, void *data,
                                    void **state, reference_error *error)
{
    struct fixture *f = data;
    (void)app;
    (void)error;
    if (strcmp(reference_request_path(r), "/upload"))
        return REFERENCE_STATE;
    *state = f;
    ++f->opened;
    return REFERENCE_OK;
}
static reference_status upload_write(struct reference_app *app,
                                     struct reference_request *r,
                                     const void *bytes, size_t size,
                                     void *state, void *data,
                                     reference_error *error)
{
    struct fixture *f = data;
    (void)app;
    (void)r;
    (void)error;
    if (state != f || !pthread_equal(f->owner, pthread_self()) || size != 3 ||
        memcmp(bytes, "abc", 3))
        return REFERENCE_STATE;
    return REFERENCE_OK;
}
static reference_status upload_finish(struct reference_app *app,
                                      struct reference_request *r,
                                      struct reference_response *reply,
                                      void *state, void *data,
                                      reference_error *error)
{
    struct fixture *f = data;
    (void)app;
    (void)r;
    (void)reply;
    (void)error;
    if (state != f)
        return REFERENCE_STATE;
    ++f->finished;
    return REFERENCE_OK; /* Vectis's implicit 204. */
}
static void upload_close(struct reference_app *app, struct reference_request *r,
                         void *state, void *data)
{
    struct fixture *f = data;
    (void)app;
    if (state != f || strcmp(reference_request_path(r), "/upload"))
        f->failed = 1;
    ++f->closed;
}
static reference_status reader(struct reference_app *app,
                               struct reference_request *r,
                               struct reference_source *source,
                               struct reference_response *reply, void *data,
                               reference_error *error)
{
    struct fixture *f = data;
    struct reference_source_error input = {0, NULL};
    char bytes[8];
    size_t n, total = 0;
    int signalled = 0;
    (void)app;
    (void)reply;
    (void)error;
    if (pthread_equal(f->owner, pthread_self()) ||
        strcmp(reference_request_header(r, "X-Reader"), "yes") ||
        reference_request_query_count(r) != 2 ||
        strcmp(reference_request_query(r, "x"), "first") ||
        strcmp(reference_request_ip(r), "127.0.0.1"))
        return REFERENCE_STATE;
    while ((n = source->read(source, bytes + total, sizeof(bytes) - total,
                             &input)) != 0) {
        total += n;
        if (!signalled) {
            if (write(f->progress[1], "r", 1) != 1)
                return REFERENCE_STATE;
            signalled = 1;
        }
    }
    /* Metadata is still valid after cancellation and HTTP request teardown. */
    if (strcmp(reference_request_param(r, "id"), "cancel") == 0) {
        struct timespec pause;
        int attempts = 0;
        pause.tv_sec = 0;
        pause.tv_nsec = 1000000;
        while (!reference_request_is_closed(r) && attempts++ < 1000)
            (void)nanosleep(&pause, NULL);
        if (!input.code ||
            strcmp(reference_request_header(r, "X-Reader"), "yes"))
            f->failed = 1;
        if (!reference_request_is_closed(r))
            f->failed = 1;
        __atomic_store_n(&f->cancelled, 1, __ATOMIC_RELEASE);
        (void)write(f->progress[1], "c", 1);
        return REFERENCE_STATE;
    }
    if (input.code || total != 6 || memcmp(bytes, "abcdef", 6))
        return REFERENCE_STATE;
    __atomic_store_n(&f->native_done, 1, __ATOMIC_RELEASE);
    return REFERENCE_OK;
}
static knurl_status acquire(knurl_request *request,
                            const knurl_upload_metadata *metadata, void *data,
                            void **out, knurl_error *error)
{
    struct fixture *f = data;
    knurl_bytes id;
    (void)error;
    if (!pthread_equal(f->owner, pthread_self()) ||
        metadata->path.length == 0 ||
        knurl_request_path_param(request, "id", &id, NULL) != KNURL_OK) {
        fprintf(stderr, "acquire owner=%d path=%lu\n",
                pthread_equal(f->owner, pthread_self()),
                (unsigned long)metadata->path.length);
        return KNURL_STATE;
    }
    ++f->acquired;
    if (id.length == 4 && !memcmp(id.data, "fail", 4))
        return KNURL_IO;
    if (id.length == 5 && !memcmp(id.data, "again", 5))
        return KNURL_AGAIN;
    *out = f;
    return KNURL_OK;
}
static void release(void *data)
{
    struct fixture *f = data;
    if (!pthread_equal(f->owner, pthread_self()) || !f->owner_read)
        f->failed = 1;
    ++f->released;
}
static knurl_status owner_read(const knurl_upload_metadata *metadata,
                               knurl_source *source, knurl_reply *reply,
                               void *data, knurl_error *error)
{
    struct fixture *f = data;
    char byte;
    size_t count;
    knurl_status status;
    (void)reply;
    status = knurl_source_read(source, &byte, 1, &count, error);
    if (pthread_equal(f->owner, pthread_self()) || metadata->path.length != 9 ||
        status != KNURL_OK || count != 1 || byte != 'x') {
        fprintf(stderr, "owner reader: status=%d count=%lu path=%lu error=%s\n",
                (int)status, (unsigned long)count,
                (unsigned long)metadata->path.length, error->message);
        return KNURL_STATE;
    }
    f->owner_read = 1;
    return KNURL_OK;
}
static reference_status dav_auth(const struct reference_webdav_auth_request *r,
                                 struct reference_webdav_auth_response *out,
                                 void *data, reference_error *error)
{
    (void)data;
    (void)error;
    if (strcmp(reference_request_header(r->request, "Authorization"),
               "Bearer host") ||
        strcmp(r->mount_path_prefix, "/dav"))
        return REFERENCE_STATE;
    if (!strcmp(r->resource_path, "/denied"))
        return REFERENCE_OK;
    if (!strcmp(r->resource_path, "/challenge")) {
        out->action = 2;
        strcpy(out->www_authenticate_value, "Basic realm=host");
        out->www_authenticate = out->www_authenticate_value;
        return REFERENCE_OK;
    }
    out->action = 1;
    strcpy(out->principal, "host-principal");
    return REFERENCE_OK;
}
static void ws_open(struct reference_app *app, reference_websocket *ws,
                    void *data)
{
    struct fixture *f = data;
    unsigned char *large = malloc(20000);
    reference_error error;
    (void)app;
    if (!large) {
        f->failed = 1;
        return;
    }
    memset(large, 'w', 20000);
    if (reference_websocket_send(ws, 0, NULL, 0, &error) != REFERENCE_OK ||
        reference_websocket_send(ws, 1, "\xc0\xaf", 2, &error) !=
            REFERENCE_OK ||
        reference_websocket_send(ws, 2, large, 20000, &error) != REFERENCE_OK ||
        reference_websocket_close(ws, &error) != REFERENCE_OK ||
        reference_websocket_send(ws, 1, "after-close", 11, &error) !=
            REFERENCE_OK)
        f->failed = 1;
    free(large);
    ++f->ws_opened;
}
static void ws_message(struct reference_app *app, reference_websocket *ws,
                       int op, const void *bytes, size_t size, void *data)
{
    struct fixture *f = data;
    reference_error error;
    (void)app;
    if (reference_websocket_send(ws, op, bytes, size, &error) != REFERENCE_OK)
        f->failed = 1;
}
static void ws_close(struct reference_app *app, reference_websocket *ws,
                     void *data)
{
    struct fixture *f = data;
    (void)app;
    (void)ws;
    ++f->ws_closed;
}
struct event_source {
    struct reference_source source;
    int done;
};
static size_t event_read(struct reference_source *source, void *bytes,
                         size_t size, struct reference_source_error *error)
{
    struct event_source *s = source->impl;
    knurl_sse_event event;
    size_t written;
    if (s->done++)
        return 0;
    knurl_sse_event_init(&event);
    event.has_data = 1;
    event.data.data = "hello";
    event.data.length = 5;
    if (knurl_sse_encode(&event, bytes, size, &written, NULL) != KNURL_OK) {
        error->code = 3;
        return 0;
    }
    return written;
}
static void event_close(struct reference_source *source) { free(source->impl); }
static reference_status route(struct reference_app *app,
                              struct reference_request *r,
                              struct reference_response *reply, void *data,
                              reference_error *error)
{
    struct fixture *f = data;
    struct event_source *source;
    if (reference_request_kore(r) == NULL)
        return REFERENCE_STATE;
    if (!strcmp(reference_request_path(r), "/events")) {
        source = calloc(1, sizeof(*source));
        if (!source)
            return REFERENCE_NOMEM;
        source->source.read = event_read;
        source->source.close = event_close;
        source->source.impl = source;
        return reference_response_stream_source(reply, 200, "text/event-stream",
                                                &source->source, error);
    }
    if (f->failed || f->proxy_reports != 2 || f->opened != 1 ||
        f->closed != 1 || f->finished != 1 || f->ws_opened != 1 ||
        f->ws_closed != 1 || f->acquired != 3 || f->released != 1 ||
        f->packed_opened != 4 || f->packed_closed != 4 ||
        f->provider_closed != 0 ||
        !__atomic_load_n(&f->native_done, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&f->cancelled, __ATOMIC_ACQUIRE))
        return REFERENCE_STATE;
    (void)knurl_server_request_stop(app->server, NULL);
    return reference_response_text(reply, 200, "text/plain", "ok", error);
}
static void proxy_report(struct reference_request *request,
                         const knurl_proxy_exchange *event, void *data)
{
    struct fixture *f = data;
    ++f->proxy_reports;
    if (!request || reference_request_is_closed(request))
        f->failed = 1;
    if (!strcmp(event->route, "/proxy/:id") &&
        (event->status != 202 || event->downstream_bytes.low != 7))
        f->failed = 1;
    if (!strcmp(event->route, "/dead") &&
        (event->status != 529 || strcmp(event->disconnect_side, "upstream")))
        f->failed = 1;
}
static int origin(int listener)
{
    static const char reply[] = "HTTP/1.1 201 Created\r\nContent-Length: "
                                "7\r\nConnection: close\r\n\r\nproxied";
    char head[2048];
    size_t used = 0;
    int fd = accept(listener, NULL, NULL);
    CHECK(fd >= 0);
    do {
        CHECK(used < sizeof(head) - 1 && read(fd, head + used, 1) == 1);
        head[++used] = 0;
    } while (!strstr(head, "\r\n\r\n"));
    CHECK(strstr(head, "GET /upstream?raw=%2f HTTP/1.1") &&
          strstr(head, "origin.test") && strstr(head, "retained") &&
          !strstr(head, "X-Remove"));
    CHECK(!write_all(fd, reply, sizeof(reply) - 1));
    close(fd);
    return 0;
}
static int websocket(unsigned short port)
{
    const char *request =
        "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: "
        "websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: "
        "13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    char head[512];
    unsigned char frame[8], chunk[512];
    size_t used = 0, left, i;
    int fd = connect_to(port);
    CHECK(fd >= 0 && !write_all(fd, request, strlen(request)));
    do {
        CHECK(used < sizeof(head) - 1 && read(fd, head + used, 1) == 1);
        head[++used] = 0;
    } while (!strstr(head, "\r\n\r\n"));
    CHECK(atoi(head + 9) == 101 && !read_all(fd, frame, 2) && frame[0] == 128 &&
          frame[1] == 0);
    CHECK(!read_all(fd, frame, 4) && frame[0] == 129 && frame[1] == 2 &&
          frame[2] == 0xc0 && frame[3] == 0xaf);
    CHECK(!read_all(fd, frame, 4) && frame[0] == 130 && frame[1] == 126 &&
          frame[2] * 256 + frame[3] == 20000);
    left = 20000;
    while (left) {
        used = left < sizeof(chunk) ? left : sizeof(chunk);
        CHECK(!read_all(fd, chunk, used));
        for (i = 0; i < used; ++i)
            CHECK(chunk[i] == 'w');
        left -= used;
    }
    CHECK(!read_all(fd, frame, 2) && frame[0] == 136 && frame[1] == 0);
    CHECK(!read_all(fd, frame, 2) && frame[0] == 129 && frame[1] == 11 &&
          !read_all(fd, chunk, 11) && !memcmp(chunk, "after-close", 11));
    frame[0] = 1;
    frame[1] = 130;
    memset(frame + 2, 0, 4);
    frame[6] = 0xc0;
    frame[7] = 0xaf;
    CHECK(!write_all(fd, frame, 8) && !read_all(fd, frame, 4) &&
          frame[0] == 129 && frame[1] == 2 && frame[2] == 0xc0 &&
          frame[3] == 0xaf);
    frame[0] = 136;
    frame[1] = 128;
    memset(frame + 2, 0, 4);
    CHECK(!write_all(fd, frame, 6) && !read_all(fd, frame, 2) &&
          frame[0] == 136 && frame[1] == 0);
    CHECK(!close(fd));
    return 0;
}
static int client(struct fixture *f, unsigned short port)
{
    char head[512];
    int fd;
    CHECK(!exchange(port, "GET", "/proxy/deny", "", "", 403, "denied", NULL));
    CHECK(!write_all(f->first_response[1], "r", 1));
    CHECK(!exchange(port, "GET", "/proxy/yes?raw=%2f", "", "", 202, "proxied",
                    "X-Adapter: yes"));
    CHECK(!exchange(port, "GET", "/dead", "", "", 529, "gateway", NULL));
    CHECK(!exchange(port, "POST", "/upload", "abc", "", 204, "", NULL));
    CHECK(!exchange(port, "POST", "/file", "abc", "", 200, "3", NULL));
    CHECK(!exchange(port, "GET", "/static-file", "", "", 200, "abc",
                    "content-type: text/x-host"));
    CHECK(!exchange(port, "GET", "/files", "", "", 404, "", NULL));
    CHECK(!exchange(port, "GET", "/index?raw=%2f", "", "", 308, "",
                    "location: /index/?raw=%2f"));
    CHECK(!exchange(port, "GET", "/index/", "", "", 200, "abc", NULL));
    CHECK(!exchange(port, "HEAD", "/index/", "", "", 200, "",
                    "content-length: 3"));
    CHECK(!exchange(port, "GET", "/static-file", "", "Range: bytes=1-2\r\n",
                    206, "bc", "content-range: bytes 1-2/3"));
    CHECK(!exchange(port, "GET", "/pack/item", "", "", 200, "pack",
                    "cache-control: no-cache"));
    CHECK(!exchange(port, "HEAD", "/pack/item", "", "", 200, "",
                    "content-length: 4"));
    CHECK(!exchange(port, "GET", "/pack/item", "", "Range: bytes=1-2\r\n", 206,
                    "ac", NULL));
    CHECK(!exchange(port, "GET", "/pack/item", "",
                    "If-None-Match: \"packed\"\r\n", 304, "", NULL));
    CHECK(!exchange(port, "GET", "/pack/missing", "", "", 404, "not found\n",
                    "cache-control: no-store"));
    CHECK(!exchange(port, "POST", "/owner/ok", "x", "", 204, "", NULL));
    CHECK(!exchange(port, "POST", "/owner/fail", "x", "", 500, "", NULL));
    CHECK(!exchange(port, "POST", "/owner/again", "x", "", 500, "", NULL));
    fd = connect_to(port);
    CHECK(fd >= 0);
    (void)snprintf(head, sizeof(head),
                   "POST /reader/ok?x=first&x=second HTTP/1.1\r\nHost: "
                   "localhost\r\nX-Reader: yes\r\nConnection: "
                   "close\r\nContent-Length: 6\r\n\r\nabc");
    CHECK(!write_all(fd, head, strlen(head)) && !progress(f->progress[0]));
    CHECK(!write_all(fd, "def", 3) && !response(fd, 204, "", NULL) &&
          !close(fd));
    fd = connect_to(port);
    CHECK(fd >= 0);
    (void)snprintf(head, sizeof(head),
                   "POST /reader/cancel?x=first&x=second HTTP/1.1\r\nHost: "
                   "localhost\r\nX-Reader: yes\r\nContent-Length: 9\r\n\r\na");
    CHECK(!write_all(fd, head, strlen(head)) && !progress(f->progress[0]) &&
          !close(fd));
    CHECK(!progress(f->progress[0]));
    CHECK(!exchange(port, "PUT", "/dav/item", "abc",
                    "Authorization: Bearer host\r\n", 201, "", NULL));
    CHECK(!exchange(port, "PUT", "/dav/oversized", "abcde",
                    "Authorization: Bearer host\r\n", 413, NULL, NULL));
    CHECK(!exchange(port, "GET", "/dav/item", "",
                    "Authorization: Bearer host\r\n", 200, "abc", NULL));
    CHECK(!exchange(port, "PROPFIND", "/dav", "",
                    "Authorization: Bearer host\r\nDepth: 1\r\n", 207, "item",
                    NULL));
    CHECK(!exchange(port, "GET", "/dav/denied", "",
                    "Authorization: Bearer host\r\n", 404, "", NULL));
    CHECK(!exchange(port, "GET", "/dav/challenge", "",
                    "Authorization: Bearer host\r\n", 401, "",
                    "Basic realm=host"));
    CHECK(!websocket(port));
    CHECK(!exchange(port, "GET", "/events", "", "", 200, "data: hello\n\n",
                    NULL));
    CHECK(!exchange(port, "GET", "/stop", "", "", 200, "ok", NULL));
    return 0;
}
static knurl_status started(knurl_server *server,
                            const knurl_process_context *process, void *data,
                            knurl_error *error)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    (void)error;
    f->owner = pthread_self();
    return KNURL_OK;
}
static int admission_client(struct fixture *f, unsigned short port)
{
    CHECK(!exchange(port, "PUT", "/dav/file", "abc", "", 201, "", NULL));
    CHECK(!exchange(port, "GET", "/dav/file", "", "", 200, "abc", NULL));
    CHECK(!exchange(port, "PUT", "/dav/ceiling", "abcd", "",
                    f->hard_limit ? 413 : 201, NULL, NULL));
    if (!f->hard_limit)
        CHECK(!exchange(port, "PUT", "/dav/oversized", "abcde", "", 413,
                        "request body exceeds route limit", NULL));
    return 0;
}
static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *data,
                          knurl_error *error)
{
    struct fixture *f = data;
    knurl_bound_listener listener;
    knurl_status status;
    (void)process;
    if (knurl_server_listener_at(server, 0, &listener, error) != KNURL_OK)
        return KNURL_STATE;
    status = knurl_server_set_serving(server, 1, error);
    if (status != KNURL_OK)
        return status;
    if (f->admission) {
        f->child = fork();
        if (!f->child) {
            int status = admission_client(f, listener.port);
            (void)kill(getppid(), SIGTERM);
            _exit(status);
        }
        return f->child > 0 ? KNURL_OK : KNURL_IO;
    }
    f->upstream = fork();
    if (!f->upstream)
        _exit(origin(f->origin));
    f->child = fork();
    if (!f->child)
        _exit(client(f, listener.port));
    if (f->upstream <= 0 || f->child <= 0)
        return KNURL_IO;
    /* Require a successful response while the ready callback is still active.
     * This makes premature client startup fail independently of scheduling. */
    if (progress(f->first_response[0])) {
        (void)kill(f->child, SIGKILL);
        (void)kill(f->upstream, SIGKILL);
        (void)waitpid(f->child, NULL, 0);
        (void)waitpid(f->upstream, NULL, 0);
        return KNURL_IO;
    }
    return KNURL_OK;
}
static int admission_case(int hard)
{
    struct fixture *f = calloc(1, sizeof(*f));
    struct reference_webdav_config dav;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_error error;
    reference_error translated;
    knurl_run_result result;
    char *cwd;
    int status;
    CHECK(f != NULL);
    f->admission = 1;
    f->hard_limit = hard;
    strcpy(f->directory, "dav-admission-XXXXXX");
    CHECK(mkdtemp(f->directory) && (cwd = getcwd(NULL, 0)));
    snprintf(f->cache, sizeof(f->cache), "%s/%s", cwd, f->directory);
    free(cwd);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.limits.default_body_bytes.low = 2;
    config.limits.hard_body_bytes.low = hard ? 3 : 8;
    reference_route_callbacks(&f->app, &config.callbacks);
    config.callbacks.on_parent_ready = ready;
    CHECK(knurl_server_new(&config, &f->app.server, &error) == KNURL_OK);
    memset(&dav, 0, sizeof(dav));
    dav.path_prefix = "/dav";
    dav.storage.cache_dir = f->cache;
    dav.storage.site_id = "admission";
    dav.storage.max_file_bytes = 4;
    dav.storage.max_total_bytes = 1024;
    dav.storage.max_resources = 16;
    CHECK(reference_webdav_register(&f->app, &dav, &f->dav, NULL,
                                    &translated) == REFERENCE_OK);
    CHECK(knurl_server_run(f->app.server, &result, &error) == KNURL_OK);
    CHECK(waitpid(f->child, &status, 0) == f->child && WIFEXITED(status) &&
          !WEXITSTATUS(status));
    CHECK(knurl_server_destroy(f->app.server, &error) == KNURL_OK);
    CHECK(!remove_tree(f->directory));
    free(f);
    return 0;
}
int main(void)
{
    struct fixture *f = calloc(1, sizeof(*f));
    struct sockaddr_in a;
    socklen_t length = sizeof(a);
    struct reference_proxy_config proxy;
    struct reference_upload_config upload;
    struct reference_websocket_config ws;
    struct reference_webdav_config dav;
    struct reference_route_config ordinary;
    struct reference_tls_config tls;
    struct reference_tls_binding translated_tls;
    struct reference_static_file_config static_file;
    struct reference_static_directory_config directory;
    struct reference_static_embedded_config embedded;
    struct reference_embedded_fs fs;
    knurl_asset_provider_config assets;
    knurl_server_config config, secure;
    knurl_listener_config listener;
    knurl_upload_reader_config owned_reader;
    knurl_run_result result;
    knurl_error error;
    reference_error translated;
    char target[80], *cwd;
    int status;
    size_t i;
    CHECK(f != NULL);
    CHECK(!reset_response("HTTP/1.1 413 Content Too Large\r\n"
                          "content-length: 4\r\n\r\nfull",
                          413, 1));
    CHECK(!reset_response("HTTP/1.1 413 Content Too Large\r\n"
                          "content-length: 5\r\n\r\nfull",
                          413, 0));
    CHECK(!reset_response("HTTP/1.1 413 Content Too Large\r\n"
                          "content-length: 3\r\n\r\nfull",
                          413, 0));
    CHECK(
        !reset_response("HTTP/1.1 413 Content Too Large\r\n\r\nfull", 413, 0));
    CHECK(!reset_response("HTTP/1.1 413 Content Too Large\r\n"
                          "content-length: 4\r\n",
                          413, 0));
#if defined(__linux__)
    CHECK(!reset_response("HTTP/1.1 200 OK\r\n"
                          "content-length: 4\r\n\r\nfull",
                          200, 0));
#endif
    CHECK(!admission_case(0) && !admission_case(1));
    strcpy(f->directory, "vectis-services-XXXXXX");
    CHECK(mkdtemp(f->directory));
    cwd = getcwd(NULL, 0);
    CHECK(cwd != NULL);
    (void)snprintf(f->cache, sizeof(f->cache), "%s/%s", cwd, f->directory);
    free(cwd);
    (void)snprintf(f->file, sizeof(f->file), "%s/uploaded", f->cache);
    CHECK(!pipe(f->progress) && !pipe(f->first_response));
    f->origin = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(f->origin >= 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(!bind(f->origin, (struct sockaddr *)&a, sizeof(a)) &&
          !listen(f->origin, 4) &&
          !getsockname(f->origin, (struct sockaddr *)&a, &length));
    (void)snprintf(target, sizeof(target), "http://127.0.0.1:%u",
                   (unsigned int)ntohs(a.sin_port));
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.io.output_queue_bytes = 128;
    config.io.chunk_bytes = 64;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    reference_route_callbacks(&f->app, &config.callbacks);
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_process_start = started;
    CHECK(knurl_server_new(&config, &f->app.server, &error) == KNURL_OK);
    memset(&static_file, 0, sizeof(static_file));
    static_file.path = "/static-file";
    static_file.file_path = f->file;
    static_file.content_type = "text/x-host";
    CHECK(reference_static_file(&f->app, &static_file, &translated) ==
          REFERENCE_OK);
    memset(&directory, 0, sizeof(directory));
    directory.path_prefix = "/files";
    directory.root_dir = f->cache;
    CHECK(reference_static_directory(&f->app, &directory, &translated) ==
          REFERENCE_OK);
    directory.path_prefix = "/index";
    directory.index_enabled = 1;
    directory.index_file = "uploaded";
    CHECK(reference_static_directory(&f->app, &directory, &translated) ==
          REFERENCE_OK);
    knurl_asset_provider_config_init(&assets);
    assets.open = packed_open;
    assets.close = provider_closed;
    assets.userdata = f;
    CHECK(knurl_asset_provider_new(&assets, &f->packed, &error) == KNURL_OK);
    fs.provider = f->packed;
    memset(&embedded, 0, sizeof(embedded));
    embedded.path_prefix = "/pack";
    embedded.fs = &fs;
    CHECK(reference_static_embedded(&f->app, &embedded, &translated) ==
          REFERENCE_OK);
    memset(&proxy, 0, sizeof(proxy));
    proxy.path = "/proxy/:id";
    proxy.path_kind = 1;
    proxy.target = target;
    proxy.preflight = preflight;
    proxy.rewrite = rewrite;
    proxy.modify_response = modify;
    proxy.on_exchange = proxy_report;
    proxy.on_exchange_userdata = f;
    CHECK(reference_proxy_register(&f->app, &proxy, &f->proxy[0],
                                   &translated) == REFERENCE_OK);
    memset(&proxy, 0, sizeof(proxy));
    proxy.path = "/dead";
    proxy.target = "http://127.0.0.1:1";
    proxy.on_error = gateway;
    proxy.on_exchange = proxy_report;
    proxy.on_exchange_userdata = f;
    CHECK(reference_proxy_register(&f->app, &proxy, &f->proxy[1],
                                   &translated) == REFERENCE_OK);
    memset(&upload, 0, sizeof(upload));
    upload.route.path = "/upload";
    upload.route.method = KNURL_HTTP_POST;
    upload.route.body.mode = 3;
    upload.route.body.max_bytes = 16;
    upload.open = upload_open;
    upload.write = upload_write;
    upload.finish = upload_finish;
    upload.close = upload_close;
    upload.userdata = f;
    CHECK(reference_upload_register(&f->app, &upload, &f->upload[0],
                                    &translated) == REFERENCE_OK);
    upload.route.path = "/file";
    CHECK(reference_upload_file_register(&f->app, &upload.route, f->file,
                                         &translated) == REFERENCE_OK);
    upload.route.path = "/reader/:id";
    upload.route.path_kind = 1;
    upload.reader = reader;
    CHECK(reference_upload_register(&f->app, &upload, &f->upload[1],
                                    &translated) == REFERENCE_OK);
    knurl_upload_reader_config_init(&owned_reader);
    owned_reader.method = KNURL_HTTP_POST;
    owned_reader.path = "/owner/:id";
    owned_reader.path_kind = KNURL_PATH_PARAMS;
    owned_reader.handler = owner_read;
    owned_reader.userdata = f;
    owned_reader.acquire = acquire;
    CHECK(knurl_router_upload_reader(knurl_server_router(f->app.server),
                                     &owned_reader, NULL,
                                     &error) == KNURL_INVALID);
    owned_reader.acquire = NULL;
    owned_reader.release = release;
    CHECK(knurl_router_upload_reader(knurl_server_router(f->app.server),
                                     &owned_reader, NULL,
                                     &error) == KNURL_INVALID);
    owned_reader.acquire = acquire;
    CHECK(knurl_router_upload_reader(knurl_server_router(f->app.server),
                                     &owned_reader, NULL, &error) == KNURL_OK);
    memset(&ws, 0, sizeof(ws));
    ws.path = "/ws";
    ws.connect = ws_open;
    ws.message = ws_message;
    ws.disconnect = ws_close;
    ws.userdata = f;
    CHECK(reference_websocket_register(&f->app, &ws, &f->websocket,
                                       &translated) == REFERENCE_OK);
    memset(&dav, 0, sizeof(dav));
    dav.path_prefix = "/dav";
    dav.storage.cache_dir = f->cache;
    dav.storage.site_id = "host";
    dav.storage.max_file_bytes = 4;
    dav.storage.max_total_bytes = 1024;
    dav.storage.max_resources = 128;
    dav.auth_required = dav.conceal_unauthorized = 1;
    dav.auth = dav_auth;
    CHECK(reference_webdav_register(&f->app, &dav, &f->dav, NULL,
                                    &translated) == REFERENCE_OK);
    dav.path_prefix = "/dav-hard";
    dav.storage.max_file_bytes = 32;
    CHECK(reference_webdav_register(&f->app, &dav, &f->dav_hard, NULL,
                                    &translated) == REFERENCE_OK);
    memset(&ordinary, 0, sizeof(ordinary));
    ordinary.method = KNURL_HTTP_GET;
    ordinary.handler = route;
    ordinary.userdata = f;
    ordinary.path = "/events";
    CHECK(reference_route_register(&f->app, &ordinary, &translated) ==
          REFERENCE_OK);
    ordinary.path = "/stop";
    CHECK(reference_route_register(&f->app, &ordinary, &translated) ==
          REFERENCE_OK);
    memset(&tls, 0, sizeof(tls));
    tls.mode = KNURL_TLS_MANUAL;
    tls.bind = "127.0.0.1";
    tls.port = 8443;
    tls.domain = "example.test";
    tls.http_redirect_enabled = 1;
    tls.presented_https_port = 9443;
    tls.version = KNURL_TLS_VERSION_1_2;
    knurl_server_config_init(&secure);
    CHECK(reference_tls_translate(&tls, &secure, &translated_tls,
                                  &translated) == REFERENCE_OK);
    CHECK(secure.listener_count == 2 && secure.listeners[1].port == 8080 &&
          secure.listeners[1].redirect_https_port == 9443 &&
          secure.tls.version == KNURL_TLS_VERSION_1_2 &&
          secure.tls.identity_count == 1 &&
          !strcmp(secure.tls.identities[0].domains[0], "example.test"));
    CHECK(knurl_server_run(f->app.server, &result, &error) == KNURL_OK);
    CHECK(waitpid(f->child, &status, 0) == f->child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(waitpid(f->upstream, &status, 0) == f->upstream &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(knurl_server_destroy(f->app.server, &error) == KNURL_OK);
    CHECK(f->provider_closed == 0);
    CHECK(knurl_asset_provider_close(f->packed, &error) == KNURL_OK &&
          f->provider_closed == 1);
    reference_route_cleanup(&f->app);
    close(f->origin);
    for (i = 0; i < 2; ++i) {
        close(f->progress[i]);
        close(f->first_response[i]);
    }
    unlink(f->file);
    CHECK(!remove_tree(f->directory));
    free(f);
    return 0;
}
