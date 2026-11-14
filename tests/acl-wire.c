#define _POSIX_C_SOURCE 200809L
#include "acl-wire.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int knurl_test_acl_exchange(unsigned short port, const char *method,
                            const char *path, const char *headers,
                            const char *expected)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char request[1024], response[2048], *body;
    size_t used = 0, sent = 0, length;
    int fd, code = -1, count;
    ssize_t n;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
        goto done;
    count =
        snprintf(request, sizeof(request),
                 "%s %s HTTP/1.1\r\nHost: localhost\r\n%s%s\r\n", method, path,
                 headers && strstr(headers, "Connection: Upgrade\r\n")
                     ? ""
                     : "Connection: close\r\n",
                 headers ? headers : "");
    if (count <= 0 || (size_t)count >= sizeof(request))
        goto done;
    length = (size_t)count;
    while (sent < length) {
        n = write(fd, request + sent, length - sent);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto done;
        sent += (size_t)n;
    }
    while (used + 1 < sizeof(response)) {
        n = read(fd, response + used, sizeof(response) - used - 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            goto done;
        if (n == 0)
            break;
        used += (size_t)n;
    }
    response[used] = 0;
    body = strstr(response, "\r\n\r\n");
    if (body == NULL || sscanf(response, "HTTP/1.1 %d", &code) != 1 ||
        (expected && strcmp(body + 4, expected) != 0))
        code = -1;
done:
    (void)close(fd);
    return code;
}
int knurl_test_acl_blocked(unsigned short port)
{
    static const char *const paths[] = {
        "/blocked", "/json",        "/typed",     "/xml",
        "/rest",    "/static",      "/assets/a",  "/upload",
        "/reader",  "/upload-file", "/dav/a",     "/proxy",
        "/ws",      "/group/open",  "/list/open", "/rest-list/open"};
    size_t i;
    int code;
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        code = knurl_test_acl_exchange(
            port, i == 5 || i == 6 || i == 12 ? "GET" : "POST", paths[i],
            "Content-Length: 999999999\r\nExpect: 100-continue\r\n", NULL);
        if (code != 403) {
            fprintf(stderr, "ACL route %s: first response %d, expected 403\n",
                    paths[i], code);
            return 1;
        }
    }
    return 0;
}
