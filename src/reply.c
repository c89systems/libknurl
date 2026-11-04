#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "reply_internal.h"
#include "file_internal.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static rotmos_status mutable(rotmos_reply *reply, rotmos_error *error)
{
    if (reply == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "reply", "reply is required");
    if (reply->busy)
        return rotmos_fail(error, ROTMOS_STATE, "reply",
                           "reply mutation is already in progress");
    return ROTMOS_OK;
}

static void clear_payload(rotmos_reply *reply)
{
    unsigned char *body;
    char *type;
    int file;
    rotmos_source *source;
    body = reply->body;
    type = reply->content_type;
    file = reply->file;
    source = reply->source;
    reply->kind = ROTMOS_REPLY_EMPTY;
    reply->body = NULL;
    reply->body_length = 0;
    reply->content_type = NULL;
    reply->file = -1;
    reply->file_length.high = reply->file_length.low = 0;
    reply->source = NULL;
    free(body);
    free(type);
    if (file != -1)
        (void)close(file);
    if (source != NULL) {
        if (reply->owner != NULL) {
            source->retired_next = NULL;
            if (reply->retired_tail != NULL)
                reply->retired_tail->retired_next = source;
            else
                reply->retired_head = source;
            reply->retired_tail = source;
        } else {
            rotmos_source_release(source, ROTMOS_CANCELLED);
        }
    }
}

static rotmos_status prepare_type(rotmos_reply *reply, int status,
                                  const char *content_type, char **copy,
                                  rotmos_error *error)
{
    rotmos_status result;
    *copy = NULL;
    result = mutable(reply, error);
    if (result != ROTMOS_OK)
        return result;
    if (status < 200 || status > 599 ||
        !rotmos_header_value_valid(content_type) || content_type[0] == '\0')
        return rotmos_fail(
            error, ROTMOS_INVALID, "reply",
            "provide final status 200..599 and a nonempty content type");
    *copy = rotmos_copy_text(content_type);
    if (*copy == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "reply",
                           "cannot copy content type");
    return ROTMOS_OK;
}

static void select_payload(rotmos_reply *reply, int status,
                           rotmos_reply_kind kind, char *type)
{
    reply->busy = 1;
    clear_payload(reply);
    reply->status = status;
    reply->selected = 1;
    reply->kind = kind;
    reply->content_type = type;
    reply->busy = 0;
}

