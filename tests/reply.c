#include <rotmos/rotmos.h>

#include <stdio.h>
#include <string.h>

#define REQUIRE(expression)                                                    \
    do {                                                                       \
        if (!(expression)) {                                                   \
            (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__,           \
                          #expression);                                        \
            return 1;                                                          \
        }                                                                      \
    } while (0)

typedef struct source_state {
    int reads;
    int closes;
    rotmos_status returned;
    rotmos_source *closing_target;
    rotmos_status target_result;
} source_state;

static rotmos_status source_read(void *buffer, size_t capacity,
                                 size_t *produced, void *userdata,
                                 rotmos_error *error)
{
    source_state *state;
    (void)buffer;
    (void)capacity;
    state = userdata;
    ++state->reads;
    *produced = 0;
    rotmos_error_clear(error);
    return state->returned;
}

static void source_close(void *userdata, rotmos_status outcome)
{
    source_state *state;
    (void)outcome;
    state = userdata;
    ++state->closes;
    if (state->closing_target != NULL)
        state->target_result =
            rotmos_source_destroy(state->closing_target, NULL);
}

static int view_is(rotmos_bytes view, const char *text)
{
    return view.length == strlen(text) &&
           (view.length == 0 || memcmp(view.data, text, view.length) == 0);
}

int main(void)
{
    rotmos_reply *reply;
    rotmos_source *source;
    rotmos_source *next;
    rotmos_source_config config;
    rotmos_source_materialize_config materialize;
    rotmos_error error;
    rotmos_header header;
    rotmos_bytes bytes;
    source_state first;
    source_state second;
    char input[] = "abc";
    char output[4];
    size_t produced;

    first.reads = first.closes = 0;
    second.reads = second.closes = 0;
    first.closing_target = second.closing_target = NULL;
    first.target_result = second.target_result = ROTMOS_OK;
    first.returned = second.returned = ROTMOS_EOF;
    REQUIRE(rotmos_reply_new(&reply, &error) == ROTMOS_OK);
    REQUIRE(!rotmos_reply_is_selected(reply));
    REQUIRE(rotmos_reply_header_add(reply, "Set-Cookie", "a=1", &error) ==
            ROTMOS_OK);
    REQUIRE(rotmos_reply_header_add(reply, "Set-Cookie", "b=2", &error) ==
            ROTMOS_OK);
    REQUIRE(rotmos_reply_header_add(reply, "Content-Length", "100", &error) ==
            ROTMOS_OK);
    REQUIRE(rotmos_reply_header_add(reply, "Transfer-Encoding", "chunked",
                                    &error) == ROTMOS_OK);
    REQUIRE(rotmos_reply_header_count(reply) == 4);
    REQUIRE(rotmos_reply_header_set(reply, "set-cookie", "c=3", &error) ==
            ROTMOS_OK);
    REQUIRE(rotmos_reply_header_count(reply) == 3);
    REQUIRE(rotmos_reply_header_at(reply, 0, &header, &error) == ROTMOS_OK);
    REQUIRE(view_is(header.value, "c=3"));
    REQUIRE(rotmos_reply_header_add(reply, "bad name", "x", &error) ==
            ROTMOS_INVALID);
    REQUIRE(rotmos_reply_header_add(reply, "X-Test", "x\r\ny", &error) ==
            ROTMOS_INVALID);

    bytes.data = input;
    bytes.length = 3;
    REQUIRE(rotmos_source_bytes(bytes, &source, &error) == ROTMOS_OK);
    input[0] = 'z';
    rotmos_source_materialize_config_init(&materialize);
    materialize.chunk_bytes = 2;
    materialize.reset_before = materialize.reset_after = 1;
    REQUIRE(rotmos_reply_source_materialized(reply, 200, "text/plain", source,
                                             &materialize,
                                             &error) == ROTMOS_OK);
    REQUIRE(view_is(rotmos_reply_body(reply), "abc"));
    materialize.max_bytes = 2;
    REQUIRE(rotmos_reply_source_materialized(reply, 201, "text/plain", source,
                                             &materialize,
                                             &error) == ROTMOS_LIMIT);
    REQUIRE(rotmos_reply_status_code(reply) == 200);
    REQUIRE(view_is(rotmos_reply_body(reply), "abc"));
    REQUIRE(rotmos_source_read(source, output, sizeof(output), &produced,
                               &error) == ROTMOS_OK);
    REQUIRE(produced == 3 && memcmp(output, "abc", 3) == 0);
    REQUIRE(rotmos_source_destroy(source, &error) == ROTMOS_OK);

    rotmos_source_config_init(&config);
    config.read = source_read;
    config.close = source_close;
    config.userdata = &first;
    REQUIRE(rotmos_source_new(&config, &source, &error) == ROTMOS_OK);
    config.userdata = &second;
    REQUIRE(rotmos_source_new(&config, &next, &error) == ROTMOS_OK);
    REQUIRE(first.reads == 0 && second.reads == 0);
    REQUIRE(rotmos_reply_source_take(reply, 200, "text/plain", source,
                                     &error) == ROTMOS_OK);
    REQUIRE(rotmos_source_destroy(source, &error) == ROTMOS_STATE);
    first.closing_target = next;
    REQUIRE(rotmos_reply_source_take(reply, 200, "text/plain", next, &error) ==
            ROTMOS_OK);
    REQUIRE(first.closes == 1 && first.target_result == ROTMOS_STATE);
    REQUIRE(rotmos_reply_status(reply, 204, &error) == ROTMOS_OK);
    REQUIRE(second.closes == 1 && first.reads == 0 && second.reads == 0);
    REQUIRE(rotmos_reply_payload_kind(reply) == ROTMOS_REPLY_EMPTY);
    REQUIRE(rotmos_reply_content_type(reply).length == 0);
    REQUIRE(rotmos_reply_header_count(reply) == 3);
    REQUIRE(rotmos_reply_file(reply, 200, "text/plain", "/no/such/rotmos/file",
                              &error) == ROTMOS_OK);
    REQUIRE(rotmos_reply_status_code(reply) == 200);
    REQUIRE(rotmos_reply_payload_kind(reply) == ROTMOS_REPLY_FILE);
    REQUIRE(rotmos_reply_destroy(reply, &error) == ROTMOS_OK);

    first.closing_target = NULL;
    first.returned = ROTMOS_OK;
    config.userdata = &first;
    REQUIRE(rotmos_source_new(&config, &source, &error) == ROTMOS_OK);
    REQUIRE(rotmos_source_read(source, output, sizeof(output), &produced,
                               &error) == ROTMOS_INVALID);
    REQUIRE(produced == 0);
    REQUIRE(rotmos_source_destroy(source, &error) == ROTMOS_OK);
    REQUIRE(first.closes == 2);
    return 0;
}
