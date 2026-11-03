#include <rotmos/rotmos.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    static const char expected[] = "id: 7\ndata: first\ndata: second\n\n";
    rotmos_u64 maximum;
    rotmos_u64 one;
    rotmos_u64 result;
    rotmos_error error;
    rotmos_sse_event event;
    char buffer[128];
    size_t length;

    if (rotmos_u64_from_decimal("18446744073709551615", 20, &maximum, &error) !=
            ROTMOS_OK ||
        rotmos_u64_format_decimal(maximum, buffer, sizeof(buffer), &length,
                                  &error) != ROTMOS_OK ||
        strcmp(buffer, "18446744073709551615") != 0)
        return 1;
    if (rotmos_u64_from_size(1, &one, &error) != ROTMOS_OK ||
        rotmos_u64_add(maximum, one, &result, &error) != ROTMOS_OVERFLOW)
        return 2;
    rotmos_sse_event_init(&event);
    event.has_id = event.has_data = 1;
    event.id.data = "7";
    event.id.length = 1;
    event.data.data = "first\r\nsecond";
    event.data.length = 13;
    if (rotmos_sse_encode(&event, buffer, sizeof(buffer), &length, &error) !=
            ROTMOS_OK ||
        length != sizeof(expected) - 1 || memcmp(buffer, expected, length) != 0)
        return 3;
    memset(buffer, 'x', sizeof(buffer));
    if (rotmos_sse_encode(&event, buffer, 2, &length, &error) != ROTMOS_LIMIT ||
        buffer[0] != 'x' || length != 0)
        return 4;
    (void)printf("librotmos %s: wide counters and SSE framing passed\n",
                 rotmos_version());
    return 0;
}
