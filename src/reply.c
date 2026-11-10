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

static knurl_status mutable(knurl_reply *reply, knurl_error *error)
{
    if (reply == NULL)
        return knurl_fail(error, KNURL_INVALID, "reply", "reply is required");
    if (reply->busy)
        return knurl_fail(error, KNURL_STATE, "reply",
                          "reply mutation is already in progress");
    return KNURL_OK;
}

static void clear_payload(knurl_reply *reply)
{
    unsigned char *body;
    char *type;
    int file;
    knurl_source *source;
    body = reply->body;
    type = reply->content_type;
    file = reply->file;
    source = reply->source;
    reply->kind = KNURL_REPLY_EMPTY;
    reply->body = NULL;
    reply->body_length = 0;
    reply->content_type = NULL;
    reply->file = -1;
    reply->file_length.high = reply->file_length.low = 0;
    reply->file_modified = 0;
    reply->source = NULL;
    free(body);
    free(type);
    free(reply->file_path);
    reply->file_path = NULL;
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
            knurl_source_release(source, KNURL_CANCELLED);
        }
    }
}

static knurl_status prepare_type(knurl_reply *reply, int status,
                                 const char *content_type, char **copy,
                                 knurl_error *error)
{
    knurl_status result;
    *copy = NULL;
    result = mutable(reply, error);
    if (result != KNURL_OK)
        return result;
    if (status < 100 || status > 599 || !knurl_content_type_valid(content_type))
        return knurl_fail(
            error, KNURL_INVALID, "reply",
            "provide status 100..599 and a valid Content-Type media type");
    *copy = knurl_copy_text(content_type);
    if (*copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "reply",
                          "cannot copy content type");
    return KNURL_OK;
}

static void select_payload(knurl_reply *reply, int status,
                           knurl_reply_kind kind, char *type)
{
    reply->busy = 1;
    clear_payload(reply);
    reply->status = status;
    reply->selected = 1;
    reply->kind = kind;
    reply->content_type = type;
    reply->busy = 0;
}

/* Vectis's handler-error response is raw diagnostic bytes, with no application
 * headers or synthesized content type. The caller supplies a fresh builder. */
knurl_status knurl_reply_failure(knurl_reply *reply, int code,
                                 const char *message)
{
    knurl_status status;
    if (message == NULL || *message == '\0')
        return knurl_reply_status(reply, code, NULL);
    status = knurl_reply_text(reply, code, "text/plain", message, NULL);
    if (status == KNURL_OK) {
        free(reply->content_type);
        reply->content_type = NULL;
    }
    return status;
}

