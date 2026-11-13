#include <knurl/knurl.h>

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
    knurl_sse_event event;
    knurl_error error;
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
            knurl_sse_event_init(&event);
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
            if (knurl_sse_encode(&event, buffer, expected_length, &length,
                                 &error) != KNURL_OK ||
                length != expected_length || memcmp(buffer, expected, length) ||
                buffer[length] != 'x') {
                fprintf(stderr, "SSE line case %lu, comment=%d failed\n",
                        (unsigned long)i, comment);
                return 1;
            }
            memcpy(buffer, untouched, sizeof(buffer));
            if (knurl_sse_encode(&event, buffer, expected_length - 1, &length,
                                 &error) != KNURL_LIMIT ||
                length != 0 || memcmp(buffer, untouched, sizeof(buffer)))
                return 1;
        }
    }
    return 0;
}

int main(void)
{
    static const char expected[] = "id: 7\ndata: first\ndata: second\n\n";
    knurl_u64 maximum;
    knurl_u64 one;
    knurl_u64 result;
    knurl_error error;
    knurl_sse_event event;
    char buffer[128];
    size_t length;

    knurl_bytes input;
    long integer;
    input.data = "42";
    input.length = 2;
    if (knurl_input_long(input, 0, 100, &integer, &error) != KNURL_OK ||
        integer != 42)
        return 6;
    input.data = "https://example.org";
    input.length = strlen(input.data);
    if (knurl_input_http_url(input, &error) != KNURL_OK)
        return 7;

    if (knurl_u64_from_decimal("18446744073709551615", 20, &maximum, &error) !=
            KNURL_OK ||
        knurl_u64_format_decimal(maximum, buffer, sizeof(buffer), &length,
                                 &error) != KNURL_OK ||
        strcmp(buffer, "18446744073709551615") != 0)
        return 1;
    if (knurl_u64_from_size(1, &one, &error) != KNURL_OK ||
        knurl_u64_add(maximum, one, &result, &error) != KNURL_OVERFLOW)
        return 2;
    knurl_sse_event_init(&event);
    event.has_id = event.has_data = 1;
    event.id.data = "7";
    event.id.length = 1;
    event.data.data = "first\r\nsecond";
    event.data.length = 13;
    if (knurl_sse_encode(&event, buffer, sizeof(buffer), &length, &error) !=
            KNURL_OK ||
        length != sizeof(expected) - 1 || memcmp(buffer, expected, length) != 0)
        return 3;
    memset(buffer, 'x', sizeof(buffer));
    if (knurl_sse_encode(&event, buffer, 2, &length, &error) != KNURL_LIMIT ||
        buffer[0] != 'x' || length != 0)
        return 4;
    if (sse_lines() != 0)
        return 5;
    (void)printf("libknurl %s: wide counters and SSE framing passed\n",
                 knurl_version());
    return 0;
}
