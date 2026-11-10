#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "file_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static knurl_status file_error(knurl_error *error, const char *operation,
                               const char *message, int code)
{
    knurl_fail(error, KNURL_IO, operation, message);
    if (error != NULL) {
        error->source = KNURL_ERROR_SYSTEM;
        error->native_code = code;
    }
    return KNURL_IO;
}

int knurl_file_offset(knurl_u64 value, off_t *out)
{
    off_t result;
    unsigned long high;
    int bit;
    result = 0;
    high = value.high;
    /* Construct without an out-of-range signed conversion or a 32-bit shift. */
    for (bit = 63; bit >= 0; --bit) {
        if (bit >= 32 && sizeof(off_t) <= 4 && (high >> (bit - 32) & 1UL))
            return 0;
        if ((sizeof(off_t) <= 4 && bit == 31 && (value.low >> 31 & 1UL)) ||
            (bit == 63 && (high >> 31 & 1UL)))
            return 0;
        result = result * 2 + (off_t)(bit >= 32 ? (high >> (bit - 32) & 1UL)
                                                : (value.low >> bit & 1UL));
    }
    *out = result;
    return 1;
}

knurl_status knurl_private_file(const char *directory, const char *prefix,
                                int *fd, char **path, knurl_error *error)
{
    size_t directory_length;
    size_t prefix_length;
    size_t i;
    char *text;
    int file;
    int code;
    *fd = -1;
    *path = NULL;
    if (prefix == NULL || prefix[0] == '\0')
        prefix = "knurl-body";
    if (directory == NULL || directory[0] == '\0')
        return knurl_fail(error, KNURL_INVALID, "body_file",
                          "provide an explicit directory and filename prefix");
    for (i = 0; prefix[i] != '\0'; ++i)
        if (!((prefix[i] >= 'a' && prefix[i] <= 'z') ||
              (prefix[i] >= 'A' && prefix[i] <= 'Z') ||
              (prefix[i] >= '0' && prefix[i] <= '9') || prefix[i] == '-' ||
              prefix[i] == '_' || prefix[i] == '.'))
            return knurl_fail(error, KNURL_INVALID, "body_file",
                              "prefix must contain filename characters only");
    directory_length = strlen(directory);
    prefix_length = strlen(prefix);
    if (directory_length > (size_t)-1 - prefix_length - 9)
        return knurl_fail(error, KNURL_OVERFLOW, "body_file",
                          "file path length overflow");
    text = malloc(directory_length + prefix_length + 9);
    if (text == NULL)
        return knurl_fail(error, KNURL_NOMEM, "body_file",
                          "cannot allocate file path");
    memcpy(text, directory, directory_length);
    text[directory_length] = '/';
    memcpy(text + directory_length + 1, prefix, prefix_length);
    memcpy(text + directory_length + 1 + prefix_length, ".XXXXXX", 8);
    file = mkstemp(text);
    if (file == -1) {
        code = errno;
        free(text);
        return file_error(error, "private_file", "cannot create private file",
                          code);
    }
    if (fcntl(file, F_SETFD, FD_CLOEXEC) == -1) {
        code = errno;
        (void)close(file);
        (void)unlink(text);
        free(text);
        return file_error(error, "private_file", "cannot set close-on-exec",
                          code);
    }
    *fd = file;
    *path = text;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_file_write(int fd, const void *data, size_t length,
                              knurl_error *error)
{
    const unsigned char *bytes;
    ssize_t count;
    bytes = data;
    while (length != 0) {
        count = write(fd, bytes, length);
        if (count == -1 && errno == EINTR)
            continue;
        if (count <= 0)
            return file_error(error, "file_write", "cannot write private file",
                              count == 0 ? EIO : errno);
        bytes += (size_t)count;
        length -= (size_t)count;
    }
    knurl_error_clear(error);
    return KNURL_OK;
}
