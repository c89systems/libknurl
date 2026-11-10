#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "body_internal.h"
#include <knurl/io.h>

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct result_reader {
    int fd;
    const unsigned char *memory;
    knurl_u64 position;
    knurl_u64 length;
};

static knurl_status read_result(void *buffer, size_t capacity, size_t *produced,
                                void *userdata, knurl_error *error)
{
    struct result_reader *reader;
    knurl_u64 remaining;
    knurl_u64 amount;
    size_t count;
    size_t position;
    off_t offset;
    ssize_t got;
    reader = userdata;
    remaining.high = reader->length.high - reader->position.high -
                     (reader->length.low < reader->position.low ? 1 : 0);
    remaining.low = (reader->length.low - reader->position.low) & 0xffffffffUL;
    *produced = 0;
    if (remaining.high == 0 && remaining.low == 0)
        return KNURL_EOF;
    count = capacity;
    if (knurl_u64_to_size(remaining, &position, NULL) == KNURL_OK &&
        count > position)
        count = position;
    if (reader->fd != -1) {
        if (!knurl_file_offset(reader->position, &offset))
            return knurl_fail(error, KNURL_OVERFLOW, "body_result_read",
                              "file offset cannot fit off_t");
        do {
            got = pread(reader->fd, buffer, count, offset);
        } while (got == -1 && errno == EINTR);
        if (got <= 0)
            return knurl_fail(error, KNURL_IO, "body_result_read",
                              "file ended short or cannot be read");
        count = (size_t)got;
    } else {
        (void)knurl_u64_to_size(reader->position, &position, NULL);
        memcpy(buffer, reader->memory + position, count);
    }
    (void)knurl_u64_from_size(count, &amount, NULL);
    (void)knurl_u64_add(reader->position, amount, &reader->position, NULL);
    *produced = count;
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status reset_result(void *userdata, knurl_error *error)
{
    struct result_reader *reader;
    reader = userdata;
    reader->position.high = reader->position.low = 0;
    knurl_error_clear(error);
    return KNURL_OK;
}

static void close_result(void *userdata, knurl_status outcome)
{
    struct result_reader *reader;
    (void)outcome;
    reader = userdata;
    if (reader->fd != -1)
        (void)close(reader->fd);
    free(reader);
}
static knurl_status reader_source(struct result_reader *reader,
                                  knurl_source **out, knurl_error *error)
{
    knurl_source_config config;
    knurl_status status;
    knurl_source_config_init(&config);
    config.read = read_result;
    config.reset = reset_result;
    config.close = close_result;
    config.userdata = reader;
    config.has_length = 1;
    config.length = reader->length;
    status = knurl_source_new(&config, out, error);
    if (status != KNURL_OK)
        close_result(reader, status);
    return status;
}
knurl_status knurl_source_file(const char *path, knurl_source **out,
                               knurl_error *error)
{
    struct result_reader *reader;
    struct stat st;
    int code, stat_result;
    if (out != NULL)
        *out = NULL;
    if (path == NULL || path[0] == 0 || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "source_file",
                          "provide a nonempty path and source output");
    reader = calloc(1, sizeof(*reader));
    if (reader == NULL)
        return knurl_fail(error, KNURL_NOMEM, "source_file",
                          "cannot allocate file reader");
    reader->fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    stat_result = reader->fd == -1 ? -1 : fstat(reader->fd, &st);
    if (stat_result != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        code = stat_result != 0 ? errno : EINVAL;
        close_result(reader, KNURL_IO);
        knurl_fail(error, KNURL_IO, "source_file",
                   "cannot open a regular input file");
        if (error != NULL) {
            error->source = KNURL_ERROR_SYSTEM;
            error->native_code = code;
        }
        return KNURL_IO;
    }
    reader->length.low = (unsigned long)st.st_size & 0xffffffffUL;
    reader->length.high = (unsigned long)((st.st_size >> 16) >> 16);
    return reader_source(reader, out, error);
}

knurl_status knurl_body_result_open_reader(const knurl_body_result *result,
                                           knurl_source **out,
                                           knurl_error *error)
{
    struct result_reader *reader;
    struct stat st;
    size_t length;
    off_t offset;
    if (out != NULL)
        *out = NULL;
    if (result == NULL || out == NULL || !knurl_u64_valid(result->length) ||
        (result->kind != KNURL_BODY_MEMORY && result->kind != KNURL_BODY_FILE))
        return knurl_fail(error, KNURL_INVALID, "body_result_reader",
                          "provide a valid result and source output");
    if (result->kind == KNURL_BODY_MEMORY &&
        (knurl_u64_to_size(result->length, &length, NULL) != KNURL_OK ||
         length != result->memory.length ||
         (length != 0 && result->memory.data == NULL)))
        return knurl_fail(error, KNURL_INVALID, "body_result_reader",
                          "memory storage does not match declared length");
    if (result->kind == KNURL_BODY_FILE &&
        (result->path == NULL || !knurl_file_offset(result->length, &offset)))
        return knurl_fail(error, KNURL_INVALID, "body_result_reader",
                          "provide a path with a seekable length");
    reader = calloc(1, sizeof(*reader));
    if (reader == NULL)
        return knurl_fail(error, KNURL_NOMEM, "body_result_reader",
                          "cannot allocate result reader");
    reader->fd = -1;
    reader->memory = result->memory.data;
    reader->length = result->length;
    if (result->kind == KNURL_BODY_FILE) {
        reader->fd = open(result->path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (reader->fd == -1 || fstat(reader->fd, &st) != 0 ||
            !S_ISREG(st.st_mode) || st.st_size < offset) {
            close_result(reader, KNURL_IO);
            return knurl_fail(error, KNURL_IO, "body_result_reader",
                              "cannot open a regular result file containing "
                              "the declared length");
        }
    }
    return reader_source(reader, out, error);
}
