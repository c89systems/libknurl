#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <rotmos/lonejson.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            (void)fprintf(stderr, "static wire %d: %s\n", __LINE__, #t);       \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#include <sys/stat.h>
#include <fcntl.h>
struct fixture {
    pid_t child;
    char directory[64];
    int opened, closed;
};
static int stopped;
static rotmos_status stop(rotmos_request *request, rotmos_reply *reply,
                          void *data, rotmos_error *error)
{
    (void)data;
    stopped = 1;
    if (rotmos_server_request_stop(rotmos_request_server(request), error) !=
        ROTMOS_OK)
        return ROTMOS_STATE;
    return rotmos_reply_text(reply, 200, "text/plain", "stopped", error);
}
static rotmos_status ordinary(rotmos_request *request, rotmos_reply *reply,
                              void *data, rotmos_error *error)
{
    (void)request;
    (void)data;
    return rotmos_reply_text(reply, 200, "text/plain", "ordinary", error);
}
static rotmos_status before(rotmos_request *request, rotmos_reply *reply,
                            int *finish, void *data, rotmos_error *error)
{
    (void)request;
    (void)finish;
    (void)data;
    return rotmos_reply_header_set(reply, "X-Group", "yes", error);
}
static rotmos_status replace(rotmos_request *request, rotmos_reply *reply,
                             int *finish, void *data, rotmos_error *error)
{
    (void)request;
    (void)finish;
    (void)data;
    return rotmos_reply_text(reply, 201, "text/plain", "replaced", error);
}
static void closed(void *data)
{
    struct fixture *f = data;
    f->closed++;
}
static rotmos_status empty_list(rotmos_asset_entry *entry, void *data,
                                rotmos_error *error)
{
    (void)entry;
    (void)data;
    (void)error;
    return ROTMOS_EOF;
}
static rotmos_status virtual_read(rotmos_u64 offset, void *bytes,
                                  size_t capacity, size_t *produced, void *data,
                                  rotmos_error *error)
{
    (void)data;
    (void)error;
    if (offset.high != 1 || offset.low != 0 || capacity != 1)
        return ROTMOS_STATE;
    *(char *)bytes = 'v';
    *produced = 1;
    return ROTMOS_OK;
}
static rotmos_status packed_open(const char *path, rotmos_asset **out,
                                 void *data, rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_asset_config c;
    rotmos_status status;
    rotmos_bytes bytes;
    rotmos_asset_config_init(&c);
    c.close = closed;
    c.userdata = f;
    c.info.etag = "\"packed\"";
    c.info.last_modified = "Sun, 06 Nov 1994 08:49:37 GMT";
    if (strcmp(path, "/") == 0) {
        c.info.kind = ROTMOS_ASSET_DIRECTORY;
        c.next = empty_list;
        status = rotmos_asset_new(&c, out, error);
    } else if (strcmp(path, "/virtual") == 0) {
        c.info.size.high = 1;
        c.info.size.low = 16;
        c.read = virtual_read;
        status = rotmos_asset_new(&c, out, error);
    } else if (strcmp(path, "/picture.png") == 0 ||
               strcmp(path, "/data") == 0 || strcmp(path, "/index.html") == 0 ||
               strcmp(path, "/untyped.html") == 0) {
        bytes.data = strcmp(path, "/data") == 0 ? "abcdef" : "packed-index";
        bytes.length = strlen(bytes.data);
        rotmos_u64_from_size(bytes.length, &c.info.size, NULL);
        if (strcmp(path, "/index.html") == 0)
            c.info.content_type = "text/html";
        if (strcmp(path, "/picture.png") == 0)
            c.info.content_type = "image/png";
        status = rotmos_asset_memory(&c.info, bytes, closed, f, out, error);
    } else
        return ROTMOS_EOF;
    if (status == ROTMOS_OK)
        f->opened++;
    return status;
}
static int write_all(int fd, const void *bytes, size_t count)
{
    const unsigned char *p;
    ssize_t n;
    p = bytes;
    while (count != 0) {
        n = write(fd, p, count);
        CHECK(n > 0);
        p += n;
        count -= (size_t)n;
    }
    return 0;
}
static int read_all(int fd, void *bytes, size_t count)
{
    unsigned char *p;
    ssize_t n;
    p = bytes;
    while (count != 0) {
        n = read(fd, p, count);
        CHECK(n > 0);
        p += n;
        count -= (size_t)n;
    }
    return 0;
}
static int line(int fd, char *out, size_t bound)
{
    size_t used = 0;
    do {
        CHECK(used + 1 < bound && read_all(fd, out + used, 1) == 0);
        out[++used] = 0;
    } while (used < 2 || memcmp(out + used - 2, "\r\n", 2) != 0);
    return 0;
}
static int exchange(unsigned short port, const char *method, const char *path,
                    const char *headers, const char *body, int code,
                    const char *expected, const char *expected_header,
                    char *etag)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char metadata[4096], request[2048], result[1024], ending[2];
    char *length, *tag, *end;
    size_t used = 0, total = 0, count;
    ssize_t n;
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
          0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    snprintf(request, sizeof(request),
             "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: "
             "close\r\n%sContent-Length: %lu\r\n\r\n",
             method, path, headers ? headers : "",
             (unsigned long)(body != NULL ? strlen(body) : 16));
    CHECK(write_all(fd, request, strlen(request)) == 0);
    if (body != NULL)
        CHECK(write_all(fd, body, strlen(body)) == 0);
    metadata[0] = 0;
    do {
        CHECK(used + 1 < sizeof(metadata) &&
              read_all(fd, metadata + used, 1) == 0);
        metadata[++used] = 0;
    } while (strstr(metadata, "\r\n\r\n") == NULL);
    snprintf(request, sizeof(request), " %d ", code);
    if (strstr(metadata, request) == NULL)
        fprintf(stderr, "%s: %s\n", path, metadata);
    CHECK(strstr(metadata, request) != NULL);
    if (expected_header != NULL) {
        if (strstr(metadata, expected_header) == NULL)
            fprintf(stderr, "%s headers: %s\n", path, metadata);
        CHECK(strstr(metadata, expected_header) != NULL);
    }
    if (etag != NULL) {
        tag = strstr(metadata, "etag: ");
        CHECK(tag != NULL);
        tag += 6;
        end = strstr(tag, "\r\n");
        CHECK(end != NULL && (size_t)(end - tag) < 256);
        memcpy(etag, tag, (size_t)(end - tag));
        etag[end - tag] = 0;
    }
    if (strcmp(method, "HEAD") == 0)
        count = 0;
    else if (strstr(metadata, "transfer-encoding: chunked") != NULL) {
        do {
            CHECK(line(fd, request, sizeof(request)) == 0);
            count = (size_t)strtoul(request, NULL, 16);
            CHECK(count < sizeof(result) - total);
            if (count == 0) {
                CHECK(line(fd, request, sizeof(request)) == 0 &&
                      strcmp(request, "\r\n") == 0);
                break;
            }
            CHECK(read_all(fd, result + total, count) == 0 &&
                  read_all(fd, ending, 2) == 0 &&
                  memcmp(ending, "\r\n", 2) == 0);
            total += count;
        } while (1);
        count = 0;
    } else {
        length = strstr(metadata, "content-length: ");
        count = length != NULL ? (size_t)strtoul(length + 16, NULL, 10) : 0;
    }
    CHECK(count < sizeof(result) - total &&
          read_all(fd, result + total, count) == 0);
    total += count;
    result[total] = 0;
    if (strcmp(result, expected) != 0)
        fprintf(stderr, "%s body: %s\n", path, result);
    CHECK(strcmp(result, expected) == 0);
    n = read(fd, ending, 1);
    CHECK(n == 0);
    close(fd);
    return 0;
}
static int client(unsigned short port)
{
    char etag[256], headers[1024];
    CHECK(exchange(port, "GET", "/files/song.MP3", "Range: bytes=2-4\r\n", "",
                   206, "cde", "content-type: audio/mpeg", NULL) == 0);
    CHECK(exchange(port, "HEAD", "/files/movie.mp4", NULL, "", 200, "",
                   "content-type: video/mp4", NULL) == 0);
    CHECK(exchange(port, "GET", "/files/data.global", NULL, "", 200, "abcdef",
                   "content-type: application/x-instance", NULL) == 0);
    CHECK(exchange(
              port, "GET", "/mime-route/data.global", NULL, "", 200, "abcdef",
              "content-type: application/x-route; note=\"a;b,c\"", NULL) == 0);
    CHECK(exchange(port, "GET", "/mime-route/song.MP3", NULL, "", 200, "abcdef",
                   "content-type: audio/mpeg", NULL) == 0);
    CHECK(exchange(port, "GET", "/mime-explicit/data.global", NULL, "", 200,
                   "abcdef", "content-type: application/x-explicit",
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/files/data.txt", NULL, "", 200, "abcdef",
                   "content-type: text/plain; charset=utf-8", etag) == 0);
    CHECK(exchange(port, "GET", "/files/README.MD", NULL, "", 200, "# Readme\n",
                   "content-type: text/markdown; charset=utf-8", NULL) == 0);
    CHECK(exchange(port, "HEAD", "/files/guide.MarkDown", NULL, "", 200, "",
                   "content-type: text/markdown; charset=utf-8", NULL) == 0);
    CHECK(exchange(port, "GET", "/markdown", NULL, "", 200, "# Readme\n",
                   "content-type: text/markdown; charset=utf-8", NULL) == 0);
    CHECK(exchange(port, "GET", "/markdown-override", NULL, "", 200,
                   "# Readme\n", "content-type: text/plain; charset=us-ascii",
                   NULL) == 0);
    CHECK(exchange(port, "HEAD", "/files/data.txt", "Range: bytes=1-3\r\n", "",
                   200, "", "content-length: 6", NULL) == 0);
    CHECK(exchange(port, "GET", "/files/data.txt", "Range: bytes=1-3\r\n", "",
                   206, "bcd", "content-range: bytes 1-3/6", NULL) == 0);
    CHECK(exchange(port, "GET", "/files/data.txt", "Range: bytes=-2\r\n", "",
                   206, "ef", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/files/data.txt", "Range: bytes=4-\r\n", "",
                   206, "ef", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/files/data.txt", "Range: bytes=6-\r\n", "",
                   416, "", "content-range: bytes */6", NULL) == 0);
    CHECK(exchange(port, "GET", "/files/data.txt", "Range: bytes=0-1,3-4\r\n",
                   "", 416, "", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/files/data.txt", "Range: bytes=bad\r\n", "",
                   416, "", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/files/data.txt", "Range: items=1-3\r\n", "",
                   200, "abcdef", NULL, NULL) == 0);
    snprintf(headers, sizeof(headers), "If-None-Match: %s\r\n", etag);
    CHECK(exchange(port, "GET", "/files/data.txt", headers, "", 304, "", NULL,
                   NULL) == 0);
    snprintf(headers, sizeof(headers), "If-Match: %s\r\n", etag);
    CHECK(exchange(port, "GET", "/files/data.txt", headers, "", 412, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/files/data.txt", "If-Match: *\r\n", "", 200,
                   "abcdef", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/files/data.txt",
                   "If-None-Match: \"different\", bad\r\n", "", 400, "", NULL,
                   NULL) == 0);
    snprintf(headers, sizeof(headers), "Range: bytes=1-3\r\nIf-Range: %s\r\n",
             etag);
    CHECK(exchange(port, "GET", "/files/data.txt", headers, "", 200, "abcdef",
                   NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/files?a=%2F", NULL, "", 308, "",
                   "location: /files/?a=%2F", NULL) == 0);
    CHECK(exchange(port, "GET", "/files/", NULL, "", 200, "index", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/files/sub", NULL, "", 308, "",
                   "location: /files/sub/", NULL) == 0);
    CHECK(exchange(port, "GET", "/files/sub/", NULL, "", 200, "sub-index", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/files/a%20b", NULL, "", 308, "",
                   "location: /files/a%20b/", NULL) == 0);
    CHECK(exchange(port, "GET", "/bare/sub/", NULL, "", 404, "", NULL, NULL) ==
          0);
    CHECK(exchange(port, "GET", "/files/data.txt/", NULL, "", 404, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/files//", NULL, "", 400, "", NULL, NULL) ==
          0);
    CHECK(exchange(port, "GET", "/files/%2e%2e/secret", NULL, "", 400, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/files/%252e%252e/secret", NULL, "", 400, "",
                   NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/files/sub%2findex.html", NULL, "", 400, "",
                   NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/files/link", NULL, "", 404, "", NULL, NULL) ==
          0);
    CHECK(exchange(port, "GET", "/files/escape/secret", NULL, "", 404, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/ordinary/", NULL, "", 400, "", NULL, NULL) ==
          0);
    CHECK(exchange(port, "POST", "/files/data.txt", "Expect: 100-continue\r\n",
                   NULL, 405, "", "allow: GET, HEAD", NULL) == 0);
    CHECK(exchange(port, "POST", "/one", NULL, "", 200, "ordinary", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/one", NULL, "", 200, "abcdef", NULL, NULL) ==
          0);
    CHECK(exchange(port, "GET", "/replaced", NULL, "", 201, "replaced", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/packed/data",
                   "If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n", "",
                   304, "", "last-modified: Sun, 06 Nov 1994 08:49:37 GMT",
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/packed/data",
                   "If-Modified-Since: invalid\r\n", "", 200, "abcdef", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/packed/data",
                   "If-None-Match: \"different\"\r\nIf-Modified-Since: Sun, 06 "
                   "Nov 1994 08:49:37 GMT\r\n",
                   "", 200, "abcdef", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/packed/data",
                   "Range: bytes=1-3\r\nIf-Range: \"packed\"\r\n", "", 206,
                   "bcd", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/packed/data", "If-Match: \"packed\", bad\r\n",
                   "", 400, "", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/packed/data",
                   "If-None-Match: W/\"packed\"\r\n", "", 304, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/packed-raw/untyped.html", NULL, "", 200,
                   "packed-index", "content-type: text/html; charset=utf-8",
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/packed-raw/index.html", NULL, "", 200,
                   "packed-index", "content-type: text/html", NULL) == 0);
    CHECK(exchange(port, "GET", "/packed-raw/picture.png", NULL, "", 200,
                   "packed-index", "content-type: image/x-instance",
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/packed-mime/picture.png", NULL, "", 200,
                   "packed-index", "content-type: image/x-route", NULL) == 0);
    CHECK(exchange(port, "GET", "/packed/virtual",
                   "Range: bytes=4294967296-4294967296\r\n", "", 206, "v",
                   "content-range: bytes 4294967296-4294967296/4294967312",
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/packed", NULL, "", 308, "",
                   "location: /packed/", NULL) == 0);
    CHECK(exchange(port, "GET", "/packed/", NULL, "", 200, "packed-index",
                   "content-type: text/plain", NULL) == 0);
    CHECK(exchange(port, "GET", "/packed/missing", NULL, "", 404, "not found\n",
                   "cache-control: no-store", NULL) == 0);
    CHECK(exchange(port, "GET", "/g/assets/data", NULL, "", 200, "abcdef",
                   "X-Group: yes", NULL) == 0);
    CHECK(exchange(port, "GET", "/g", NULL, "", 308, "", "location: /g/",
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/g/", NULL, "", 200, "index", NULL, NULL) ==
          0);
    CHECK(exchange(port, "GET", "/stop", NULL, "", 200, "stopped", NULL,
                   NULL) == 0);
    return 0;
}
static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *p, void *data,
                           rotmos_error *error)
{
    struct fixture *f;
    rotmos_bound_listener listener;
    int result;
    (void)p;
    f = data;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    f->child = fork();
    if (f->child == 0) {
        (void)signal(SIGTERM, SIG_DFL);
        result = client(listener.port);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    return f->child < 0 ? ROTMOS_IO : ROTMOS_OK;
}
static int worker_exit(rotmos_server *server, const rotmos_process_context *p,
                       void *data)
{
    (void)server;
    (void)p;
    return stopped && ((struct fixture *)data)->opened ==
                          ((struct fixture *)data)->closed
               ? 0
               : 1;
}
static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t n = strlen(text);
    if (fd < 0)
        return 0;
    if (write(fd, text, n) != (ssize_t)n) {
        close(fd);
        return 0;
    }
    return close(fd) == 0;
}
int main(void)
{
    static const rotmos_mime_type instance_types[] = {
        {".GLOBAL", "application/x-instance"}, {"png", "image/x-instance"}};
    rotmos_mime_type route_types[2];
    char copied_type[] = "application/x-route; note=\"a;b,c\"";
    struct fixture fixture;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_static_config statics;
    rotmos_route_config route;
    rotmos_route_group_config group_config;
    rotmos_route_group *group;
    rotmos_server *server;
    rotmos_asset_provider *provider;
    rotmos_asset_provider_config packed;
    rotmos_error error;
    rotmos_run_result result;
    rotmos_status status;
    int child_status;
    char root[256], file[512], path[512], sub[512], space[512], secret[256];
    char markdown[512];
    memset(&fixture, 0, sizeof(fixture));
    fixture.child = -1;
    strcpy(fixture.directory, "static-fixture-XXXXXX");
    CHECK(mkdtemp(fixture.directory) != NULL);
    snprintf(root, sizeof(root), "%s/root", fixture.directory);
    CHECK(mkdir(root, 0700) == 0);
    snprintf(file, sizeof(file), "%s/data.txt", root);
    CHECK(put(file, "abcdef"));
    snprintf(markdown, sizeof(markdown), "%s/README.MD", root);
    CHECK(put(markdown, "# Readme\n"));
    snprintf(path, sizeof(path), "%s/guide.MarkDown", root);
    CHECK(put(path, "# Guide\n"));
    snprintf(path, sizeof(path), "%s/song.MP3", root);
    CHECK(put(path, "abcdef"));
    snprintf(path, sizeof(path), "%s/movie.mp4", root);
    CHECK(put(path, "abcdef"));
    snprintf(path, sizeof(path), "%s/data.global", root);
    CHECK(put(path, "abcdef"));
    snprintf(path, sizeof(path), "%s/index.html", root);
    CHECK(put(path, "index"));
    snprintf(sub, sizeof(sub), "%s/sub", root);
    CHECK(mkdir(sub, 0700) == 0);
    snprintf(path, sizeof(path), "%s/sub/index.html", root);
    CHECK(put(path, "sub-index"));
    snprintf(space, sizeof(space), "%s/a b", root);
    CHECK(mkdir(space, 0700) == 0);
    snprintf(path, sizeof(path), "%s/a b/index.html", root);
    CHECK(put(path, "space-index"));
    snprintf(secret, sizeof(secret), "%s/secret", fixture.directory);
    CHECK(put(secret, "outside"));
    snprintf(path, sizeof(path), "%s/link", root);
    CHECK(symlink("../secret", path) == 0);
    snprintf(path, sizeof(path), "%s/escape", root);
    CHECK(symlink("..", path) == 0);
    signal(SIGPIPE, SIG_IGN);
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.io.output_queue_bytes = 4096;
    config.io.chunk_bytes = 128;
    config.http.mime_types = instance_types;
    config.http.mime_type_count = 2;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_exit = worker_exit;
    config.callbacks.userdata = &fixture;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    rotmos_asset_provider_config_init(&packed);
    packed.open = packed_open;
    packed.userdata = &fixture;
    CHECK(rotmos_asset_provider_new(&packed, &provider, &error) == ROTMOS_OK);
    rotmos_route_config_init(&route);
    route.path = "^/ordinary/.*$";
    route.path_kind = ROTMOS_PATH_REGEX;
    route.handler = ordinary;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    rotmos_static_config_init(&statics);
    route_types[0].extension = ".gLoBaL";
    route_types[0].content_type = copied_type;
    route_types[1].extension = "PNG";
    route_types[1].content_type = "image/x-route";
    statics.path = "/mime-route";
    statics.root_dir = root;
    statics.mime_types = route_types;
    statics.mime_type_count = 2;
    CHECK(rotmos_router_static_directory(rotmos_server_router(server), &statics,
                                         NULL, &error) == ROTMOS_OK);
    statics.path = "/mime-explicit";
    statics.content_type = "application/x-explicit";
    CHECK(rotmos_router_static_directory(rotmos_server_router(server), &statics,
                                         NULL, &error) == ROTMOS_OK);
    statics.path = "/packed-mime";
    statics.provider = provider;
    statics.content_type = NULL;
    CHECK(rotmos_router_static_assets(rotmos_server_router(server), &statics,
                                      NULL, &error) == ROTMOS_OK);
    copied_type[0] = 'X';
    statics.mime_types = NULL;
    statics.mime_type_count = 0;
    statics.path = "/ordinary";
    statics.root_dir = root;
    statics.index_enabled = 1;
    CHECK(rotmos_router_static_directory(rotmos_server_router(server), &statics,
                                         NULL, &error) == ROTMOS_OK);
    statics.path = "/markdown";
    statics.file_path = markdown;
    CHECK(rotmos_router_static_file(rotmos_server_router(server), &statics,
                                    NULL, &error) == ROTMOS_OK);
    statics.path = "/markdown-override";
    statics.content_type = "text/plain; charset=us-ascii";
    CHECK(rotmos_router_static_file(rotmos_server_router(server), &statics,
                                    NULL, &error) == ROTMOS_OK);
    statics.content_type = NULL;
    statics.path = "/files/";
    CHECK(rotmos_router_static_directory(rotmos_server_router(server), &statics,
                                         NULL, &error) == ROTMOS_OK);
    statics.path = "/bare";
    statics.index_enabled = 0;
    CHECK(rotmos_router_static_directory(rotmos_server_router(server), &statics,
                                         NULL, &error) == ROTMOS_OK);
    statics.path = "/one";
    statics.file_path = file;
    CHECK(rotmos_router_static_file(rotmos_server_router(server), &statics,
                                    NULL, &error) == ROTMOS_OK);
    route.path = "/one";
    route.path_kind = ROTMOS_PATH_LITERAL;
    route.method = ROTMOS_HTTP_POST;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    statics.path = "/replaced";
    statics.hooks.after = replace;
    CHECK(rotmos_router_static_file(rotmos_server_router(server), &statics,
                                    NULL, &error) == ROTMOS_OK);
    statics.hooks.after = NULL;
    statics.path = "/packed";
    statics.provider = provider;
    statics.index_enabled = 1;
    statics.content_type = "text/plain";
    CHECK(rotmos_router_static_assets(rotmos_server_router(server), &statics,
                                      NULL, &error) == ROTMOS_OK);
    statics.path = "/packed-raw";
    statics.content_type = NULL;
    CHECK(rotmos_router_static_assets(rotmos_server_router(server), &statics,
                                      NULL, &error) == ROTMOS_OK);
    statics.content_type = "text/plain";
    rotmos_route_group_config_init(&group_config);
    group_config.prefix = "/g";
    group_config.hooks.before = before;
    CHECK(rotmos_router_group(rotmos_server_router(server), &group_config,
                              &group, &error) == ROTMOS_OK);
    statics.path = "/assets";
    CHECK(rotmos_route_group_static_assets(group, &statics, NULL, &error) ==
          ROTMOS_OK);
    statics.path = "/";
    statics.content_type = NULL;
    CHECK(rotmos_route_group_static_directory(group, &statics, NULL, &error) ==
          ROTMOS_OK);
    route.path = "/stop";
    route.method = ROTMOS_HTTP_GET;
    route.handler = stop;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK) {
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
        if (fixture.child > 0) {
            kill(fixture.child, SIGTERM);
            waitpid(fixture.child, NULL, 0);
        }
        rotmos_server_destroy(server, NULL);
        return 1;
    }
    CHECK(!result.escalated && result.recovered_failures.low == 0 &&
          result.recovered_failures.high == 0);
    CHECK(waitpid(fixture.child, &child_status, 0) == fixture.child &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK &&
          rotmos_asset_provider_close(provider, &error) == ROTMOS_OK);
    CHECK(unlink(file) == 0);
    CHECK(unlink(markdown) == 0);
    snprintf(path, sizeof(path), "%s/song.MP3", root);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/movie.mp4", root);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/data.global", root);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/guide.MarkDown", root);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/index.html", root);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/sub/index.html", root);
    CHECK(unlink(path) == 0 && rmdir(sub) == 0);
    snprintf(path, sizeof(path), "%s/a b/index.html", root);
    CHECK(unlink(path) == 0 && rmdir(space) == 0);
    snprintf(path, sizeof(path), "%s/link", root);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/escape", root);
    CHECK(unlink(path) == 0);
    CHECK(unlink(secret) == 0 && rmdir(root) == 0 &&
          rmdir(fixture.directory) == 0);
    return 0;
}
