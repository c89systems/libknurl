#include "json_internal.h"
#include <stdlib.h>
#include <string.h>
lonejson_status knurl_json_buffer_write(void *data, const void *bytes,
                                        size_t count, lonejson_error *error)
{
    struct knurl_json_buffer *s;
    size_t capacity;
    void *grown;
    (void)error;
    s = data;
    if (count > s->bound - s->bytes.length) {
        s->failure = KNURL_LIMIT;
        return LONEJSON_STATUS_OVERFLOW;
    }
    if (count > s->capacity - s->bytes.length) {
        capacity = s->capacity == 0 ? 1024 : s->capacity;
        while (capacity < s->bytes.length + count)
            capacity = capacity > s->bound / 2 ? s->bound : capacity * 2;
        if (capacity > s->bound)
            capacity = s->bound;
        grown = realloc(s->bytes.data, capacity);
        if (grown == NULL) {
            s->failure = KNURL_NOMEM;
            return LONEJSON_STATUS_ALLOCATION_FAILED;
        }
        s->bytes.data = grown;
        s->capacity = capacity;
    }
    if (count != 0)
        memcpy((unsigned char *)s->bytes.data + s->bytes.length, bytes, count);
    s->bytes.length += count;
    return LONEJSON_STATUS_OK;
}
