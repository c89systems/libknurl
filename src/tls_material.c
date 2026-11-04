#define _POSIX_C_SOURCE 200809L
#include "tls_internal.h"
#include "source_internal.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void rotmos_tls_material_clear(unsigned char *data, size_t length)
{
    if (data != NULL) {
        OPENSSL_cleanse(data, length);
        free(data);
    }
}

rotmos_status rotmos_tls_material_read(const rotmos_tls_material *material,
                                       size_t maximum, unsigned char **out,
                                       size_t *length, rotmos_error *error)
{
    rotmos_source *source;
    rotmos_status status;
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
    if (material->kind == ROTMOS_MATERIAL_NONE)
        return ROTMOS_OK;
    if (material->kind == ROTMOS_MATERIAL_MEMORY) {
        data = malloc(material->bytes.length + 1);
        if (data == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "tls_material",
                               "cannot copy bounded TLS material");
        memcpy(data, material->bytes.data, material->bytes.length);
        data[material->bytes.length] = 0;
        *out = data;
        *length = material->bytes.length;
        return ROTMOS_OK;
    }
    fd = -1;
    source = NULL;
    if (material->kind == ROTMOS_MATERIAL_FILE) {
        fd = open(material->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            return rotmos_fail(error, ROTMOS_IO, "tls_material",
                               "cannot open TLS material file");
        if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
            (void)close(fd);
            return rotmos_fail(error, ROTMOS_INVALID, "tls_material",
                               "TLS material file must be a regular file");
        }
        if (info.st_size < 0 || info.st_size > (off_t)maximum) {
            (void)close(fd);
            return rotmos_fail(error, ROTMOS_LIMIT, "tls_material",
                               "TLS material exceeds max_material_bytes");
        }
    } else {
        status = material->open(&source, material->userdata, error);
        if (status != ROTMOS_OK)
            return rotmos_fail(error,
                               status > ROTMOS_EOF && status <= ROTMOS_BACKEND
                                   ? status
                                   : ROTMOS_INVALID,
                               "tls_material", "TLS material open failed");
        if (source == NULL || source->owner != NULL || source->busy)
            return rotmos_fail(
                error, ROTMOS_STATE, "tls_material",
                "TLS material open must transfer an unowned source");
        source->owner = &source;
    }
    capacity = maximum < 8192 ? maximum + 1 : 8192;
    data = malloc(capacity);
    used = 0;
    status = data == NULL ? ROTMOS_NOMEM : ROTMOS_OK;
    while (status == ROTMOS_OK) {
        if (used == capacity) {
            next = capacity > (maximum + 1) / 2 ? maximum + 1 : capacity * 2;
            grown = malloc(next);
            if (grown == NULL) {
                status = ROTMOS_NOMEM;
                break;
            }
            memcpy(grown, data, used);
            rotmos_tls_material_clear(data, used);
            data = grown;
            capacity = next;
        }
        count = 0;
        if (source != NULL) {
            status = rotmos_source_pull(source, &source, data + used,
                                        capacity - used, &count, error);
            if (status == ROTMOS_AGAIN)
                status = ROTMOS_STATE;
        } else {
            do {
                got = read(fd, data + used, capacity - used);
            } while (got < 0 && errno == EINTR);
            status = got < 0 ? ROTMOS_IO : (got == 0 ? ROTMOS_EOF : ROTMOS_OK);
            if (got > 0)
                count = (size_t)got;
        }
        used += count;
        if (used > maximum)
            status = ROTMOS_LIMIT;
        if (status == ROTMOS_EOF)
            break;
    }
    if (fd >= 0)
        (void)close(fd);
    if (source != NULL)
        rotmos_source_release(source,
                              status == ROTMOS_EOF ? ROTMOS_OK : status);
    if (status != ROTMOS_EOF || used == 0) {
        rotmos_tls_material_clear(data, used);
        return rotmos_fail(error,
                           status == ROTMOS_EOF ? ROTMOS_INVALID : status,
                           "tls_material",
                           "TLS material must be nonempty, bounded and "
                           "synchronously readable");
    }
    data[used] = 0;
    *out = data;
    *length = used;
    return ROTMOS_OK;
}