knurl_status knurl_reply_new(knurl_reply **out, knurl_error *error)
{
    knurl_reply *reply;
    if (out != NULL)
        *out = NULL;
    if (out == NULL)
        return knurl_fail(error, KNURL_INVALID, "reply_new",
                          "reply output is required");
    reply = malloc(sizeof(*reply));
    if (reply == NULL)
        return knurl_fail(error, KNURL_NOMEM, "reply_new",
                          "cannot allocate reply");
    reply->selected = reply->status = reply->busy = 0;
    reply->owner = NULL;
    reply->kind = KNURL_REPLY_EMPTY;
    reply->content_type = NULL;
    reply->body = NULL;
    reply->body_length = 0;
    reply->file = -1;
    reply->file_path = NULL;
    reply->file_length.high = reply->file_length.low = 0;
    reply->file_modified = 0;
    reply->source = NULL;
    reply->retired_head = reply->retired_tail = NULL;
    reply->headers = NULL;
    reply->header_count = 0;
    *out = reply;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_reply_retire(knurl_reply *reply)
{
    knurl_source *source;
    int busy;
    if (reply == NULL)
        return;
    busy = reply->busy;
    reply->busy = 1;
    while (reply->retired_head != NULL) {
        source = reply->retired_head;
        reply->retired_head = source->retired_next;
        knurl_source_release(source, KNURL_CANCELLED);
    }
    reply->retired_tail = NULL;
    reply->busy = busy;
}

knurl_status knurl_reply_destroy(knurl_reply *reply, knurl_error *error)
{
    size_t i;
    if (reply != NULL) {
        if (reply->busy || reply->owner != NULL)
            return knurl_fail(error, KNURL_STATE, "reply_destroy",
                              "reply is owned or busy");
        reply->busy = 1;
        knurl_reply_retire(reply);
        clear_payload(reply);
        for (i = 0; i < reply->header_count; ++i) {
            free((void *)reply->headers[i].name.data);
            free((void *)reply->headers[i].value.data);
        }
        free(reply->headers);
        free(reply);
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_reply_status(knurl_reply *reply, int status,
                                knurl_error *error)
{
    knurl_status result;
    result = mutable(reply, error);
    if (result != KNURL_OK)
        return result;
    if (status < 100 || status > 599)
        return knurl_fail(error, KNURL_INVALID, "reply_status",
                          "status must be 100..599");
    select_payload(reply, status, KNURL_REPLY_EMPTY, NULL);
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status header_store(knurl_reply *reply, const char *name,
                                 const char *value, int replace,
                                 knurl_error *error)
{
    knurl_status status;
    char *name_copy;
    char *value_copy;
    knurl_header *headers;
    knurl_header header;
    size_t i;
    size_t count;
    int inserted;
    status = mutable(reply, error);
    if (status != KNURL_OK)
        return status;
    if (!knurl_header_name_valid(name) || !knurl_header_value_valid(value) ||
        (knurl_text_equal_case(name, "content-type") &&
         !knurl_content_type_valid(value)))
        return knurl_fail(error, KNURL_INVALID, "reply_header",
                          "provide a token header name, no value controls, "
                          "and valid media-type syntax for Content-Type");
    if (reply->header_count >= (size_t)-1 / sizeof(*headers))
        return knurl_fail(error, KNURL_OVERFLOW, "reply_header",
                          "header count overflow");
    name_copy = knurl_copy_text(name);
    value_copy = knurl_copy_text(value);
    headers = malloc((reply->header_count + 1) * sizeof(*headers));
    if (name_copy == NULL || value_copy == NULL || headers == NULL) {
        free(name_copy);
        free(value_copy);
        free(headers);
        return knurl_fail(error, KNURL_NOMEM, "reply_header",
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
            knurl_text_equal_case(reply->headers[i].name.data, name_copy)) {
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
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_reply_header_add(knurl_reply *reply, const char *name,
                                    const char *value, knurl_error *error)
{
    return header_store(reply, name, value, 0, error);
}

knurl_status knurl_reply_header_set(knurl_reply *reply, const char *name,
                                    const char *value, knurl_error *error)
{
    return header_store(reply, name, value, 1, error);
}

knurl_status knurl_reply_bytes(knurl_reply *reply, int status,
                               const char *content_type, knurl_bytes body,
                               knurl_error *error)
{
    char *type;
    unsigned char *copy;
    knurl_status result;
    result = prepare_type(reply, status, content_type, &type, error);
    if (result != KNURL_OK)
        return result;
    if (body.data == NULL && body.length != 0) {
        free(type);
        return knurl_fail(error, KNURL_INVALID, "reply_bytes",
                          "nonempty body requires data");
    }
    copy = NULL;
    if (body.length != 0) {
        copy = malloc(body.length);
        if (copy == NULL) {
            free(type);
            return knurl_fail(error, KNURL_NOMEM, "reply_bytes",
                              "cannot copy reply body");
        }
        memcpy(copy, body.data, body.length);
    }
    select_payload(reply, status, KNURL_REPLY_BYTES, type);
    reply->body = copy;
    reply->body_length = body.length;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_reply_text(knurl_reply *reply, int status,
                              const char *content_type, const char *text,
                              knurl_error *error)
{
    knurl_bytes bytes;
    if (text == NULL)
        return knurl_fail(error, KNURL_INVALID, "reply_text",
                          "text is required");
    bytes.data = text;
    bytes.length = strlen(text);
    return knurl_reply_bytes(reply, status, content_type, bytes, error);
}

knurl_status knurl_reply_file(knurl_reply *reply, int status,
                              const char *content_type, const char *path,
                              knurl_error *error)
{
    char *type;
    char *copy;
    knurl_status result;
    result = prepare_type(reply, status, content_type, &type, error);
    if (result != KNURL_OK)
        return result;
    if (path == NULL || *path == '\0') {
        free(type);
        return knurl_fail(error, KNURL_INVALID, "reply_file",
                          "file path is required");
    }
    copy = knurl_copy_text(path);
    if (copy == NULL) {
        free(type);
        return knurl_fail(error, KNURL_NOMEM, "reply_file",
                          "cannot copy reply file path");
    }
    select_payload(reply, status, KNURL_REPLY_FILE, type);
    reply->file_path = copy;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_reply_open_file(knurl_reply *reply, knurl_error *error)
{
    int file;
    int code;
    knurl_status result;
    struct stat info;
    if (reply->file != -1)
        return KNURL_OK;
    file = open(reply->file_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (file == -1) {
        code = errno;
        result = knurl_fail(error, KNURL_EOF, "reply_file_open",
                            "cannot open reply file");
        if (error != NULL) {
            error->source = KNURL_ERROR_SYSTEM;
            error->native_code = code;
        }
        return result;
    }
    if (fstat(file, &info) == -1) {
        code = errno;
        (void)close(file);
        result = knurl_fail(error, KNURL_EOF, "reply_file_open",
                            "cannot inspect reply file");
        if (error != NULL) {
            error->source = KNURL_ERROR_SYSTEM;
            error->native_code = code;
        }
        return result;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        (void)close(file);
        return knurl_fail(
            error, KNURL_IO, "reply_file_open",
            "reply file must be a regular file with a valid size");
    }
    reply->file = file;
    reply->file_length.high = (unsigned long)((info.st_size >> 16) >> 16);
    reply->file_length.low = (unsigned long)info.st_size & 0xffffffffUL;
    reply->file_modified = info.st_mtime;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_reply_source_take(knurl_reply *reply, int status,
                                     const char *content_type,
                                     knurl_source *source, knurl_error *error)
{
    char *type;
    knurl_status result;
    result = prepare_type(reply, status, content_type, &type, error);
    if (result != KNURL_OK)
        return result;
    if (source == NULL || source->borrowed || source->owner != NULL ||
        source->busy || source->failure != KNURL_OK) {
        free(type);
        return knurl_fail(
            error, source == NULL ? KNURL_INVALID : KNURL_STATE,
            "reply_source_take",
            "provide an unowned source which is not busy or failed");
    }
    source->owner = reply;
    select_payload(reply, status, KNURL_REPLY_SOURCE, type);
    reply->source = source;
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status spool_error(knurl_error *error, const char *message,
                                int code)
{
    knurl_fail(error, KNURL_IO, "reply_spooled", message);
    if (error != NULL) {
        error->source = KNURL_ERROR_SYSTEM;
        error->native_code = code;
    }
    return KNURL_IO;
}

knurl_status knurl_reply_generate_file(knurl_reply *reply, int status,
                                       const char *content_type,
                                       const char *directory,
                                       knurl_reply_file_writer_fn writer,
                                       void *data, knurl_error *error)
{
    char *type, *path = NULL;
    int fd = -1, code;
    struct stat info;
    knurl_status result;
    result = prepare_type(reply, status, content_type, &type, error);
    if (result != KNURL_OK)
        return result;
    if (directory == NULL || directory[0] == 0)
        directory = "/tmp";
    reply->busy = 1;
    result = knurl_private_file(directory, "knurl-response", &fd, &path, error);
    if (result == KNURL_OK) {
        if (unlink(path) != 0)
            result = spool_error(error, "cannot unlink private response file",
                                 errno);
        else {
            free(path);
            path = NULL;
        }
    }
    if (result == KNURL_OK)
        result = writer(fd, data, error);
    if (result == KNURL_OK && fstat(fd, &info) != 0)
        result =
            spool_error(error, "cannot inspect private response file", errno);
    if (result == KNURL_OK && (!S_ISREG(info.st_mode) || info.st_size < 0))
        result =
            spool_error(error, "invalid private response file size", EOVERFLOW);
    reply->busy = 0;
    if (result == KNURL_OK) {
        select_payload(reply, status, KNURL_REPLY_FILE, type);
        reply->file = fd;
        reply->file_length.high = (unsigned long)((info.st_size >> 16) >> 16);
        reply->file_length.low = (unsigned long)info.st_size & 0xffffffffUL;
        reply->file_modified = info.st_mtime;
        knurl_error_clear(error);
        return KNURL_OK;
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
    knurl_source *source;
    int claimed;
};
static knurl_status spool_source_write(int fd, void *data, knurl_error *error)
{
    struct spool_source *s = data;
    unsigned char *chunk;
    size_t count;
    knurl_status status;
    chunk = malloc(8192);
    if (chunk == NULL)
        return knurl_fail(error, KNURL_NOMEM, "reply_source_spooled",
                          "cannot allocate response source chunk");
    status = s->source->config.reset != NULL
                 ? knurl_source_reset(s->source, error)
                 : KNURL_OK;
    if (status == KNURL_OK) {
        s->source->owner = s;
        s->claimed = 1;
    }
    while (status == KNURL_OK) {
        status = knurl_source_pull(s->source, s, chunk, 8192, &count, error);
        if (status == KNURL_EOF) {
            status = KNURL_OK;
            break;
        }
        if (status == KNURL_AGAIN)
            status = knurl_fail(
                error, KNURL_NOT_SUPPORTED, "reply_source_spooled",
                "synchronous spooling cannot wait on a paused source");
        if (status == KNURL_OK)
            status = knurl_file_write(fd, chunk, count, error);
    }
    free(chunk);
    return status;
}

knurl_status knurl_reply_source_spooled(knurl_reply *reply, int status,
                                        const char *content_type,
                                        knurl_source *source,
                                        const char *directory,
                                        knurl_error *error)
{
    struct spool_source state;
    knurl_status result;
    if (source == NULL || source->owner != NULL || source->busy)
        return knurl_fail(error, source == NULL ? KNURL_INVALID : KNURL_STATE,
                          "reply_source_spooled",
                          "provide an unowned source which is not busy");
    state.source = source;
    state.claimed = 0;
    result = knurl_reply_generate_file(reply, status, content_type, directory,
                                       spool_source_write, &state, error);
    if (state.claimed)
        source->owner = NULL;
    return result;
}

void knurl_source_materialize_config_init(
    knurl_source_materialize_config *config)
{
    if (config == NULL)
        return;
    config->max_bytes = 10UL * 1024UL * 1024UL;
    config->chunk_bytes = 8192;
    config->reset_before = config->reset_after = 0;
}

knurl_status
knurl_reply_source_materialized(knurl_reply *reply, int status,
                                const char *content_type, knurl_source *source,
                                const knurl_source_materialize_config *config,
                                knurl_error *error)
{
    knurl_source_materialize_config effective;
    unsigned char *body;
    unsigned char *chunk;
    unsigned char *grown;
    char *type;
    size_t length;
    size_t capacity;
    size_t count;
    size_t next;
    int claimed = 0;
    knurl_status result;
    knurl_status reset_result;
    result = prepare_type(reply, status, content_type, &type, error);
    if (result != KNURL_OK)
        return result;
    if (source == NULL || source->owner != NULL || source->busy) {
        free(type);
        return knurl_fail(error, source == NULL ? KNURL_INVALID : KNURL_STATE,
                          "reply_source_materialized",
                          "provide an unowned source which is not busy");
    }
    knurl_source_materialize_config_init(&effective);
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
        return knurl_fail(error, KNURL_NOMEM, "reply_source_materialized",
                          "cannot allocate source chunk");
    }
    reply->busy = 1;
    result =
        effective.reset_before ? knurl_source_reset(source, error) : KNURL_OK;
    if (result == KNURL_OK) {
        source->owner = reply;
        claimed = 1;
    }
    while (result == KNURL_OK) {
        result = knurl_source_pull(source, reply, chunk, effective.chunk_bytes,
                                   &count, error);
        if (result == KNURL_EOF) {
            result = KNURL_OK;
            break;
        }
        if (result == KNURL_AGAIN)
            result = knurl_fail(
                error, KNURL_NOT_SUPPORTED, "reply_source_materialized",
                "synchronous materialization cannot wait on a paused source");
        if (result != KNURL_OK)
            break;
        if (count > effective.max_bytes - length) {
            result =
                knurl_fail(error, KNURL_LIMIT, "reply_source_materialized",
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
                result =
                    knurl_fail(error, KNURL_NOMEM, "reply_source_materialized",
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
            knurl_source_reset(source, result == KNURL_OK ? error : NULL);
        source->owner = reply;
        claimed = 1;
        if (result == KNURL_OK)
            result = reset_result;
    }
    reply->busy = 0;
    free(chunk);
    if (result != KNURL_OK) {
        if (claimed)
            source->owner = NULL;
        free(body);
        free(type);
        return result;
    }
    select_payload(reply, status, KNURL_REPLY_BYTES, type);
    reply->body = body;
    reply->body_length = length;
    if (claimed)
        source->owner = NULL;
    knurl_error_clear(error);
    return KNURL_OK;
}

int knurl_reply_is_selected(const knurl_reply *reply)
{
    return reply != NULL && reply->selected;
}

int knurl_reply_status_code(const knurl_reply *reply)
{
    return reply != NULL ? reply->status : 0;
}

knurl_reply_kind knurl_reply_payload_kind(const knurl_reply *reply)
{
    return reply != NULL ? reply->kind : KNURL_REPLY_EMPTY;
}

knurl_bytes knurl_reply_content_type(const knurl_reply *reply)
{
    knurl_bytes value;
    value.data = reply != NULL ? reply->content_type : NULL;
    value.length = value.data != NULL ? strlen(value.data) : 0;
    return value;
}

knurl_bytes knurl_reply_body(const knurl_reply *reply)
{
    knurl_bytes value;
    value.data = reply != NULL ? reply->body : NULL;
    value.length = reply != NULL ? reply->body_length : 0;
    return value;
}

size_t knurl_reply_header_count(const knurl_reply *reply)
{
    return reply != NULL ? reply->header_count : 0;
}

knurl_status knurl_reply_header_at(const knurl_reply *reply, size_t index,
                                   knurl_header *out, knurl_error *error)
{
    if (reply == NULL || out == NULL || index >= reply->header_count)
        return knurl_fail(error, KNURL_INVALID, "reply_header_at",
                          "header index is outside this reply");
    *out = reply->headers[index];
    knurl_error_clear(error);
    return KNURL_OK;
}
