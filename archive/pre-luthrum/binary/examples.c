#define _POSIX_C_SOURCE 200809L
#include "examples.h"
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define knurl_facade example_static
#include "example_static.inc"
#undef knurl_facade
#define knurl_facade example_proxy
#include "example_proxy.inc"
#undef knurl_facade
#define knurl_facade example_webdav
#include "example_webdav.inc"
#undef knurl_facade
#define knurl_facade example_combined
#include "example_combined.inc"
#undef knurl_facade

int knurl_dump_examples(const char *directory)
{
    static const struct example {
        const char *name;
        const unsigned char *bytes;
        size_t length;
    } examples[] = {
        {"knurl-example-static.lua", example_static, sizeof(example_static)},
        {"knurl-example-proxy.lua", example_proxy, sizeof(example_proxy)},
        {"knurl-example-webdav.lua", example_webdav, sizeof(example_webdav)},
        {"knurl-example-combined.lua", example_combined,
         sizeof(example_combined)}};
    int dir, files[4], saved;
    size_t i, created = 0, offset;
    ssize_t count;
    dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) {
        fprintf(stderr, "knurl: opening example directory %s: %s\n", directory,
                strerror(errno));
        return 1;
    }
    for (i = 0; i < 4; ++i) {
        files[i] =
            openat(dir, examples[i].name,
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (files[i] < 0)
            goto failed;
        ++created;
        offset = 0;
        while (offset < examples[i].length) {
            count = write(files[i], examples[i].bytes + offset,
                          examples[i].length - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                goto failed;
            offset += (size_t)count;
        }
        if (close(files[i]) != 0) {
            files[i] = -1;
            goto failed;
        }
        files[i] = -1;
    }
    close(dir);
    return 0;
failed:
    saved = errno;
    fprintf(stderr, "knurl: writing %s/%s: %s\n", directory, examples[i].name,
            strerror(saved));
    for (i = 0; i < created; ++i) {
        if (files[i] >= 0)
            close(files[i]);
        (void)unlinkat(dir, examples[i].name, 0);
    }
    close(dir);
    return 1;
}
