#include "internal.h"
#include <stdlib.h>
#include <string.h>

void knurl_owned_bytes_init(knurl_owned_bytes *bytes)
{
    if (bytes != NULL) {
        bytes->data = NULL;
        bytes->length = 0;
    }
}

void knurl_owned_bytes_cleanup(knurl_owned_bytes *bytes)
{
    if (bytes != NULL) {
        free(bytes->data);
        knurl_owned_bytes_init(bytes);
    }
}

const char *knurl_version(void) { return KNURL_VERSION; }

void knurl_error_clear(knurl_error *error)
{
    if (error != NULL) {
        error->status = KNURL_OK;
        error->source = KNURL_ERROR_NONE;
        error->native_code = 0;
        memset(error->operation, 0, sizeof(error->operation));
        memset(error->message, 0, sizeof(error->message));
        memset(error->detail, 0, sizeof(error->detail));
    }
}

const char *knurl_status_string(knurl_status status)
{
    switch (status) {
    case KNURL_OK:
        return "ok";
    case KNURL_AGAIN:
        return "again";
    case KNURL_EOF:
        return "end of stream";
    case KNURL_INVALID:
        return "invalid argument";
    case KNURL_NOMEM:
        return "out of memory";
    case KNURL_STATE:
        return "invalid state";
    case KNURL_CONFLICT:
        return "conflict";
    case KNURL_LIMIT:
        return "limit exceeded";
    case KNURL_OVERFLOW:
        return "numeric overflow";
    case KNURL_TIMEOUT:
        return "timeout";
    case KNURL_IO:
        return "I/O error";
    case KNURL_NOT_SUPPORTED:
        return "not supported";
    case KNURL_CANCELLED:
        return "cancelled";
    case KNURL_BACKEND:
        return "backend error";
    }
    return "unknown status";
}

static void copy_text(char *destination, size_t capacity, const char *text)
{
    size_t length;
    length = strlen(text);
    if (length >= capacity)
        length = capacity - 1;
    memcpy(destination, text, length);
    destination[length] = '\0';
}

knurl_status knurl_fail(knurl_error *error, knurl_status status,
                        const char *operation, const char *message)
{
    knurl_error_clear(error);
    if (error != NULL) {
        error->status = status;
        error->source = KNURL_ERROR_CORE;
        copy_text(error->operation, sizeof(error->operation), operation);
        copy_text(error->message, sizeof(error->message), message);
    }
    return status;
}
