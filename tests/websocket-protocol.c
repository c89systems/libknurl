#include "websocket_internal.h"
#include <stdio.h>
#include <string.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "websocket protocol %d: %s\n", __LINE__, #t);      \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static int message(void)
{
    struct knurl_websocket_message state;
    static const unsigned char good[] = "a\302\242\342\202\254\360\237\230\200";
    static const unsigned char *const bad[] = {
        (const unsigned char *)"\300\200",
        (const unsigned char *)"\355\240\200",
        (const unsigned char *)"\364\220\200\200",
        (const unsigned char *)"\200",
        (const unsigned char *)"\342\202",
        (const unsigned char *)"\360\200\200\200"};
    size_t split, i;
    for (split = 0; split < sizeof(good); ++split) {
        memset(&state, 0, sizeof(state));
        CHECK(knurl_websocket_message_check(&state, KNURL_WEBSOCKET_TEXT, 0,
                                            good, split, 32) == 0);
        CHECK(knurl_websocket_message_check(
                  &state, KNURL_WEBSOCKET_CONTINUATION, 1, good + split,
                  sizeof(good) - 1 - split, 32) == 0);
        CHECK(state.kind == 0 && state.bytes == 0 && state.need == 0);
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memset(&state, 0, sizeof(state));
        CHECK(knurl_websocket_message_check(
                  &state, KNURL_WEBSOCKET_TEXT, 1, bad[i],
                  strlen((const char *)bad[i]), 32) == 1007);
        memset(&state, 0, sizeof(state));
        CHECK(knurl_websocket_message_check(
                  &state, KNURL_WEBSOCKET_BINARY, 1, bad[i],
                  strlen((const char *)bad[i]), 32) == 0);
    }
    memset(&state, 0, sizeof(state));
    CHECK(knurl_websocket_message_check(&state, KNURL_WEBSOCKET_CONTINUATION, 1,
                                        good, 0, 32) == 1002);
    CHECK(knurl_websocket_message_check(&state, KNURL_WEBSOCKET_BINARY, 0, good,
                                        5, 6) == 0);
    CHECK(knurl_websocket_message_check(&state, KNURL_WEBSOCKET_TEXT, 1, good,
                                        0, 6) == 1002);
    CHECK(knurl_websocket_message_check(&state, KNURL_WEBSOCKET_CONTINUATION, 1,
                                        good, 2, 6) == 1009);
    state.bytes = (size_t)-1;
    CHECK(knurl_websocket_message_check(&state, KNURL_WEBSOCKET_CONTINUATION, 1,
                                        good, 1, (size_t)-1) == 1009);
    return 0;
}
static int closing(void)
{
    unsigned char bytes[4] = {3, 232, 'o', 'k'};
    static const unsigned short invalid[] = {0,    999,  1004, 1005, 1006,
                                             1015, 2000, 2999, 5000, 65535};
    static const unsigned short valid[] = {1000, 1001, 1002, 1003, 1007,
                                           1008, 1009, 1010, 1011, 1012,
                                           1013, 1014, 3000, 4999};
    size_t i;
    CHECK(knurl_websocket_close_check(NULL, 0) == 0);
    CHECK(knurl_websocket_close_check(bytes, 1) == 1002);
    CHECK(knurl_websocket_close_check(bytes, 4) == 0);
    bytes[2] = 0xc0;
    bytes[3] = 0xaf;
    CHECK(knurl_websocket_close_check(bytes, 4) == 1007);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        bytes[0] = (unsigned char)(invalid[i] >> 8);
        bytes[1] = (unsigned char)invalid[i];
        CHECK(knurl_websocket_close_check(bytes, 2) == 1002);
    }
    for (i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        bytes[0] = (unsigned char)(valid[i] >> 8);
        bytes[1] = (unsigned char)valid[i];
        CHECK(knurl_websocket_close_check(bytes, 2) == 0);
    }
    return 0;
}
int main(void)
{
    CHECK(message() == 0);
    CHECK(closing() == 0);
    return 0;
}
