#include "internal.h"
#include <knurl/http.h>

#include <string.h>

typedef struct sse_output {
    unsigned char *buffer;
    size_t length;
    int overflow;
} sse_output;

void knurl_sse_event_init(knurl_sse_event *event)
{
    if (event == NULL)
        return;
    event->comment.data = event->id.data = event->event.data = NULL;
    event->data.data = event->frame.data = NULL;
    event->comment.length = event->id.length = event->event.length = 0;
    event->data.length = event->frame.length = 0;
    event->retry_ms.high = event->retry_ms.low = 0;
    event->has_comment = event->has_id = event->has_event = 0;
    event->has_data = event->has_frame = event->has_retry = 0;
}

static void append(sse_output *out, const void *data, size_t length)
{
    if (length > (size_t)-1 - out->length) {
        out->overflow = 1;
        return;
    }
    if (out->buffer != NULL && length > 0)
        memcpy(out->buffer + out->length, data, length);
    out->length += length;
}

static void lines(sse_output *out, const char *prefix, knurl_bytes value)
{
    const unsigned char *data;
    size_t start;
    size_t end;
    size_t length;
    data = value.data;
    start = 0;
    for (;;) {
        end = start;
        while (end < value.length && data[end] != '\n' && data[end] != '\r')
            ++end;
        length = end - start;
        append(out, prefix, strlen(prefix));
        if (length > 0)
            append(out, data + start, length);
        append(out, "\n", 1);
        if (end == value.length)
            break;
        start = end + 1;
        if (data[end] == '\r' && start < value.length && data[start] == '\n')
            ++start;
    }
}

static int valid_view(knurl_bytes value)
{
    return value.data != NULL || value.length == 0;
}

static void encode(sse_output *out, const knurl_sse_event *event,
                   const char *retry, size_t retry_length)
{
    if (event->has_frame) {
        append(out, event->frame.data, event->frame.length);
        return;
    }
    if (event->has_comment)
        lines(out, ": ", event->comment);
    if (event->has_id)
        lines(out, "id: ", event->id);
    if (event->has_event)
        lines(out, "event: ", event->event);
    if (event->has_retry) {
        append(out, "retry: ", 7);
        append(out, retry, retry_length);
        append(out, "\n", 1);
    }
    if (event->has_data)
        lines(out, "data: ", event->data);
    append(out, "\n", 1);
}

knurl_status knurl_sse_encode(const knurl_sse_event *event, void *buffer,
                              size_t capacity, size_t *written,
                              knurl_error *error)
{
    sse_output out;
    char retry[21];
    size_t retry_length;
    knurl_status status;
    if (written != NULL)
        *written = 0;
    if (event == NULL || written == NULL || (buffer == NULL && capacity != 0))
        return knurl_fail(
            error, KNURL_INVALID, "sse_encode",
            "provide an event, output capacity and length output");
    if ((event->has_frame && !valid_view(event->frame)) ||
        (!event->has_frame &&
         ((event->has_comment && !valid_view(event->comment)) ||
          (event->has_id && !valid_view(event->id)) ||
          (event->has_event && !valid_view(event->event)) ||
          (event->has_data && !valid_view(event->data)))))
        return knurl_fail(error, KNURL_INVALID, "sse_encode",
                          "nonempty event fields require data");
    retry_length = 0;
    if (!event->has_frame && event->has_retry) {
        status = knurl_u64_format_decimal(event->retry_ms, retry, sizeof(retry),
                                          &retry_length, error);
        if (status != KNURL_OK)
            return status;
    }
    out.buffer = NULL;
    out.length = 0;
    out.overflow = 0;
    encode(&out, event, retry, retry_length);
    if (out.overflow)
        return knurl_fail(error, KNURL_OVERFLOW, "sse_encode",
                          "event size overflow");
    if (out.length > capacity)
        return knurl_fail(error, KNURL_LIMIT, "sse_encode",
                          "event exceeds the provided frame capacity");
    out.buffer = buffer;
    out.length = 0;
    encode(&out, event, retry, retry_length);
    *written = out.length;
    knurl_error_clear(error);
    return KNURL_OK;
}
