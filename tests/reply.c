#include <knurl/knurl.h>

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
    knurl_status returned;
    knurl_source *closing_target;
    knurl_status target_result;
} source_state;

static knurl_status source_read(void *buffer, size_t capacity, size_t *produced,
                                void *userdata, knurl_error *error)
{
    source_state *state;
    (void)buffer;
    (void)capacity;
    state = userdata;
    ++state->reads;
    *produced = 0;
    knurl_error_clear(error);
    return state->returned;
}

static void source_close(void *userdata, knurl_status outcome)
{
    source_state *state;
    (void)outcome;
    state = userdata;
    ++state->closes;
    if (state->closing_target != NULL)
        state->target_result =
            knurl_source_destroy(state->closing_target, NULL);
}

static int view_is(knurl_bytes view, const char *text)
{
    return view.length == strlen(text) &&
           (view.length == 0 || memcmp(view.data, text, view.length) == 0);
}

int main(void)
{
    knurl_reply *reply;
    knurl_source *source;
    knurl_source *next;
    knurl_source_config config;
    knurl_source_materialize_config materialize;
    knurl_error error;
    knurl_header header;
    knurl_bytes bytes;
    source_state first;
    source_state second;
    char input[] = "abc";
    char output[4];
    size_t produced;

    first.reads = first.closes = 0;
    second.reads = second.closes = 0;
    first.closing_target = second.closing_target = NULL;
    first.target_result = second.target_result = KNURL_OK;
    first.returned = second.returned = KNURL_EOF;
    REQUIRE(knurl_reply_new(&reply, &error) == KNURL_OK);
    REQUIRE(!knurl_reply_is_selected(reply));
    REQUIRE(knurl_reply_header_add(reply, "Set-Cookie", "a=1", &error) ==
            KNURL_OK);
    REQUIRE(knurl_reply_header_add(reply, "Set-Cookie", "b=2", &error) ==
            KNURL_OK);
    REQUIRE(knurl_reply_header_add(reply, "Content-Length", "100", &error) ==
            KNURL_OK);
    REQUIRE(knurl_reply_header_add(reply, "Transfer-Encoding", "chunked",
                                   &error) == KNURL_OK);
    REQUIRE(knurl_reply_header_count(reply) == 4);
    REQUIRE(knurl_reply_header_set(reply, "set-cookie", "c=3", &error) ==
            KNURL_OK);
    REQUIRE(knurl_reply_header_count(reply) == 3);
    REQUIRE(knurl_reply_header_at(reply, 0, &header, &error) == KNURL_OK);
    REQUIRE(view_is(header.value, "c=3"));
    REQUIRE(knurl_reply_header_add(reply, "bad name", "x", &error) ==
            KNURL_INVALID);
    REQUIRE(knurl_reply_header_add(reply, "X-Test", "x\r\ny", &error) ==
            KNURL_INVALID);

    bytes.data = input;
    bytes.length = 3;
    REQUIRE(knurl_source_bytes(bytes, &source, &error) == KNURL_OK);
    input[0] = 'z';
    knurl_source_materialize_config_init(&materialize);
    materialize.chunk_bytes = 2;
    materialize.reset_before = materialize.reset_after = 1;
    REQUIRE(knurl_reply_source_materialized(reply, 200, "text/plain", source,
                                            &materialize, &error) == KNURL_OK);
    REQUIRE(view_is(knurl_reply_body(reply), "abc"));
    materialize.max_bytes = 2;
    REQUIRE(knurl_reply_source_materialized(reply, 201, "text/plain", source,
                                            &materialize,
                                            &error) == KNURL_LIMIT);
    REQUIRE(knurl_reply_status_code(reply) == 200);
    REQUIRE(view_is(knurl_reply_body(reply), "abc"));
    REQUIRE(knurl_source_read(source, output, sizeof(output), &produced,
                              &error) == KNURL_OK);
    REQUIRE(produced == 3 && memcmp(output, "abc", 3) == 0);
    REQUIRE(knurl_source_destroy(source, &error) == KNURL_OK);

    knurl_source_config_init(&config);
    config.read = source_read;
    config.close = source_close;
    config.userdata = &first;
    REQUIRE(knurl_source_new(&config, &source, &error) == KNURL_OK);
    config.userdata = &second;
    REQUIRE(knurl_source_new(&config, &next, &error) == KNURL_OK);
    REQUIRE(first.reads == 0 && second.reads == 0);
    REQUIRE(knurl_reply_source_take(reply, 200, "text/plain", source, &error) ==
            KNURL_OK);
    REQUIRE(knurl_source_destroy(source, &error) == KNURL_STATE);
    first.closing_target = next;
    REQUIRE(knurl_reply_source_take(reply, 200, "text/plain", next, &error) ==
            KNURL_OK);
    REQUIRE(first.closes == 1 && first.target_result == KNURL_STATE);
    REQUIRE(knurl_reply_status(reply, 204, &error) == KNURL_OK);
    REQUIRE(second.closes == 1 && first.reads == 0 && second.reads == 0);
    REQUIRE(knurl_reply_payload_kind(reply) == KNURL_REPLY_EMPTY);
    REQUIRE(knurl_reply_content_type(reply).length == 0);
    REQUIRE(knurl_reply_header_count(reply) == 3);
    REQUIRE(knurl_reply_file(reply, 200, "text/plain", "/no/such/knurl/file",
                             &error) == KNURL_OK);
    REQUIRE(knurl_reply_status_code(reply) == 200);
    REQUIRE(knurl_reply_payload_kind(reply) == KNURL_REPLY_FILE);
    REQUIRE(knurl_reply_destroy(reply, &error) == KNURL_OK);

    first.closing_target = NULL;
    first.returned = KNURL_OK;
    config.userdata = &first;
    REQUIRE(knurl_source_new(&config, &source, &error) == KNURL_OK);
    REQUIRE(knurl_source_read(source, output, sizeof(output), &produced,
                              &error) == KNURL_INVALID);
    REQUIRE(produced == 0);
    REQUIRE(knurl_source_destroy(source, &error) == KNURL_OK);
    REQUIRE(first.closes == 2);
    return 0;
}