rotmos_status rotmos_reply_new(rotmos_reply **out, rotmos_error *error)
{
    rotmos_reply *reply;
    if (out != NULL)
        *out = NULL;
    if (out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "reply_new",
                           "reply output is required");
    reply = malloc(sizeof(*reply));
    if (reply == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "reply_new",
                           "cannot allocate reply");
    reply->selected = reply->status = reply->busy = 0;
    reply->owner = NULL;
    reply->kind = ROTMOS_REPLY_EMPTY;
    reply->content_type = NULL;
    reply->body = NULL;
    reply->body_length = 0;
    reply->file = -1;
    reply->file_length.high = reply->file_length.low = 0;
    reply->source = NULL;
    reply->retired_head = reply->retired_tail = NULL;
    reply->headers = NULL;
    reply->header_count = 0;
    *out = reply;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_reply_retire(rotmos_reply *reply)
{
    rotmos_source *source;
    int busy;
    if (reply == NULL)
        return;
    busy = reply->busy;
    reply->busy = 1;
    while (reply->retired_head != NULL) {
        source = reply->retired_head;
        reply->retired_head = source->retired_next;
        rotmos_source_release(source, ROTMOS_CANCELLED);
    }
    reply->retired_tail = NULL;
    reply->busy = busy;
}

rotmos_status rotmos_reply_destroy(rotmos_reply *reply, rotmos_error *error)
{
    size_t i;
    if (reply != NULL) {
        if (reply->busy || reply->owner != NULL)
            return rotmos_fail(error, ROTMOS_STATE, "reply_destroy",
                               "reply is owned or busy");
        reply->busy = 1;
        rotmos_reply_retire(reply);
        clear_payload(reply);
        for (i = 0; i < reply->header_count; ++i) {
            free((void *)reply->headers[i].name.data);
            free((void *)reply->headers[i].value.data);
        }
        free(reply->headers);
        free(reply);
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_reply_status(rotmos_reply *reply, int status,
                                  rotmos_error *error)
{
    rotmos_status result;
    result = mutable(reply, error);
    if (result != ROTMOS_OK)
        return result;
    if (status < 200 || status > 599)
        return rotmos_fail(error, ROTMOS_INVALID, "reply_status",
                           "final status must be 200..599");
    select_payload(reply, status, ROTMOS_REPLY_EMPTY, NULL);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static rotmos_status header_store(rotmos_reply *reply, const char *name,
                                  const char *value, int replace,
                                  rotmos_error *error)
{
    rotmos_status status;
    char *name_copy;
    char *value_copy;
    rotmos_header *headers;
    rotmos_header header;
    size_t i;
    size_t count;
    int inserted;
    status = mutable(reply, error);
    if (status != ROTMOS_OK)
        return status;
    if (!rotmos_header_name_valid(name) || !rotmos_header_value_valid(value))
        return rotmos_fail(error, ROTMOS_INVALID, "reply_header",
                           "provide a token header name and a value without "
                           "control characters");
    if (reply->header_count >= (size_t)-1 / sizeof(*headers))
        return rotmos_fail(error, ROTMOS_OVERFLOW, "reply_header",
                           "header count overflow");
    name_copy = rotmos_copy_text(name);
    value_copy = rotmos_copy_text(value);
    headers = malloc((reply->header_count + 1) * sizeof(*headers));
    if (name_copy == NULL || value_copy == NULL || headers == NULL) {
        free(name_copy);
        free(value_copy);
        free(headers);
        return rotmos_fail(error, ROTMOS_NOMEM, "reply_header",
                           "cannot copy header");
    }
    header.name.data = name_copy;
    header.name.length = strlen(name_copy);
    header.value.data = value_copy;
    header.value.length = strlen(value_copy);
    count = 0;
    inserted = 0;
    for (i = 0; i < reply->header_count; ++i) {
        if (replace &&
            rotmos_text_equal_case(reply->headers[i].name.data, name_copy)) {
            if (!inserted) {
                headers[count++] = header;
                inserted = 1;
            }
            free((void *)reply->headers[i].name.data);
            free((void *)reply->headers[i].value.data);
        } else {
            headers[count++] = reply->headers[i];
        }
    }
    if (!inserted)
        headers[count++] = header;
    free(reply->headers);
    reply->headers = headers;
    reply->header_count = count;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_reply_header_add(rotmos_reply *reply, const char *name,
                                      const char *value, rotmos_error *error)
{
    return header_store(reply, name, value, 0, error);
}

rotmos_status rotmos_reply_header_set(rotmos_reply *reply, const char *name,
                                      const char *value, rotmos_error *error)
{
    return header_store(reply, name, value, 1, error);
}

rotmos_status rotmos_reply_bytes(rotmos_reply *reply, int status,
                                 const char *content_type, rotmos_bytes body,
                                 rotmos_error *error)
{
    char *type;
    unsigned char *copy;
    rotmos_status result;
    result = prepare_type(reply, status, content_type, &type, error);
    if (result != ROTMOS_OK)
        return result;
    if (body.data == NULL && body.length != 0) {
        free(type);
        return rotmos_fail(error, ROTMOS_INVALID, "reply_bytes",
                           "nonempty body requires data");
    }
    copy = NULL;
    if (body.length != 0) {
        copy = malloc(body.length);
        if (copy == NULL) {
            free(type);
            return rotmos_fail(error, ROTMOS_NOMEM, "reply_bytes",
                               "cannot copy reply body");
        }
        memcpy(copy, body.data, body.length);
    }
    select_payload(reply, status, ROTMOS_REPLY_BYTES, type);
    reply->body = copy;
    reply->body_length = body.length;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_reply_text(rotmos_reply *reply, int status,
                                const char *content_type, const char *text,
                                rotmos_error *error)
{
    rotmos_bytes bytes;
    if (text == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "reply_text",
                           "text is required");
    bytes.data = text;
    bytes.length = strlen(text);
    return rotmos_reply_bytes(reply, status, content_type, bytes, error);
}

rotmos_status rotmos_reply_file(rotmos_reply *reply, int status,
                                const char *content_type, const char *path,
                                rotmos_error *error)
{
    char *type;
    rotmos_status result;
    int file;
    int code;
    struct stat info;
    result = prepare_type(reply, status, content_type, &type, error);
    if (result != ROTMOS_OK)
        return result;
    if (path == NULL || *path == '\0') {
        free(type);
        return rotmos_fail(error, ROTMOS_INVALID, "reply_file",
                           "file path is required");
    }
    file = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (file == -1) {
        code = errno;
        free(type);
        result = rotmos_fail(error, ROTMOS_IO, "reply_file",
                             "cannot open reply file");
        if (error != NULL) {
            error->source = ROTMOS_ERROR_SYSTEM;
            error->native_code = code;
        }
        return result;
    }
    if (fstat(file, &info) == -1) {
        code = errno;
        (void)close(file);
        free(type);
        result = rotmos_fail(error, ROTMOS_IO, "reply_file",
                             "cannot inspect reply file");
        if (error != NULL) {
            error->source = ROTMOS_ERROR_SYSTEM;
            error->native_code = code;
        }
        return result;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        (void)close(file);
        free(type);
        return rotmos_fail(
            error, ROTMOS_INVALID, "reply_file",
            "reply file must be a regular file with a valid size");
    }
    select_payload(reply, status, ROTMOS_REPLY_FILE, type);
    reply->file = file;
    reply->file_length.high = (unsigned long)((info.st_size >> 16) >> 16);
    reply->file_length.low = (unsigned long)info.st_size & 0xffffffffUL;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_reply_source_take(rotmos_reply *reply, int status,
                                       const char *content_type,
                                       rotmos_source *source,
                                       rotmos_error *error)
{
    char *type;
    rotmos_status result;
    result = prepare_type(reply, status, content_type, &type, error);
    if (result != ROTMOS_OK)
        return result;
    if (source == NULL || source->borrowed || source->owner != NULL ||
        source->busy || source->failure != ROTMOS_OK) {
        free(type);
        return rotmos_fail(
            error, source == NULL ? ROTMOS_INVALID : ROTMOS_STATE,
            "reply_source_take",
            "provide an unowned source which is not busy or failed");
    }
    source->owner = reply;
    select_payload(reply, status, ROTMOS_REPLY_SOURCE, type);
    reply->source = source;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static rotmos_status spool_error(rotmos_error *error, const char *message,
                                 int code)
{
    rotmos_fail(error, ROTMOS_IO, "reply_spooled", message);
    if (error != NULL) {
        error->source = ROTMOS_ERROR_SYSTEM;
        error->native_code = code;
    }
    return ROTMOS_IO;
}

rotmos_status rotmos_reply_generate_file(rotmos_reply *reply, int status,
                                         const char *content_type,
                                         const char *directory,
                                         rotmos_reply_file_writer_fn writer,
                                         void *data, rotmos_error *error)
{
    char *type, *path = NULL;
    int fd = -1, code;
    struct stat info;
    rotmos_status result;
    result = prepare_type(reply, status, content_type, &type, error);
    if (result != ROTMOS_OK)
        return result;
    if (directory == NULL || directory[0] == 0)
        directory = "/tmp";
    reply->busy = 1;
    result =
        rotmos_private_file(directory, "rotmos-response", &fd, &path, error);
    if (result == ROTMOS_OK) {
        if (unlink(path) != 0)
            result = spool_error(error, "cannot unlink private response file",
                                 errno);
        else {
            free(path);
            path = NULL;
        }
    }
    if (result == ROTMOS_OK)
        result = writer(fd, data, error);
    if (result == ROTMOS_OK && fstat(fd, &info) != 0)
        result =
            spool_error(error, "cannot inspect private response file", errno);
    if (result == ROTMOS_OK && (!S_ISREG(info.st_mode) || info.st_size < 0))
        result =
            spool_error(error, "invalid private response file size", EOVERFLOW);
    reply->busy = 0;
    if (result == ROTMOS_OK) {
        select_payload(reply, status, ROTMOS_REPLY_FILE, type);
        reply->file = fd;
        reply->file_length.high = (unsigned long)((info.st_size >> 16) >> 16);
        reply->file_length.low = (unsigned long)info.st_size & 0xffffffffUL;
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    code = errno;
    if (fd != -1)
        (void)close(fd);
    if (path != NULL)
        (void)unlink(path);
    free(path);
    free(type);
    errno = code;
    return result;
}

struct spool_source {
    rotmos_source *source;
    int claimed;
};
static rotmos_status spool_source_write(int fd, void *data, rotmos_error *error)
{
    struct spool_source *s = data;
    unsigned char *chunk;
    size_t count;
    rotmos_status status;
    chunk = malloc(8192);
    if (chunk == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "reply_source_spooled",
                           "cannot allocate response source chunk");
    status = s->source->config.reset != NULL
                 ? rotmos_source_reset(s->source, error)
                 : ROTMOS_OK;
    if (status == ROTMOS_OK) {
        s->source->owner = s;
        s->claimed = 1;
    }
    while (status == ROTMOS_OK) {
        status = rotmos_source_pull(s->source, s, chunk, 8192, &count, error);
        if (status == ROTMOS_EOF) {
            status = ROTMOS_OK;
            break;
        }
        if (status == ROTMOS_AGAIN)
            status = rotmos_fail(
                error, ROTMOS_NOT_SUPPORTED, "reply_source_spooled",
                "synchronous spooling cannot wait on a paused source");
        if (status == ROTMOS_OK)
            status = rotmos_file_write(fd, chunk, count, error);
    }
    free(chunk);
    return status;
}

rotmos_status rotmos_reply_source_spooled(rotmos_reply *reply, int status,
                                          const char *content_type,
                                          rotmos_source *source,
                                          const char *directory,
                                          rotmos_error *error)
{
    struct spool_source state;
    rotmos_status result;
    if (source == NULL || source->owner != NULL || source->busy)
        return rotmos_fail(error,
                           source == NULL ? ROTMOS_INVALID : ROTMOS_STATE,
                           "reply_source_spooled",
                           "provide an unowned source which is not busy");
    state.source = source;
    state.claimed = 0;
    result = rotmos_reply_generate_file(reply, status, content_type, directory,
                                        spool_source_write, &state, error);
    if (state.claimed)
        source->owner = NULL;
    return result;
}

void rotmos_source_materialize_config_init(
    rotmos_source_materialize_config *config)
{
    if (config == NULL)
        return;
    config->max_bytes = 10UL * 1024UL * 1024UL;
    config->chunk_bytes = 8192;
    config->reset_before = config->reset_after = 0;
}

rotmos_status rotmos_reply_source_materialized(
    rotmos_reply *reply, int status, const char *content_type,
    rotmos_source *source, const rotmos_source_materialize_config *config,
    rotmos_error *error)
{
    rotmos_source_materialize_config effective;
    unsigned char *body;
    unsigned char *chunk;
    unsigned char *grown;
    char *type;
    size_t length;
    size_t capacity;
    size_t count;
    size_t next;
    int claimed = 0;
    rotmos_status result;
    rotmos_status reset_result;
    result = prepare_type(reply, status, content_type, &type, error);
    if (result != ROTMOS_OK)
        return result;
    if (source == NULL || source->owner != NULL || source->busy) {
        free(type);
        return rotmos_fail(error,
                           source == NULL ? ROTMOS_INVALID : ROTMOS_STATE,
                           "reply_source_materialized",
                           "provide an unowned source which is not busy");
    }
    rotmos_source_materialize_config_init(&effective);
    if (config != NULL) {
        if (config->max_bytes != 0)
            effective.max_bytes = config->max_bytes;
        if (config->chunk_bytes != 0)
            effective.chunk_bytes = config->chunk_bytes;
        effective.reset_before = config->reset_before;
        effective.reset_after = config->reset_after;
    }
    if (effective.chunk_bytes > effective.max_bytes)
        effective.chunk_bytes = effective.max_bytes;
    chunk = malloc(effective.chunk_bytes);
    body = NULL;
    length = capacity = 0;
    if (chunk == NULL) {
        free(type);
        return rotmos_fail(error, ROTMOS_NOMEM, "reply_source_materialized",
                           "cannot allocate source chunk");
    }
    reply->busy = 1;
    result =
        effective.reset_before ? rotmos_source_reset(source, error) : ROTMOS_OK;
    if (result == ROTMOS_OK) {
        source->owner = reply;
        claimed = 1;
    }
    while (result == ROTMOS_OK) {
        result = rotmos_source_pull(source, reply, chunk, effective.chunk_bytes,
                                    &count, error);
        if (result == ROTMOS_EOF) {
            result = ROTMOS_OK;
            break;
        }
        if (result == ROTMOS_AGAIN)
            result = rotmos_fail(
                error, ROTMOS_NOT_SUPPORTED, "reply_source_materialized",
                "synchronous materialization cannot wait on a paused source");
        if (result != ROTMOS_OK)
            break;
        if (count > effective.max_bytes - length) {
            result = rotmos_fail(
                error, ROTMOS_LIMIT, "reply_source_materialized",
                "source exceeds the explicit materialization bound");
            break;
        }
        if (length + count > capacity) {
            next = capacity == 0 ? effective.chunk_bytes : capacity;
            while (next < length + count) {
                if (next > effective.max_bytes / 2) {
                    next = effective.max_bytes;
                    break;
                }
                next *= 2;
            }
            grown = realloc(body, next);
            if (grown == NULL) {
                result = rotmos_fail(error, ROTMOS_NOMEM,
                                     "reply_source_materialized",
                                     "cannot grow materialized reply");
                break;
            }
            body = grown;
            capacity = next;
        }
        memcpy(body + length, chunk, count);
        length += count;
    }
    if (effective.reset_after) {
        if (claimed)
            source->owner = NULL;
        reset_result =
            rotmos_source_reset(source, result == ROTMOS_OK ? error : NULL);
        source->owner = reply;
        claimed = 1;
        if (result == ROTMOS_OK)
            result = reset_result;
    }
    reply->busy = 0;
    free(chunk);
    if (result != ROTMOS_OK) {
        if (claimed)
            source->owner = NULL;
        free(body);
        free(type);
        return result;
    }
    select_payload(reply, status, ROTMOS_REPLY_BYTES, type);
    reply->body = body;
    reply->body_length = length;
    if (claimed)
        source->owner = NULL;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

int rotmos_reply_is_selected(const rotmos_reply *reply)
{
    return reply != NULL && reply->selected;
}

int rotmos_reply_status_code(const rotmos_reply *reply)
{
    return reply != NULL ? reply->status : 0;
}

rotmos_reply_kind rotmos_reply_payload_kind(const rotmos_reply *reply)
{
    return reply != NULL ? reply->kind : ROTMOS_REPLY_EMPTY;
}

rotmos_bytes rotmos_reply_content_type(const rotmos_reply *reply)
{
    rotmos_bytes value;
    value.data = reply != NULL ? reply->content_type : NULL;
    value.length = value.data != NULL ? strlen(value.data) : 0;
    return value;
}

rotmos_bytes rotmos_reply_body(const rotmos_reply *reply)
{
    rotmos_bytes value;
    value.data = reply != NULL ? reply->body : NULL;
    value.length = reply != NULL ? reply->body_length : 0;
    return value;
}

size_t rotmos_reply_header_count(const rotmos_reply *reply)
{
    return reply != NULL ? reply->header_count : 0;
}

rotmos_status rotmos_reply_header_at(const rotmos_reply *reply, size_t index,
                                     rotmos_header *out, rotmos_error *error)
{
    if (reply == NULL || out == NULL || index >= reply->header_count)
        return rotmos_fail(error, ROTMOS_INVALID, "reply_header_at",
                           "header index is outside this reply");
    *out = reply->headers[index];
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
