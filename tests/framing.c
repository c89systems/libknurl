#include <rotmos/rotmos.h>
#include "proxy-check.h"
#include <string.h>

struct context {
    rotmos_framer *framer;
    size_t room;
    size_t delivered;
    size_t trailers;
    int invalid;
};

static rotmos_status body(rotmos_bytes input, size_t *consumed, void *data,
                          rotmos_error *error)
{
    struct context *c;
    size_t ignored;
    c = data;
    PROXY_CHECK(rotmos_framer_destroy(c->framer, error) == ROTMOS_STATE);
    PROXY_CHECK(rotmos_framer_feed(c->framer, NULL, 0, &ignored, error) ==
                ROTMOS_STATE);
    *consumed = input.length < c->room ? input.length : c->room;
    if (c->invalid) {
        *consumed = input.length + 1;
        return ROTMOS_OK;
    }
    c->room -= *consumed;
    c->delivered += *consumed;
    return *consumed ? ROTMOS_OK : ROTMOS_AGAIN;
}

static rotmos_status trailer(rotmos_header field, void *data,
                             rotmos_error *error)
{
    struct context *c;
    (void)error;
    c = data;
    PROXY_CHECK(field.name.length == 5 &&
                memcmp(field.name.data, "X-End", 5) == 0);
    PROXY_CHECK(field.value.length == 3 &&
                memcmp(field.value.data, "yes", 3) == 0);
    ++c->trailers;
    return ROTMOS_OK;
}

int main(void)
{
    const char wire[] =
        "3;kind=\"x\"\r\nabc\r\n2\r\nde\r\n0\r\nX-End: yes\r\n\r\nTAIL";
    rotmos_framer_config config;
    struct context c;
    rotmos_error error;
    size_t consumed;
    size_t offset;
    rotmos_status status;
    memset(&c, 0, sizeof(c));
    rotmos_framer_config_init(&config);
    config.on_body = body;
    config.on_trailer = trailer;
    config.userdata = &c;
    config.chunked = 1;
    PROXY_CHECK(rotmos_framer_new(&config, &c.framer, &error) == ROTMOS_OK);
    c.room = 2;
    status =
        rotmos_framer_feed(c.framer, wire, sizeof(wire) - 1, &offset, &error);
    PROXY_CHECK(status == ROTMOS_AGAIN && c.delivered == 2 &&
                offset < sizeof(wire) - 5);
    c.room = 10;
    PROXY_CHECK(rotmos_framer_feed(c.framer, wire + offset,
                                   sizeof(wire) - 1 - offset, &consumed,
                                   &error) == ROTMOS_EOF);
    offset += consumed;
    PROXY_CHECK(strcmp(wire + offset, "TAIL") == 0 && c.delivered == 5 &&
                c.trailers == 1);
    PROXY_CHECK(rotmos_framer_body_bytes(c.framer).low == 5);
    PROXY_CHECK(rotmos_framer_feed(c.framer, "TAIL", 4, &consumed, &error) ==
                    ROTMOS_EOF &&
                consumed == 0);
    PROXY_CHECK(rotmos_framer_finish(c.framer, &error) == ROTMOS_EOF);
    PROXY_CHECK(rotmos_framer_destroy(c.framer, &error) == ROTMOS_OK);
    config.chunked = 0;
    config.content_length.low = 3;
    config.max_body_bytes.low = 2;
    PROXY_CHECK(rotmos_framer_new(&config, &c.framer, &error) == ROTMOS_LIMIT &&
                c.framer == NULL);
    config.max_body_bytes.low = 0;
    PROXY_CHECK(rotmos_framer_new(&config, &c.framer, &error) == ROTMOS_OK);
    c.room = 0;
    PROXY_CHECK(rotmos_framer_feed(c.framer, "abcTAIL", 7, &consumed, &error) ==
                    ROTMOS_AGAIN &&
                consumed == 0);
    c.room = 2;
    PROXY_CHECK(rotmos_framer_feed(c.framer, "abcTAIL", 7, &consumed, &error) ==
                    ROTMOS_AGAIN &&
                consumed == 2);
    PROXY_CHECK(rotmos_framer_finish(c.framer, &error) == ROTMOS_INVALID);
    PROXY_CHECK(rotmos_framer_feed(c.framer, "c", 1, &consumed, &error) ==
                    ROTMOS_INVALID &&
                consumed == 0);
    PROXY_CHECK(rotmos_framer_destroy(c.framer, NULL) == ROTMOS_OK);
    config.chunked = 1;
    config.content_length.low = 0;
    config.max_body_bytes.low = 2;
    PROXY_CHECK(rotmos_framer_new(&config, &c.framer, &error) == ROTMOS_OK);
    c.room = 10;
    PROXY_CHECK(rotmos_framer_feed(c.framer, "3\r\nabc\r\n0\r\n\r\n", 13,
                                   &consumed, &error) == ROTMOS_LIMIT);
    PROXY_CHECK(rotmos_framer_finish(c.framer, &error) == ROTMOS_LIMIT);
    PROXY_CHECK(rotmos_framer_destroy(c.framer, NULL) == ROTMOS_OK);
    config.chunked = 0;
    config.max_body_bytes.low = 0;
    config.content_length.high = 1;
    config.content_length.low = 7;
    PROXY_CHECK(rotmos_framer_new(&config, &c.framer, &error) == ROTMOS_OK);
    c.invalid = 1;
    PROXY_CHECK(rotmos_framer_feed(c.framer, "x", 1, &consumed, &error) ==
                    ROTMOS_INVALID &&
                consumed == 0);
    PROXY_CHECK(rotmos_framer_destroy(c.framer, NULL) == ROTMOS_OK);
    config.content_length.high = config.content_length.low = 0xffffffffUL;
    c.invalid = 0;
    c.room = 10;
    PROXY_CHECK(rotmos_framer_new(&config, &c.framer, &error) == ROTMOS_OK);
    PROXY_CHECK(rotmos_framer_feed(c.framer, "abc", 3, &consumed, &error) ==
                    ROTMOS_OK &&
                consumed == 3);
    PROXY_CHECK(rotmos_framer_body_bytes(c.framer).low == 3);
    PROXY_CHECK(rotmos_framer_finish(c.framer, &error) == ROTMOS_INVALID);
    PROXY_CHECK(rotmos_framer_destroy(c.framer, NULL) == ROTMOS_OK);
    return 0;
}
