#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <rotmos/xml.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "dav %d: %s\n", __LINE__, #t);                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    rotmos_server *server;
    rotmos_asset_provider *assets;
    rotmos_storage_provider *readonly;
    pthread_t owner;
    pid_t child;
    char root[4096], cache[4096];
    int stopped, failed, opened, closed, checks, mapped, overwritten, hidden,
        after, aborted, after_failed;
};
static char *wire_body;
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
    char metadata[4096], request[2048], ending[2];
    char *result = wire_body;
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
            CHECK(count < 32768u - total);
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
    CHECK(count < 32768u - total && read_all(fd, result + total, count) == 0);
    total += count;
    result[total] = 0;
    if (expected != NULL && strcmp(result, expected) != 0)
        fprintf(stderr, "%s body: %s\n", path, result);
    CHECK(expected == NULL || strcmp(result, expected) == 0);
    n = read(fd, ending, 1);
    CHECK(n == 0);
    close(fd);
    return 0;
}
static const char *request_header(rotmos_request *request, const char *name)
{
    rotmos_bytes bytes;
    return rotmos_request_header_first(request, name, &bytes, NULL) == ROTMOS_OK
               ? bytes.data
               : NULL;
}
static rotmos_status auth(const rotmos_webdav_auth_request *request,
                          rotmos_webdav_auth_response *reply, void *data,
                          rotmos_error *error)
{
    struct fixture *f = data;
    const char *path = request->resource_path;
    if (!pthread_equal(f->owner, pthread_self())) {
        f->failed = 1;
        return ROTMOS_STATE;
    }
    ++f->checks;
    rotmos_request_set_userdata(request->request, f);
    reply->principal = "user";
    if (request_header(request->request, "X-Deny") != NULL) {
        reply->action = ROTMOS_WEBDAV_CHALLENGE;
        reply->www_authenticate = "Basic realm=\"fixture\"";
        reply->body.data = "authenticate";
        reply->body.length = 12;
    } else if (request_header(request->request, "X-Redirect") != NULL) {
        reply->action = ROTMOS_WEBDAV_REDIRECT;
        reply->location = "/login";
    } else if (strstr(path, "/private") != NULL ||
               strcmp(path, "/blocked/public") == 0) {
        if (strcmp(path, "/blocked/public") == 0)
            ++f->mapped;
        if (strcmp(path, "/victim/private") == 0 &&
            request->method == ROTMOS_HTTP_COPY)
            ++f->overwritten;
        if (request->method == ROTMOS_HTTP_PROPFIND) {
            ++f->hidden;
            reply->action = ROTMOS_WEBDAV_REDIRECT;
            reply->location = "/private-login";
        } else
            reply->action = ROTMOS_WEBDAV_DENY;
    } else {
        reply->action = ROTMOS_WEBDAV_ALLOW;
        if (strcmp(path, "/cancel/public") == 0 &&
            request->method == ROTMOS_HTTP_COPY) {
            ++f->aborted;
            (void)rotmos_response_abort(
                rotmos_request_response(request->request), ROTMOS_CANCELLED,
                NULL);
        }
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static rotmos_status before(rotmos_request *request, rotmos_reply *reply,
                            int *finish, void *data, rotmos_error *error)
{
    (void)request;
    (void)finish;
    (void)data;
    return rotmos_reply_header_set(reply, "X-Before", "yes", error);
}
static rotmos_status after(rotmos_request *request, rotmos_reply *reply,
                           int *finish, void *data, rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_bytes principal = rotmos_webdav_request_principal(request);
    (void)finish;
    if (rotmos_request_userdata(request) != f ||
        (principal.length != 0 &&
         (principal.length != 4 || memcmp(principal.data, "user", 4) != 0))) {
        f->failed = 1;
        return ROTMOS_STATE;
    }
    ++f->after;
    if (request_header(request, "X-After-Fail") != NULL) {
        if (rotmos_response_is_committed(rotmos_request_response(request)))
            f->failed = 1;
        ++f->after_failed;
        return ROTMOS_STATE;
    }
    return rotmos_reply_header_set(reply, "X-After", "yes", error);
}
static rotmos_status ping(rotmos_request *request, rotmos_reply *reply,
                          void *data, rotmos_error *error)
{
    struct fixture *f = data;
    if (strcmp(rotmos_request_path(request).data, "/stop") == 0) {
        f->stopped = 1;
        (void)rotmos_server_request_stop(rotmos_request_server(request), error);
    }
    return rotmos_reply_text(reply, 200, "text/plain", "ok", error);
}
struct cursor {
    struct fixture *f;
    int item;
};
static void memory_closed(void *data) { ++((struct fixture *)data)->closed; }
static void list_closed(void *data)
{
    struct cursor *c = data;
    ++c->f->closed;
    free(c);
}
static rotmos_status list_next(rotmos_asset_entry *entry, void *data,
                               rotmos_error *error)
{
    struct cursor *c = data;
    (void)error;
    if (c->item < 2) {
        entry->name = c->item++ ? "a %?\\ b.txt" : "a & b.txt";
        entry->info.size.low = 6;
        return ROTMOS_OK;
    }
    return ROTMOS_EOF;
}
static rotmos_status packed_open(const char *path, rotmos_asset **out,
                                 void *data, rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_asset_config config;
    rotmos_bytes bytes;
    rotmos_asset_info info;
    rotmos_status status;
    if (strcmp(path, "/") == 0) {
        struct cursor *c = calloc(1, sizeof(*c));
        if (c == NULL)
            return ROTMOS_NOMEM;
        c->f = f;
        rotmos_asset_config_init(&config);
        config.info.kind = ROTMOS_ASSET_DIRECTORY;
        config.next = list_next;
        config.close = list_closed;
        config.userdata = c;
        status = rotmos_asset_new(&config, out, error);
        if (status != ROTMOS_OK)
            free(c);
    } else if (strcmp(path, "/a & b.txt") == 0) {
        memset(&info, 0, sizeof(info));
        info.size.low = 6;
        info.etag = "\"packed\"";
        info.content_type = "text/plain";
        bytes.data = "packed";
        bytes.length = 6;
        status =
            rotmos_asset_memory(&info, bytes, memory_closed, f, out, error);
    } else
        return ROTMOS_EOF;
    if (status == ROTMOS_OK)
        ++f->opened;
    return status;
}
static int connect_to(unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    struct timeval timeout;
    if (fd < 0)
        return -1;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static int partial_file(const char *root, int expected)
{
    DIR *directory;
    struct dirent *entry;
    struct stat st;
    char path[4096];
    int i, found;
    for (i = 0; i < 1000; ++i) {
        directory = opendir(root);
        if (directory == NULL)
            return 0;
        found = 0;
        while ((entry = readdir(directory)) != NULL) {
            if (strncmp(entry->d_name, ".vectis-tmp-", 12) != 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
            if (stat(path, &st) == 0 && st.st_size == 3)
                found = 1;
        }
        closedir(directory);
        if (found == expected)
            return 1;
        (void)poll(NULL, 0, 1);
    }
    return 0;
}
static int streaming(unsigned short port, const char *root)
{
    int fd;
    char metadata[512];
    size_t used = 0;
    const char *start =
        "PUT /dav/upload HTTP/1.1\r\nHost: localhost\r\nConnection: "
        "close\r\nContent-Length: 6\r\n\r\nabc";
    fd = connect_to(port);
    CHECK(fd >= 0 && write_all(fd, start, strlen(start)) == 0);
    CHECK(partial_file(root, 1));
    CHECK(exchange(port, "GET", "/ping", NULL, "", 200, "ok", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/upload", NULL, "", 404, "", NULL, NULL) ==
          0);
    CHECK(write_all(fd, "def", 3) == 0);
    metadata[0] = 0;
    do {
        CHECK(used + 1 < sizeof(metadata) &&
              read_all(fd, metadata + used, 1) == 0);
        metadata[++used] = 0;
    } while (strstr(metadata, "\r\n\r\n") == NULL);
    CHECK(strstr(metadata, " 201 ") != NULL);
    close(fd);
    CHECK(exchange(port, "GET", "/dav/upload", NULL, "", 200, "abcdef", NULL,
                   NULL) == 0);
    CHECK(partial_file(root, 0));
    start = "PUT /dav/aborted HTTP/1.1\r\nHost: localhost\r\nConnection: "
            "close\r\nContent-Length: 6\r\n\r\nabc";
    fd = connect_to(port);
    CHECK(fd >= 0 && write_all(fd, start, strlen(start)) == 0 &&
          partial_file(root, 1));
    close(fd);
    CHECK(partial_file(root, 0));
    CHECK(exchange(port, "GET", "/dav/aborted", NULL, "", 404, "", NULL,
                   NULL) == 0);
    /* Interrupted replacement must preserve the previously committed value,
     * release the mutation lock and permit a later writer to finish. */
    start = "PUT /dav/upload HTTP/1.1\r\nHost: localhost\r\nConnection: "
            "close\r\nContent-Length: 6\r\n\r\nxyz";
    fd = connect_to(port);
    CHECK(fd >= 0 && write_all(fd, start, strlen(start)) == 0 &&
          partial_file(root, 1));
    CHECK(exchange(port, "GET", "/dav/upload", NULL, "", 200, "abcdef", NULL,
                   NULL) == 0);
    CHECK(close(fd) == 0 && partial_file(root, 0));
    CHECK(exchange(port, "GET", "/dav/upload", NULL, "", 200, "abcdef", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "PUT", "/dav/upload", NULL, "replacement", 204, "",
                   NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/upload", NULL, "", 200, "replacement",
                   NULL, NULL) == 0);
    return 0;
}
static int staged_count(const char *root, int expected)
{
    DIR *directory;
    struct dirent *entry;
    struct stat st;
    char path[4096];
    int tries, count;
    for (tries = 0; tries < 1000; ++tries) {
        count = 0;
        directory = opendir(root);
        CHECK(directory != NULL);
        while ((entry = readdir(directory)) != NULL) {
            if (strncmp(entry->d_name, ".vectis-tmp-", 12))
                continue;
            snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
            if (!stat(path, &st) && st.st_size == 3)
                ++count;
        }
        closedir(directory);
        if (count == expected)
            return 0;
        (void)poll(NULL, 0, 1);
    }
    fprintf(stderr, "expected %d staged uploads, got %d\n", expected, count);
    return 1;
}
static int concurrent_puts(unsigned short port, const char *root)
{
    /* Cross both former undocumented thresholds (16 PUTs, 256 resources)
     * under explicit server capacity. Native writes reaching the staging files
     * form the barriers; no fixed wait stands in for completed admission. */
    int sockets[257], i;
    char wire[256], path[64], line_bytes[256];
    for (i = 0; i < 257; ++i) {
        sockets[i] = connect_to(port);
        snprintf(wire, sizeof(wire),
                 "PUT /dav/parallel%d HTTP/1.1\r\n"
                 "Host: localhost\r\nConnection: close\r\nContent-Length: "
                 "6\r\n\r\nabc",
                 i);
        CHECK(sockets[i] >= 0 &&
              write_all(sockets[i], wire, strlen(wire)) == 0);
        CHECK(staged_count(root, i + 1) == 0);
    }
    for (i = 0; i < 257; ++i) {
        if (!(i % 2)) {
            CHECK(write_all(sockets[i], "def", 3) == 0);
            CHECK(line(sockets[i], line_bytes, sizeof(line_bytes)) == 0 &&
                  strstr(line_bytes, " 201 "));
        }
        CHECK(close(sockets[i]) == 0);
    }
    CHECK(staged_count(root, 0) == 0);
    for (i = 0; i < 257; ++i) {
        snprintf(path, sizeof(path), "/dav/parallel%d", i);
        CHECK(exchange(port, "GET", path, NULL, "", i % 2 ? 404 : 200,
                       i % 2 ? "" : "abcdef", NULL, NULL) == 0);
    }
    CHECK(exchange(port, "PUT", "/dav/reused", NULL, "z", 201, "", NULL,
                   NULL) == 0);
    return 0;
}
static int valid_xml(void)
{
    rotmos_xml_document *document;
    rotmos_error error;
    CHECK(rotmos_xml_document_parse(wire_body, strlen(wire_body), NULL,
                                    &document, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_document_close(document, &error) == ROTMOS_OK);
    return 0;
}
static int client(unsigned short port, const char *root)
{
    char etag[256], headers[512];
    wire_body = malloc(32768);
    CHECK(wire_body != NULL);
    CHECK(concurrent_puts(port, root) == 0);
    /* Async GET and synchronous OPTIONS must replace staged replies when the
     * after hook fails, without sending the selected body or aborting HTTP. */
    CHECK(exchange(port, "GET", "/dav/file", "X-After-Fail: yes\r\n", "", 500,
                   "", "content-length: 0", NULL) == 0);
    CHECK(exchange(port, "HEAD", "/dav/file", "X-After-Fail: yes\r\n", "", 500,
                   "", "content-length: 0", NULL) == 0);
    CHECK(exchange(port, "OPTIONS", "/dav", "X-After-Fail: yes\r\n", "", 500,
                   "", "content-length: 0", NULL) == 0);
    CHECK(exchange(port, "PROPFIND", "/dav/listing", "Depth: 1\r\n", "", 207,
                   NULL, NULL, NULL) == 0 &&
          valid_xml() == 0 &&
          strstr(wire_body, "/dav/listing/blocked") != NULL &&
          strstr(wire_body, "/dav/listing/a%20%25%3F%5C%20b.txt") != NULL);
    CHECK(exchange(port, "GET", "/dav/file", "X-Deny: yes\r\n", "", 401,
                   "authenticate", "www-authenticate: Basic", NULL) == 0);
    CHECK(exchange(port, "PUT", "/dav/file",
                   "X-Deny: yes\r\nExpect: 100-continue\r\n", NULL, 401,
                   "authenticate", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/file", "X-Redirect: yes\r\n", "", 302, "",
                   "location: /login", NULL) == 0);
    CHECK(exchange(port, "OPTIONS", "/dav", NULL, "", 204, "", "dav: 1",
                   NULL) == 0);
    CHECK(exchange(port, "POST", "/dav/file", "Expect: 100-continue\r\n", NULL,
                   405, "", "allow: OPTIONS", NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/file", NULL, "", 200, "abcdef",
                   "X-After: yes", etag) == 0);
    snprintf(headers, sizeof(headers), "If-None-Match: %s\r\n", etag);
    CHECK(exchange(port, "GET", "/dav/file", headers, "", 304, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/file", "Range: bytes=1-3\r\n", "", 206,
                   "bcd", "content-range: bytes 1-3/6", NULL) == 0);
    CHECK(exchange(port, "HEAD", "/dav/file", NULL, "", 200, "",
                   "content-length: 6", NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/", NULL, "", 404, "", NULL, NULL) == 0);
    CHECK(exchange(port, "MKCOL", "/dav/new", NULL, "bad", 415, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "MKCOL", "/dav/new", NULL, "", 201, "", NULL, NULL) ==
          0);
    CHECK(exchange(port, "PUT", "/dav/new/file", "If-None-Match: *\r\n",
                   "first", 201, "", NULL, NULL) == 0);
    CHECK(exchange(port, "PUT", "/dav/new/file", "If-None-Match: *\r\n",
                   "wrong", 412, "", NULL, NULL) == 0);
    CHECK(exchange(port, "PUT", "/dav/new/file", NULL, "second", 204, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/new/file", NULL, "", 200, "second", NULL,
                   NULL) == 0);
    CHECK(streaming(port, root) == 0);
    CHECK(exchange(port, "PUT", "/dav/zero", NULL, "", 201, "", NULL, NULL) ==
          0);
    CHECK(exchange(port, "GET", "/dav/zero", NULL, "", 200, "", NULL, NULL) ==
          0);
    {
        int fd = connect_to(port);
        char byte;
        const char *request =
            "COPY /dav/safe HTTP/1.1\r\nHost: localhost\r\nDestination: "
            "/dav/cancel\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        CHECK(fd >= 0 && write_all(fd, request, strlen(request)) == 0);
        CHECK(read(fd, &byte, 1) == 0);
        close(fd);
        CHECK(exchange(port, "GET", "/ping", NULL, "", 200, "ok", NULL, NULL) ==
              0);
    }
    CHECK(exchange(port, "PROPFIND", "/dav/tree", NULL, "", 403, NULL, NULL,
                   NULL) == 0 &&
          strstr(wire_body, "propfind-finite-depth") != NULL);
    CHECK(exchange(port, "PROPFIND", "/dav/tree", "Depth: 2\r\n", "", 400, "",
                   NULL, NULL) == 0);
    CHECK(exchange(port, "PROPFIND", "/dav/tree/", "Depth: 1\r\n", "", 207,
                   NULL, "X-After: yes", NULL) == 0);
    CHECK(valid_xml() == 0 && strstr(wire_body, "/dav/tree/public") != NULL &&
          strstr(wire_body, "private") == NULL);
    CHECK(exchange(port, "PROPFIND", "/dav/tree", "Depth: 0\r\n", "", 207, NULL,
                   NULL, NULL) == 0 &&
          valid_xml() == 0 && strstr(wire_body, "/dav/tree/public") == NULL);
    CHECK(exchange(port, "DELETE", "/dav/tree", NULL, "", 404, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/tree/public", NULL, "", 200, "public",
                   NULL, NULL) == 0);
    CHECK(exchange(port, "COPY", "/dav/safe", "Destination: /dav/blocked\r\n",
                   "", 404, "", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/blocked/public", NULL, "", 404, "", NULL,
                   NULL) == 0);
    snprintf(headers, sizeof(headers), "%s/blocked", root);
    CHECK(access(headers, F_OK) != 0);
    CHECK(exchange(port, "COPY", "/dav/safe", "Destination: /dav/victim\r\n",
                   "", 404, "", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/victim/keep", NULL, "", 200, "keep", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "COPY", "/dav/safe",
                   "Destination: /dav/copied\r\nDepth: 0\r\n", "", 201, "",
                   NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/copied/public", NULL, "", 404, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "COPY", "/dav/safe",
                   "Destination: http://localhost:80/dav/copied\r\nDepth: "
                   "infinity\r\n",
                   "", 204, "", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/copied/public", NULL, "", 200, "public",
                   NULL, NULL) == 0);
    CHECK(exchange(port, "COPY", "/dav/safe",
                   "Destination: /dav/copied\r\nOverwrite: F\r\n", "", 412, "",
                   NULL, NULL) == 0);
    CHECK(exchange(port, "MOVE", "/dav/copied",
                   "Destination: /dav/moved\r\nDepth: 0\r\n", "", 400, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "MOVE", "/dav/copied", "Destination: /dav/moved\r\n",
                   "", 201, "", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/dav/moved/public", NULL, "", 200, "public",
                   NULL, NULL) == 0);
    CHECK(exchange(port, "DELETE", "/dav/moved", NULL, "", 204, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "COPY", "/dav/file",
                   "Destination: http://foreign/dav/x\r\n", "", 400, "", NULL,
                   NULL) == 0);
    CHECK(exchange(port, "COPY", "/dav/file", "Destination: /outside/x\r\n", "",
                   400, "", NULL, NULL) == 0);
    CHECK(exchange(port, "COPY", "/dav/file", "Destination: /dav/%2e%2e/x\r\n",
                   "", 400, "", NULL, NULL) == 0);
    CHECK(exchange(port, "COPY", "/dav/file", "Destination: /dav/a%2fb\r\n", "",
                   400, "", NULL, NULL) == 0);
    CHECK(exchange(port, "OPTIONS", "/ro", NULL, "", 204, "",
                   "allow: OPTIONS, PROPFIND, GET, HEAD", NULL) == 0);
    CHECK(exchange(port, "PUT", "/ro/a%20%26%20b.txt",
                   "Expect: 100-continue\r\n", NULL, 405, "", NULL, NULL) == 0);
    CHECK(exchange(port, "GET", "/ro/a%20%26%20b.txt", NULL, "", 200, "packed",
                   "etag: \"packed\"", NULL) == 0);
    CHECK(exchange(port, "PROPFIND", "/ro/", "Depth: 1\r\n", "", 207, NULL,
                   NULL, NULL) == 0 &&
          valid_xml() == 0 && strstr(wire_body, "/ro/a%20%26%20b.txt") != NULL);
    CHECK(strstr(wire_body, "/ro/a%20%25%3F%5C%20b.txt") != NULL);
    CHECK(exchange(port, "GET", "/g/assets/a%20%26%20b.txt", NULL, "", 200,
                   "packed", "X-Before: yes", NULL) == 0);
    CHECK(exchange(port, "PROPFIND", "/g/assets", "Depth: 1\r\n", "", 207, NULL,
                   NULL, NULL) == 0 &&
          valid_xml() == 0);
    CHECK(exchange(port, "GET", "/stop", NULL, "", 200, "ok", NULL, NULL) == 0);
    free(wire_body);
    wire_body = NULL;
    return 0;
}
static rotmos_status worker_start(rotmos_server *server,
                                  const rotmos_process_context *process,
                                  void *data, rotmos_error *error)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    (void)error;
    f->owner = pthread_self();
    return ROTMOS_OK;
}
static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process, void *data,
                           rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_bound_listener listener;
    int status;
    (void)process;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_STATE;
    f->child = fork();
    if (f->child == 0) {
        signal(SIGTERM, SIG_DFL);
        status = client(listener.port, f->root);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    return f->child < 0 ? ROTMOS_IO : ROTMOS_OK;
}
static int worker_exit(rotmos_server *server,
                       const rotmos_process_context *process, void *data)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    (void)rotmos_storage_provider_close(f->readonly, NULL);
    (void)rotmos_asset_provider_close(f->assets, NULL);
    return f->failed || !f->stopped || f->mapped == 0 || f->overwritten == 0 ||
           f->hidden == 0 || f->after == 0 || f->aborted == 0 ||
           f->opened != f->closed || f->after_failed != 3;
}
static rotmos_status
unsupported_invoke(rotmos_storage_operation *op,
                   const rotmos_storage_arguments *arguments, void *data,
                   rotmos_error *error)
{
    (void)op;
    (void)arguments;
    (void)data;
    (void)error;
    return ROTMOS_OK;
}
static int put(const char *path, const char *bytes)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return 0;
    return write(fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes) &&
           close(fd) == 0;
}
static int remove_fixture(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    struct stat st;
    char child[4096];
    int ok = 1;
    if (directory == NULL)
        return 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (lstat(child, &st) != 0) {
            ok = 0;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!remove_fixture(child))
                ok = 0;
        } else if (unlink(child) != 0)
            ok = 0;
    }
    closedir(directory);
    return rmdir(path) == 0 && ok;
}
int main(void)
{
    struct fixture f;
    rotmos_server *server;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_webdav_config dav;
    rotmos_route_config route;
    rotmos_route_group_config grouping;
    rotmos_route_group *group;
    rotmos_asset_provider_config packed;
    rotmos_storage_provider_config incapable;
    rotmos_storage_provider *bad;
    rotmos_run_result result;
    rotmos_error error;
    rotmos_status status;
    int child_status, i;
    char cwd[2048], relative[] = "dav-fixture-XXXXXX", base[4096], path[4096];
    static const char *directories[] = {"tree", "safe", "victim"};
    memset(&f, 0, sizeof(f));
    f.child = -1;
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL && mkdtemp(relative) != NULL);
    snprintf(base, sizeof(base), "%s/%s", cwd, relative);
    snprintf(f.root, sizeof(f.root), "%s/content", base);
    snprintf(f.cache, sizeof(f.cache), "%s/cache", base);
    CHECK(mkdir(f.root, 0700) == 0);
    snprintf(path, sizeof(path), "%s/listing", f.root);
    CHECK(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/listing/blocked", f.root);
    CHECK(put(path, "metadata") && chmod(path, 0000) == 0);
    snprintf(path, sizeof(path), "%s/listing/a %%?\\ b.txt", f.root);
    CHECK(put(path, "fixture"));
    snprintf(path, sizeof(path), "%s/file", f.root);
    CHECK(put(path, "abcdef"));
    for (i = 0; i < 3; ++i) {
        snprintf(path, sizeof(path), "%s/%s", f.root, directories[i]);
        CHECK(mkdir(path, 0700) == 0);
    }
    snprintf(path, sizeof(path), "%s/tree/public", f.root);
    CHECK(put(path, "public"));
    snprintf(path, sizeof(path), "%s/tree/private", f.root);
    CHECK(put(path, "secret"));
    snprintf(path, sizeof(path), "%s/safe/public", f.root);
    CHECK(put(path, "public"));
    snprintf(path, sizeof(path), "%s/victim/private", f.root);
    CHECK(put(path, "secret"));
    snprintf(path, sizeof(path), "%s/victim/keep", f.root);
    CHECK(put(path, "keep"));
    signal(SIGPIPE, SIG_IGN);
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.workers.shutdown_timeout_ms = 1000;
    config.limits.max_connections = 512;
    config.workers.rlimit_nofiles = 4096;
    config.io.chunk_bytes = 128;
    config.io.output_queue_bytes = 4096;
    config.callbacks.on_worker_start = worker_start;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_exit = worker_exit;
    config.callbacks.userdata = &f;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    f.server = server;
    rotmos_route_config_init(&route);
    route.path = "/ping";
    route.handler = ping;
    route.userdata = &f;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    route.path = "/stop";
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    rotmos_webdav_config_init(&dav);
    dav.path_prefix = "/dav";
    dav.storage.cache_dir = f.cache;
    dav.storage.root_dir = f.root;
    dav.storage.site_id = "test";
    CHECK(rotmos_router_webdav(rotmos_server_router(server), &dav, NULL,
                               &error) == ROTMOS_INVALID);
    dav.auth = auth;
    dav.auth_userdata = &f;
    rotmos_storage_provider_config_init(&incapable);
    incapable.invoke = unsupported_invoke;
    incapable.cancel = unsupported_invoke;
    incapable.capabilities = ROTMOS_STORAGE_CAN_WRITE;
    CHECK(rotmos_storage_provider_new(&incapable, &bad, &error) == ROTMOS_OK);
    dav.provider = bad;
    CHECK(rotmos_router_webdav(rotmos_server_router(server), &dav, NULL,
                               &error) == ROTMOS_NOT_SUPPORTED);
    CHECK(rotmos_storage_provider_close(bad, &error) == ROTMOS_OK);
    dav.provider = NULL;
    dav.hooks.before = before;
    dav.hooks.after = after;
    dav.hooks.userdata = &f;
    CHECK(rotmos_router_webdav(rotmos_server_router(server), &dav, NULL,
                               &error) == ROTMOS_OK);
    rotmos_asset_provider_config_init(&packed);
    packed.open = packed_open;
    packed.userdata = &f;
    CHECK(rotmos_asset_provider_new(&packed, &f.assets, &error) == ROTMOS_OK);
    CHECK(rotmos_storage_provider_assets(f.assets, &f.readonly, &error) ==
          ROTMOS_OK);
    dav.provider = f.readonly;
    dav.path_prefix = "/ro";
    CHECK(rotmos_router_webdav(rotmos_server_router(server), &dav, NULL,
                               &error) == ROTMOS_OK);
    rotmos_route_group_config_init(&grouping);
    grouping.prefix = "/g";
    grouping.hooks.before = before;
    CHECK(rotmos_router_group(rotmos_server_router(server), &grouping, &group,
                              &error) == ROTMOS_OK);
    dav.path_prefix = "/assets";
    CHECK(rotmos_route_group_webdav(group, &dav, NULL, &error) == ROTMOS_OK);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        fprintf(stderr, "run: %s: %s\n", error.operation, error.message);
    CHECK(status == ROTMOS_OK && !result.escalated && !result.has_failure);
    CHECK(waitpid(f.child, &child_status, 0) == f.child &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    CHECK(rotmos_storage_provider_close(f.readonly, &error) == ROTMOS_OK);
    CHECK(rotmos_asset_provider_close(f.assets, &error) == ROTMOS_OK);
    CHECK(remove_fixture(base));
    return 0;
}
