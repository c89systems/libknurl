#include "internal.h"

#include <string.h>

const char *rotmos_version(void) { return ROTMOS_VERSION; }

void rotmos_error_clear(rotmos_error *error)
{
    if (error != NULL) {
        error->status = ROTMOS_OK;
        error->source = ROTMOS_ERROR_NONE;
        error->native_code = 0;
        memset(error->operation, 0, sizeof(error->operation));
        memset(error->message, 0, sizeof(error->message));
        memset(error->detail, 0, sizeof(error->detail));
    }
}

const char *rotmos_status_string(rotmos_status status)
{
    switch (status) {
    case ROTMOS_OK:
        return "ok";
    case ROTMOS_AGAIN:
        return "again";
    case ROTMOS_EOF:
        return "end of stream";
    case ROTMOS_INVALID:
        return "invalid argument";
    case ROTMOS_NOMEM:
        return "out of memory";
    case ROTMOS_STATE:
        return "invalid state";
    case ROTMOS_CONFLICT:
        return "conflict";
    case ROTMOS_LIMIT:
        return "limit exceeded";
    case ROTMOS_OVERFLOW:
        return "numeric overflow";
    case ROTMOS_TIMEOUT:
        return "timeout";
    case ROTMOS_IO:
        return "I/O error";
    case ROTMOS_NOT_SUPPORTED:
        return "not supported";
    case ROTMOS_CANCELLED:
        return "cancelled";
    case ROTMOS_BACKEND:
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

rotmos_status rotmos_fail(rotmos_error *error, rotmos_status status,
                          const char *operation, const char *message)
{
    rotmos_error_clear(error);
    if (error != NULL) {
        error->status = status;
        error->source = ROTMOS_ERROR_CORE;
        copy_text(error->operation, sizeof(error->operation), operation);
        copy_text(error->message, sizeof(error->message), message);
    }
    return status;
}
