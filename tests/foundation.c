#include <rotmos/rotmos.h>

#include <stdio.h>
#include <string.h>

static int sse_lines(void)
{
    static const struct {
        const char *input;
        const char *data;
        const char *comment;
    } cases[] = {{"", "data: \n\n", ": \n\n"},
                 {"\r", "data: \ndata: \n\n", ": \n: \n\n"},
                 {"\n", "data: \ndata: \n\n", ": \n: \n\n"},
                 {"\r\n", "data: \ndata: \n\n", ": \n: \n\n"},
                 {"a\rb\nc\r\nd", "data: a\ndata: b\ndata: c\ndata: d\n\n",
                  ": a\n: b\n: c\n: d\n\n"},
                 {"a\r\rb\n\r", "data: a\ndata: \ndata: b\ndata: \ndata: \n\n",
                  ": a\n: \n: b\n: \n: \n\n"},
                 {"hello\revent: injected\rdata: forged",
                  "data: hello\ndata: event: injected\ndata: data: forged\n\n",
                  ": hello\n: event: injected\n: data: forged\n\n"}};
    rotmos_sse_event event;
    rotmos_error error;
    const char *expected;
    char buffer[128];
    char untouched[128];
    size_t i;
    size_t length;
    size_t expected_length;
    int comment;
    memset(untouched, 'x', sizeof(untouched));
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (comment = 0; comment <= 1; ++comment) {
            rotmos_sse_event_init(&event);
            if (comment) {
                event.has_comment = 1;
                event.comment.data = cases[i].input;
                event.comment.length = strlen(cases[i].input);
            } else {
                event.has_data = 1;
                event.data.data = cases[i].input;
                event.data.length = strlen(cases[i].input);
            }
            expected = comment ? cases[i].comment : cases[i].data;
            expected_length = strlen(expected);
            memcpy(buffer, untouched, sizeof(buffer));
            if (rotmos_sse_encode(&event, buffer, expected_length, &length,
                                  &error) != ROTMOS_OK ||
                length != expected_length || memcmp(buffer, expected, length) ||
                buffer[length] != 'x') {
                fprintf(stderr, "SSE line case %lu, comment=%d failed\n",
                        (unsigned long)i, comment);
                return 1;
            }
            memcpy(buffer, untouched, sizeof(buffer));
            if (rotmos_sse_encode(&event, buffer, expected_length - 1, &length,
                                  &error) != ROTMOS_LIMIT ||
                length != 0 || memcmp(buffer, untouched, sizeof(buffer)))
                return 1;
        }
    }
    return 0;
}

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
    if (sse_lines() != 0)
        return 5;
    (void)printf("librotmos %s: wide counters and SSE framing passed\n",
                 rotmos_version());
    return 0;
}
