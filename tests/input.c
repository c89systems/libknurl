#include <knurl/knurl.h>
#include "request_internal.h"
#include "receivers_internal.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "input %d: %s\n", __LINE__, #t);                   \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static knurl_bytes text(const char *s)
{
    knurl_bytes b;
    b.data = s;
    b.length = strlen(s);
    return b;
}
static int validators(void)
{
    static const char *const bad_numbers[] = {"",   "-",   "+1",  " 1",
                                              "1 ", "1.0", "1e1", "1x"};
    static const char *const bad_utf8[] = {
        "\300\200", "\355\240\200", "\364\220\200\200", "\200", "\342\202"};
    static const char *const good_email[] = {
        "a@example.org", "user+tag@sub.example", "a!b@localhost"};
    static const char *const bad_email[] = {
        "a",        "@host",   "a@",           ".a@host", "a..b@host",
        "a.@host",  "a@-host", "a@host-",      "a@x..y",  "a@x_y",
        "a b@host", "a@b@c",   "a@h\303\266st"};
    static const char *const good_url[] = {"https://example.org/path?q=1",
                                           "HTTP://example.org:80/",
                                           "http://[::1]:8080/"};
    static const char *const bad_url[] = {"example.org",
                                          "ftp://example.org",
                                          "http:///example.org",
                                          "https://",
                                          "https://user:pass@example.org",
                                          "http://host:99999",
                                          "http://host/with space",
                                          "http://host/\n"};
    knurl_error e;
    knurl_owned_bytes escaped;
    knurl_bytes b, choices[2];
    knurl_u64 zero = {0, 0}, full = {0xffffffffUL, 0xffffffffUL}, count;
    long number = 91;
    int boolean = 7;
    size_t i;
    char bound[64];
    CHECK(knurl_input_long(text("-12"), -12, 12, &number, &e) == KNURL_OK &&
          number == -12);
    CHECK(knurl_input_long(text("13"), -12, 12, &number, &e) == KNURL_INVALID &&
          number == -12);
    sprintf(bound, "%ld", LONG_MIN);
    CHECK(knurl_input_long(text(bound), LONG_MIN, LONG_MAX, &number, &e) ==
              KNURL_OK &&
          number == LONG_MIN);
    sprintf(bound, "%ld", LONG_MAX);
    CHECK(knurl_input_long(text(bound), LONG_MIN, LONG_MAX, &number, &e) ==
              KNURL_OK &&
          number == LONG_MAX);
    strcat(bound, "0");
    CHECK(knurl_input_long(text(bound), LONG_MIN, LONG_MAX, &number, &e) ==
              KNURL_OVERFLOW &&
          number == LONG_MAX);
    for (i = 0; i < sizeof(bad_numbers) / sizeof(bad_numbers[0]); ++i)
        CHECK(knurl_input_long(text(bad_numbers[i]), LONG_MIN, LONG_MAX,
                               &number, &e) == KNURL_INVALID);
    b.data = "12\0junk";
    b.length = 7;
    CHECK(knurl_input_long(b, LONG_MIN, LONG_MAX, &number, &e) ==
          KNURL_INVALID);
    CHECK(knurl_input_u64(text("18446744073709551615"), zero, full, &count,
                          &e) == KNURL_OK &&
          count.high == full.high && count.low == full.low);
    CHECK(knurl_input_u64(text("18446744073709551616"), zero, full, &count,
                          &e) == KNURL_OVERFLOW);
    CHECK(knurl_input_u64(text("-1"), zero, full, &count, &e) == KNURL_INVALID);
    CHECK(knurl_input_u64(text("1"), full, zero, &count, &e) == KNURL_INVALID);
    CHECK(knurl_input_boolean(text("false"), &boolean, &e) == KNURL_OK &&
          boolean == 0);
    CHECK(knurl_input_boolean(text("1"), &boolean, &e) == KNURL_OK &&
          boolean == 1);
    CHECK(knurl_input_boolean(text("TRUE"), &boolean, &e) == KNURL_INVALID &&
          boolean == 1);
    CHECK(knurl_input_text(text("hello \342\202\254"), 1, 9, 1, &e) ==
          KNURL_OK);
    CHECK(knurl_input_text(text("\n"), 0, 9, 1, &e) == KNURL_INVALID);
    CHECK(knurl_input_text(text("\302\205"), 0, 9, 1, &e) == KNURL_INVALID);
    CHECK(knurl_input_text(text("\n"), 0, 9, 0, &e) == KNURL_OK);
    for (i = 0; i < sizeof(bad_utf8) / sizeof(bad_utf8[0]); ++i)
        CHECK(knurl_input_text(text(bad_utf8[i]), 0, 9, 0, &e) ==
              KNURL_INVALID);
    CHECK(knurl_input_allowlist(text("abc"), text("abcd"), 1, 3, &e) ==
          KNURL_OK);
    CHECK(knurl_input_allowlist(text("abc!"), text("abcd"), 1, 9, &e) ==
          KNURL_INVALID);
    choices[0] = text("red");
    choices[1] = text("blue");
    CHECK(knurl_input_choice(text("red"), choices, 2, &e) == KNURL_OK);
    CHECK(knurl_input_choice(text("Red"), choices, 2, &e) == KNURL_INVALID);
    for (i = 0; i < sizeof(good_email) / sizeof(good_email[0]); ++i)
        CHECK(knurl_input_email(text(good_email[i]), &e) == KNURL_OK);
    for (i = 0; i < sizeof(bad_email) / sizeof(bad_email[0]); ++i)
        CHECK(knurl_input_email(text(bad_email[i]), &e) == KNURL_INVALID);
    for (i = 0; i < sizeof(good_url) / sizeof(good_url[0]); ++i)
        CHECK(knurl_input_http_url(text(good_url[i]), &e) == KNURL_OK);
    for (i = 0; i < sizeof(bad_url) / sizeof(bad_url[0]); ++i)
        CHECK(knurl_input_http_url(text(bad_url[i]), &e) == KNURL_INVALID);
    CHECK(knurl_input_filename(text("a_b-c.txt"), &e) == KNURL_OK);
    CHECK(knurl_input_filename(text(".."), &e) == KNURL_INVALID);
    CHECK(knurl_input_filename(text("../a"), &e) == KNURL_INVALID);
    knurl_owned_bytes_init(&escaped);
    CHECK(knurl_input_html_escape(text("<&\"'>"), 24, &escaped, &e) ==
          KNURL_OK);
    CHECK(escaped.length == 24 &&
          memcmp(escaped.data, "&lt;&amp;&quot;&#39;&gt;", 24) == 0);
    knurl_owned_bytes_cleanup(&escaped);
    CHECK(knurl_input_html_escape(text("<&\"'>"), 23, &escaped, &e) ==
              KNURL_LIMIT &&
          escaped.data == NULL);
    CHECK(knurl_input_html_escape(b, 0, &escaped, &e) == KNURL_INVALID);
    return 0;
}
static int requests(void)
{
    struct knurl_request_impl state;
    knurl_header pairs[4], param;
    knurl_request *request;
    knurl_error error;
    knurl_u64 zero = {0, 0}, full = {0xffffffffUL, 0xffffffffUL}, count;
    long value;
    int boolean;
    memset(&state, 0, sizeof(state));
    request = &state.receiver;
    knurl_request_receiver_init(request);
    pairs[0].name = text("n");
    pairs[0].value = text("42");
    pairs[1].name = text("b");
    pairs[1].value = text("false");
    pairs[2].name = text("wide");
    pairs[2].value = text("18446744073709551615");
    pairs[3] = pairs[0];
    state.queries = pairs;
    state.query_count = 3;
    param.name = text("id");
    param.value = text("42");
    state.params = &param;
    state.param_count = 1;
    CHECK(request->query_long(request, "n", 0, 42, &value, &error) ==
              KNURL_OK &&
          value == 42);
    CHECK(request->query_boolean(request, "b", &boolean, &error) == KNURL_OK &&
          boolean == 0);
    CHECK(request->query_u64(request, "wide", zero, full, &count, &error) ==
              KNURL_OK &&
          count.high == full.high);
    CHECK(request->query_long(request, "missing", 0, 42, &value, &error) ==
          KNURL_EOF);
    state.query_count = 4;
    CHECK(request->query_long(request, "n", 0, 42, &value, &error) ==
          KNURL_INVALID);
    CHECK(request->param_long(request, "id", 0, 42, &value, &error) ==
              KNURL_OK &&
          value == 42);
    CHECK(request->param_u64(request, "id", zero, full, &count, &error) ==
              KNURL_OK &&
          count.low == 42);
    CHECK(request->param_boolean(request, "id", &boolean, &error) ==
          KNURL_INVALID);
    CHECK(request->param_long(request, "none", 0, 42, &value, &error) ==
          KNURL_EOF);
    return 0;
}
int main(void)
{
    CHECK(validators() == 0);
    CHECK(requests() == 0);
    return 0;
}
