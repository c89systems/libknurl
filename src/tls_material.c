#define _POSIX_C_SOURCE 200809L
#include "tls_internal.h"
#include "source_internal.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void knurl_tls_material_clear(unsigned char *data, size_t length)
{
    if (data != NULL) {
        OPENSSL_cleanse(data, length);
        free(data);
    }
}

knurl_status knurl_tls_material_read(const knurl_tls_material *material,
                                     size_t maximum, unsigned char **out,
                                     size_t *length, knurl_error *error)
{
    knurl_source *source;
    knurl_status status;
    struct stat info;
    unsigned char *data;
    unsigned char *grown;
    size_t capacity;
    size_t next;
    size_t used;
    size_t count;
    ssize_t got;
    int fd;
    *out = NULL;
    *length = 0;
    /* The configured ceiling may be SIZE_MAX. The PEM BIO input length is
     * signed int; enforce that intrinsic bound on consumed bytes instead. */
    if (maximum > INT_MAX - 1UL)
        maximum = INT_MAX - 1UL;
    if (material->kind == KNURL_MATERIAL_NONE)
        return KNURL_OK;
    if (material->kind == KNURL_MATERIAL_MEMORY) {
        if (material->bytes.length > maximum)
            return knurl_fail(error, KNURL_LIMIT, "tls_material",
                              "TLS material exceeds the PEM input range");
        data = malloc(material->bytes.length + 1);
        if (data == NULL)
            return knurl_fail(error, KNURL_NOMEM, "tls_material",
                              "cannot copy bounded TLS material");
        memcpy(data, material->bytes.data, material->bytes.length);
        data[material->bytes.length] = 0;
        *out = data;
        *length = material->bytes.length;
        return KNURL_OK;
    }
    fd = -1;
    source = NULL;
    if (material->kind == KNURL_MATERIAL_FILE) {
        fd = open(material->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            return knurl_fail(error, KNURL_IO, "tls_material",
                              "cannot open TLS material file");
        if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
            (void)close(fd);
            return knurl_fail(error, KNURL_INVALID, "tls_material",
                              "TLS material file must be a regular file");
        }
        if (info.st_size < 0 || info.st_size > (off_t)maximum) {
            (void)close(fd);
            return knurl_fail(error, KNURL_LIMIT, "tls_material",
                              "TLS material exceeds max_material_bytes");
        }
    } else {
        knurl_error_clear(error);
        status = material->open(&source, material->userdata, error);
        if (status != KNURL_OK) {
            if (status > KNURL_EOF && status <= KNURL_BACKEND &&
                error != NULL && error->status == status)
                return status;
            return knurl_fail(error,
                              status > KNURL_EOF && status <= KNURL_BACKEND
                                  ? status
                                  : KNURL_INVALID,
                              "tls_material", "TLS material open failed");
        }
        if (source == NULL || source->impl->borrowed ||
            source->impl->owner != NULL || source->impl->busy)
            return knurl_fail(
                error, KNURL_STATE, "tls_material",
                "TLS material open must transfer an unowned source");
        source->impl->owner = &source;
    }
    capacity = maximum < 8192 ? maximum + 1 : 8192;
    data = malloc(capacity);
    used = 0;
    status = data == NULL ? KNURL_NOMEM : KNURL_OK;
    while (status == KNURL_OK) {
        if (used == capacity) {
            next = capacity > (maximum + 1) / 2 ? maximum + 1 : capacity * 2;
            grown = malloc(next);
            if (grown == NULL) {
                status = KNURL_NOMEM;
                break;
            }
            memcpy(grown, data, used);
            knurl_tls_material_clear(data, used);
            data = grown;
            capacity = next;
        }
        count = 0;
        if (source != NULL) {
            status = knurl_source_pull(source, &source, data + used,
                                       capacity - used, &count, error);
            if (status == KNURL_AGAIN)
                status = KNURL_STATE;
        } else {
            do {
                got = read(fd, data + used, capacity - used);
            } while (got < 0 && errno == EINTR);
            status = got < 0 ? KNURL_IO : (got == 0 ? KNURL_EOF : KNURL_OK);
            if (got > 0)
                count = (size_t)got;
        }
        used += count;
        if (used > maximum)
            status = KNURL_LIMIT;
        if (status == KNURL_EOF)
            break;
    }
    if (fd >= 0)
        (void)close(fd);
    if (source != NULL)
        knurl_source_release(source, status == KNURL_EOF ? KNURL_OK : status);
    if (status != KNURL_EOF || used == 0) {
        knurl_tls_material_clear(data, used);
        if (status > KNURL_EOF && status <= KNURL_BACKEND && error != NULL &&
            error->status == status)
            return status;
        return knurl_fail(error, status == KNURL_EOF ? KNURL_INVALID : status,
                          "tls_material",
                          "TLS material must be nonempty, bounded and "
                          "synchronously readable");
    }
    data[used] = 0;
    *out = data;
    *length = used;
    return KNURL_OK;
}
