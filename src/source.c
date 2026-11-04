#include "source_internal.h"
#include "loop_internal.h"

#include <stdlib.h>
#include <string.h>

void rotmos_source_config_init(rotmos_source_config *config)
{
    if (config == NULL)
        return;
    config->read = NULL;
    config->reset = NULL;
    config->close = NULL;
    config->userdata = NULL;
    config->execution = ROTMOS_SOURCE_OWNER_LOOP;
    config->length.high = config->length.low = 0;
    config->has_length = 0;
}

rotmos_status rotmos_source_new(const rotmos_source_config *config,
                                rotmos_source **out, rotmos_error *error)
{
    rotmos_source *source;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL || config->read == NULL ||
        (config->execution != ROTMOS_SOURCE_OWNER_LOOP &&
         config->execution != ROTMOS_SOURCE_NATIVE_READER) ||
        (config->has_length && !rotmos_u64_valid(config->length)))
        return rotmos_fail(error, ROTMOS_INVALID, "source_new",
                           "provide callbacks, an execution policy and a valid "
                           "optional length");
    source = calloc(1, sizeof(*source));
    if (source == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "source_new",
                           "cannot allocate source");
    source->config = *config;
    source->position.high = source->position.low = 0;
    source->owner = NULL;
    source->busy = source->eof = 0;
    source->failure = ROTMOS_OK;
    *out = source;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_source_pull(rotmos_source *source, void *owner,
                                 void *buffer, size_t capacity,
                                 size_t *produced, rotmos_error *error)
{
    rotmos_status status;
    size_t count;
    if (produced != NULL)
        *produced = 0;
    if (source == NULL || buffer == NULL || capacity == 0 || produced == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "source_read",
                           "provide source, buffer and positive capacity");
    if (source->busy || source->owner != owner || source->failure != ROTMOS_OK)
        return rotmos_fail(error, ROTMOS_STATE, "source_read",
                           "source is owned, busy or failed");
    rotmos_error_clear(error);
    if (source->eof)
        return ROTMOS_EOF;
    count = 0;
    source->busy = 1;
    status = source->config.read(buffer, capacity, &count,
                                 source->config.userdata, error);
    source->busy = 0;
    status = rotmos_source_validate(source, capacity, count, status, error);
    if (status == ROTMOS_OK)
        *produced = count;
    return status;
}
rotmos_status rotmos_source_validate(rotmos_source *source, size_t capacity,
                                     size_t count, rotmos_status status,
                                     rotmos_error *error)
{
    rotmos_u64 amount, next;
    int order;
    if (status < ROTMOS_OK || status > ROTMOS_BACKEND) {
        source->failure = ROTMOS_INVALID;
        return rotmos_fail(error, ROTMOS_INVALID, "source_read",
                           "callback returned an unknown status");
    }
    if ((status == ROTMOS_OK && (count == 0 || count > capacity)) ||
        (status != ROTMOS_OK && count != 0)) {
        source->failure = ROTMOS_INVALID;
        return rotmos_fail(error, ROTMOS_INVALID, "source_read",
                           "callback returned an invalid byte count");
    }
    if (status == ROTMOS_AGAIN || status == ROTMOS_EOF) {
        if (status == ROTMOS_EOF) {
            if (source->config.has_length &&
                (source->position.high != source->config.length.high ||
                 source->position.low != source->config.length.low)) {
                source->failure = ROTMOS_IO;
                return rotmos_fail(error, ROTMOS_IO, "source_read",
                                   "source ended before its declared length");
            }
            source->eof = 1;
        }
        rotmos_error_clear(error);
        return status;
    }
    if (status != ROTMOS_OK) {
        source->failure = status;
        if (error == NULL || error->status != status)
            return rotmos_fail(error, status, "source_read",
                               "source callback failed");
        return status;
    }
    status = rotmos_u64_from_size(count, &amount, error);
    if (status == ROTMOS_OK)
        status = rotmos_u64_add(source->position, amount, &next, error);
    if (status == ROTMOS_OK && source->config.has_length) {
        status = rotmos_u64_compare(next, source->config.length, &order, error);
        if (status == ROTMOS_OK && order > 0)
            status = rotmos_fail(error, ROTMOS_IO, "source_read",
                                 "source exceeded its declared length");
    }
    if (status != ROTMOS_OK) {
        source->failure = status;
        return status;
    }
    source->position = next;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_source_read(rotmos_source *source, void *buffer,
                                 size_t capacity, size_t *produced,
                                 rotmos_error *error)
{
    return rotmos_source_pull(source, NULL, buffer, capacity, produced, error);
}

rotmos_status rotmos_source_reset(rotmos_source *source, rotmos_error *error)
{
    rotmos_status status;
    if (source == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "source_reset",
                           "source is required");
    if (source->busy || source->owner != NULL)
        return rotmos_fail(error, ROTMOS_STATE, "source_reset",
                           "source is owned or busy");
    if (source->config.reset == NULL)
        return rotmos_fail(error, ROTMOS_NOT_SUPPORTED, "source_reset",
                           "source has no reset callback");
    source->busy = 1;
    rotmos_error_clear(error);
    status = source->config.reset(source->config.userdata, error);
    source->busy = 0;
    if (status < ROTMOS_OK || status > ROTMOS_BACKEND)
        status = ROTMOS_INVALID;
    if (status != ROTMOS_OK) {
        if (status == ROTMOS_AGAIN || status == ROTMOS_EOF)
            status = ROTMOS_INVALID;
        source->failure = status;
        if (error == NULL || error->status != status)
            return rotmos_fail(error, status, "source_reset",
                               "source reset failed");
        return status;
    }
    source->position.high = source->position.low = 0;
    source->eof = 0;
    source->failure = ROTMOS_OK;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_source_release_now(rotmos_source *source, rotmos_status outcome)
{
    source->busy = 1;
    if (source->config.close != NULL)
        source->config.close(source->config.userdata, outcome);
    free(source);
}

void rotmos_source_revoke(rotmos_source *source)
{
    __atomic_store_n(&source->cancelled, 1u, __ATOMIC_RELEASE);
    source->resume = NULL;
    source->resume_data = NULL;
    if (source->native != NULL)
        rotmos_source_native_revoke(source);
}
void rotmos_source_release(rotmos_source *source, rotmos_status outcome)
{
    __atomic_store_n(&source->cancelled, 1u, __ATOMIC_RELEASE);
    source->resume = NULL;
    source->resume_data = NULL;
    if (source->native != NULL) {
        rotmos_source_native_retire(source, outcome);
        return;
    }
    rotmos_source_release_now(source, outcome);
}
rotmos_status rotmos_source_resume(rotmos_source *source, rotmos_error *error)
{
    rotmos_status status;
    if (source == NULL || source->loop == NULL)
        return rotmos_fail(error, ROTMOS_STATE, "source_resume",
                           "source has no live runtime owner");
    status = rotmos_loop_owner(source->loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (source->resume == NULL ||
        __atomic_load_n(&source->cancelled, __ATOMIC_ACQUIRE))
        return rotmos_fail(error, ROTMOS_STATE, "source_resume",
                           "source owner is retired");
    source->resume_requested = 1;
    if (source->native != NULL)
        rotmos_source_native_resume(source);
    source->resume(source->resume_data);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
int rotmos_source_cancelled(const rotmos_source *source)
{
    return source == NULL ||
           __atomic_load_n(&source->cancelled, __ATOMIC_ACQUIRE) ||
           (source->native_job != NULL &&
            rotmos_job_cancelled(source->native_job));
}
rotmos_status rotmos_source_destroy(rotmos_source *source, rotmos_error *error)
{
    rotmos_status outcome;
    if (source != NULL) {
        if (source->busy || source->owner != NULL)
            return rotmos_fail(error, ROTMOS_STATE, "source_destroy",
                               "source is owned or busy");
        outcome = source->failure;
        if (outcome == ROTMOS_OK && !source->eof)
            outcome = ROTMOS_CANCELLED;
        rotmos_source_release(source, outcome);
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

typedef struct bytes_source {
    unsigned char *data;
    size_t length;
    size_t position;
} bytes_source;

static rotmos_status bytes_read(void *buffer, size_t capacity, size_t *produced,
                                void *userdata, rotmos_error *error)
{
    bytes_source *bytes;
    size_t length;
    bytes = userdata;
    length = bytes->length - bytes->position;
    if (length > capacity)
        length = capacity;
    if (length != 0)
        memcpy(buffer, bytes->data + bytes->position, length);
    bytes->position += length;
    *produced = length;
    rotmos_error_clear(error);
    return length == 0 ? ROTMOS_EOF : ROTMOS_OK;
}

static rotmos_status bytes_reset(void *userdata, rotmos_error *error)
{
    bytes_source *bytes;
    bytes = userdata;
    bytes->position = 0;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static void bytes_close(void *userdata, rotmos_status outcome)
{
    bytes_source *bytes;
    (void)outcome;
    bytes = userdata;
    free(bytes->data);
    free(bytes);
}

rotmos_status rotmos_source_bytes(rotmos_bytes bytes, rotmos_source **out,
                                  rotmos_error *error)
{
    bytes_source *copy;
    rotmos_source_config config;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (out == NULL || (bytes.data == NULL && bytes.length != 0))
        return rotmos_fail(error, ROTMOS_INVALID, "source_bytes",
                           "provide bytes and a source output");
    copy = malloc(sizeof(*copy));
    if (copy == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "source_bytes",
                           "cannot allocate byte source");
    copy->data = NULL;
    copy->length = bytes.length;
    copy->position = 0;
    if (bytes.length != 0) {
        copy->data = malloc(bytes.length);
        if (copy->data == NULL) {
            free(copy);
            return rotmos_fail(error, ROTMOS_NOMEM, "source_bytes",
                               "cannot copy byte source");
        }
        memcpy(copy->data, bytes.data, bytes.length);
    }
    rotmos_source_config_init(&config);
    config.read = bytes_read;
    config.reset = bytes_reset;
    config.close = bytes_close;
    config.userdata = copy;
    config.has_length = 1;
    status = rotmos_u64_from_size(bytes.length, &config.length, error);
    if (status == ROTMOS_OK)
        status = rotmos_source_new(&config, out, error);
    if (status != ROTMOS_OK)
        bytes_close(copy, status);
    return status;
}
