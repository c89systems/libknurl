#include "source_internal.h"
#include "loop_internal.h"

#include <stdlib.h>
#include <string.h>

void knurl_source_config_init(knurl_source_config *config)
{
    if (config == NULL)
        return;
    config->read = NULL;
    config->reset = NULL;
    config->reset_before_reply = 0;
    config->close = NULL;
    config->userdata = NULL;
    config->execution = KNURL_SOURCE_OWNER_LOOP;
    config->length.high = config->length.low = 0;
    config->has_length = 0;
}

knurl_status knurl_source_new(const knurl_source_config *config,
                              knurl_source **out, knurl_error *error)
{
    knurl_source *source;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL || config->read == NULL ||
        (config->execution != KNURL_SOURCE_OWNER_LOOP &&
         config->execution != KNURL_SOURCE_NATIVE_READER) ||
        (config->has_length && !knurl_u64_valid(config->length)))
        return knurl_fail(error, KNURL_INVALID, "source_new",
                          "provide callbacks, an execution policy and a valid "
                          "optional length");
    source = calloc(1, sizeof(*source));
    if (source == NULL)
        return knurl_fail(error, KNURL_NOMEM, "source_new",
                          "cannot allocate source");
    source->config = *config;
    source->position.high = source->position.low = 0;
    source->owner = NULL;
    source->busy = source->eof = 0;
    source->failure = KNURL_OK;
    *out = source;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_source_pull(knurl_source *source, void *owner, void *buffer,
                               size_t capacity, size_t *produced,
                               knurl_error *error)
{
    knurl_status status;
    size_t count;
    if (produced != NULL)
        *produced = 0;
    if (source == NULL || buffer == NULL || capacity == 0 || produced == NULL)
        return knurl_fail(error, KNURL_INVALID, "source_read",
                          "provide source, buffer and positive capacity");
    if (source->busy || source->owner != owner || source->failure != KNURL_OK)
        return knurl_fail(error, KNURL_STATE, "source_read",
                          "source is owned, busy or failed");
    knurl_error_clear(error);
    if (source->eof)
        return KNURL_EOF;
    count = 0;
    source->busy = 1;
    status = source->config.read(buffer, capacity, &count,
                                 source->config.userdata, error);
    source->busy = 0;
    status = knurl_source_validate(source, capacity, count, status, error);
    if (status == KNURL_OK)
        *produced = count;
    return status;
}
knurl_status knurl_source_validate(knurl_source *source, size_t capacity,
                                   size_t count, knurl_status status,
                                   knurl_error *error)
{
    knurl_u64 amount, next;
    int order;
    if (status < KNURL_OK || status > KNURL_BACKEND) {
        source->failure = KNURL_INVALID;
        return knurl_fail(error, KNURL_INVALID, "source_read",
                          "callback returned an unknown status");
    }
    if ((status == KNURL_OK && (count == 0 || count > capacity)) ||
        (status != KNURL_OK && count != 0)) {
        source->failure = KNURL_INVALID;
        return knurl_fail(error, KNURL_INVALID, "source_read",
                          "callback returned an invalid byte count");
    }
    if (status == KNURL_AGAIN || status == KNURL_EOF) {
        if (status == KNURL_EOF) {
            if (source->config.has_length &&
                (source->position.high != source->config.length.high ||
                 source->position.low != source->config.length.low)) {
                source->failure = KNURL_IO;
                return knurl_fail(error, KNURL_IO, "source_read",
                                  "source ended before its declared length");
            }
            source->eof = 1;
        }
        knurl_error_clear(error);
        return status;
    }
    if (status != KNURL_OK) {
        source->failure = status;
        if (error == NULL || error->status != status)
            return knurl_fail(error, status, "source_read",
                              "source callback failed");
        return status;
    }
    status = knurl_u64_from_size(count, &amount, error);
    if (status == KNURL_OK)
        status = knurl_u64_add(source->position, amount, &next, error);
    if (status == KNURL_OK && source->config.has_length) {
        status = knurl_u64_compare(next, source->config.length, &order, error);
        if (status == KNURL_OK && order > 0)
            status = knurl_fail(error, KNURL_IO, "source_read",
                                "source exceeded its declared length");
    }
    if (status != KNURL_OK) {
        source->failure = status;
        return status;
    }
    source->position = next;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_source_read(knurl_source *source, void *buffer,
                               size_t capacity, size_t *produced,
                               knurl_error *error)
{
    return knurl_source_pull(source, NULL, buffer, capacity, produced, error);
}

knurl_status knurl_source_reset(knurl_source *source, knurl_error *error)
{
    knurl_status status;
    if (source == NULL)
        return knurl_fail(error, KNURL_INVALID, "source_reset",
                          "source is required");
    if (source->borrowed || source->busy || source->owner != NULL)
        return knurl_fail(error, KNURL_STATE, "source_reset",
                          "source is owned or busy");
    if (source->config.reset == NULL)
        return knurl_fail(error, KNURL_NOT_SUPPORTED, "source_reset",
                          "source has no reset callback");
    source->busy = 1;
    knurl_error_clear(error);
    status = source->config.reset(source->config.userdata, error);
    source->busy = 0;
    if (status < KNURL_OK || status > KNURL_BACKEND)
        status = KNURL_INVALID;
    if (status != KNURL_OK) {
        if (status == KNURL_AGAIN || status == KNURL_EOF)
            status = KNURL_INVALID;
        source->failure = status;
        if (error == NULL || error->status != status)
            return knurl_fail(error, status, "source_reset",
                              "source reset failed");
        return status;
    }
    source->position.high = source->position.low = 0;
    source->eof = 0;
    source->failure = KNURL_OK;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_source_release_now(knurl_source *source, knurl_status outcome)
{
    source->busy = 1;
    if (source->config.close != NULL)
        source->config.close(source->config.userdata, outcome);
    free(source);
}

void knurl_source_revoke(knurl_source *source)
{
    __atomic_store_n(&source->cancelled, 1u, __ATOMIC_RELEASE);
    source->resume = NULL;
    source->resume_data = NULL;
    if (source->native != NULL)
        knurl_source_native_revoke(source);
}
void knurl_source_release(knurl_source *source, knurl_status outcome)
{
    __atomic_store_n(&source->cancelled, 1u, __ATOMIC_RELEASE);
    source->resume = NULL;
    source->resume_data = NULL;
    if (source->native != NULL) {
        knurl_source_native_retire(source, outcome);
        return;
    }
    knurl_source_release_now(source, outcome);
}
knurl_status knurl_source_resume(knurl_source *source, knurl_error *error)
{
    knurl_status status;
    if (source == NULL || source->loop == NULL)
        return knurl_fail(error, KNURL_STATE, "source_resume",
                          "source has no live runtime owner");
    status = knurl_loop_owner(source->loop, error);
    if (status != KNURL_OK)
        return status;
    if (source->resume == NULL ||
        __atomic_load_n(&source->cancelled, __ATOMIC_ACQUIRE))
        return knurl_fail(error, KNURL_STATE, "source_resume",
                          "source owner is retired");
    source->resume_requested = 1;
    if (source->native != NULL)
        knurl_source_native_resume(source);
    source->resume(source->resume_data);
    knurl_error_clear(error);
    return KNURL_OK;
}
int knurl_source_cancelled(const knurl_source *source)
{
    return source == NULL ||
           __atomic_load_n(&source->cancelled, __ATOMIC_ACQUIRE) ||
           (source->native_job != NULL &&
            knurl_job_cancelled(source->native_job));
}
knurl_status knurl_source_destroy(knurl_source *source, knurl_error *error)
{
    knurl_status outcome;
    if (source != NULL) {
        if (source->borrowed || source->busy || source->owner != NULL)
            return knurl_fail(error, KNURL_STATE, "source_destroy",
                              "source is owned or busy");
        outcome = source->failure;
        if (outcome == KNURL_OK && !source->eof)
            outcome = KNURL_CANCELLED;
        knurl_source_release(source, outcome);
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

typedef struct bytes_source {
    unsigned char *data;
    size_t length;
    size_t position;
} bytes_source;

static knurl_status bytes_read(void *buffer, size_t capacity, size_t *produced,
                               void *userdata, knurl_error *error)
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
    knurl_error_clear(error);
    return length == 0 ? KNURL_EOF : KNURL_OK;
}

static knurl_status bytes_reset(void *userdata, knurl_error *error)
{
    bytes_source *bytes;
    bytes = userdata;
    bytes->position = 0;
    knurl_error_clear(error);
    return KNURL_OK;
}

static void bytes_close(void *userdata, knurl_status outcome)
{
    bytes_source *bytes;
    (void)outcome;
    bytes = userdata;
    free(bytes->data);
    free(bytes);
}

knurl_status knurl_source_bytes(knurl_bytes bytes, knurl_source **out,
                                knurl_error *error)
{
    bytes_source *copy;
    knurl_source_config config;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (out == NULL || (bytes.data == NULL && bytes.length != 0))
        return knurl_fail(error, KNURL_INVALID, "source_bytes",
                          "provide bytes and a source output");
    copy = malloc(sizeof(*copy));
    if (copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "source_bytes",
                          "cannot allocate byte source");
    copy->data = NULL;
    copy->length = bytes.length;
    copy->position = 0;
    if (bytes.length != 0) {
        copy->data = malloc(bytes.length);
        if (copy->data == NULL) {
            free(copy);
            return knurl_fail(error, KNURL_NOMEM, "source_bytes",
                              "cannot copy byte source");
        }
        memcpy(copy->data, bytes.data, bytes.length);
    }
    knurl_source_config_init(&config);
    config.read = bytes_read;
    config.reset = bytes_reset;
    config.close = bytes_close;
    config.userdata = copy;
    config.has_length = 1;
    status = knurl_u64_from_size(bytes.length, &config.length, error);
    if (status == KNURL_OK)
        status = knurl_source_new(&config, out, error);
    if (status != KNURL_OK)
        bytes_close(copy, status);
    return status;
}
