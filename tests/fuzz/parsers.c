#define _GNU_SOURCE 1
#include "http_framing.h"
#include "proxy_http.h"
#include "proxy_ws_wire.h"
#include "static_internal.h"
#include "websocket_internal.h"
#include "server_internal.h" /* IWYU pragma: keep (fixture's server layout) */
#include "loop_internal.h"
#include "receivers_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
#define FUZZ_INPUT_BYTES 16384u

static size_t accept_body(void *data, const unsigned char *bytes, size_t count)
{
    (void)data;
    (void)bytes;
    return count;
}
static int accept_trailer(void *data, const char *name, const char *value)
{
    (void)data;
    (void)name;
    (void)value;
    return 1;
}
static void framing(const unsigned char *data, size_t length)
{
    knurl_http_body_framer *framer = malloc(sizeof(*framer));
    size_t at = 0, count, chunk;
    knurl_http_frame_result status;
    if (framer == NULL)
        abort();
    knurl_http_body_framer_chunked(framer);
    while (at < length) {
        chunk = (data[at] & 31) + 1;
        if (chunk > length - at)
            chunk = length - at;
        status = knurl_http_body_framer_feed(framer, data + at, chunk, &count,
                                             accept_body, accept_trailer, NULL);
        assert(count <= chunk);
        at += count;
        if (status == KNURL_HTTP_FRAME_COMPLETE ||
            status == KNURL_HTTP_FRAME_INVALID)
            break;
        assert(count > 0);
    }
    free(framer);
}
static void heads(const unsigned char *data, size_t length)
{
    knurl_proxy_http_response response;
    knurl_proxy_headers headers;
    knurl_proxy_http_event event;
    const char *reason;
    size_t at = 0, end, consumed;
    unsigned status;
    knurl_proxy_http_response_init(&response, length && (data[0] & 1));
    while (at < length) {
        end = at;
        while (end < length && data[end++] != '\n')
            ;
        if (knurl_proxy_http_response_header(&response, (const char *)data + at,
                                             end - at, &event,
                                             &reason) != KNURL_PROXY_HEADER_OK)
            break;
        at = end;
    }
    (void)knurl_proxy_http_response_finish(&response, &reason);
    knurl_proxy_http_response_cleanup(&response);
    knurl_proxy_headers_init(&headers);
    (void)knurl_proxy_ws_wire_response_head(data, length, &consumed, &status,
                                            &headers, &reason);
    knurl_proxy_headers_cleanup(&headers);
}
static void paths(char *text)
{
    knurl_u64 length;
    knurl_u64 end;
    int order;
    struct knurl_static_ranges range;
    length.high = 1;
    length.low = 0;
    (void)knurl_route_path_validate(text, KNURL_PATH_LITERAL, NULL);
    (void)knurl_http_tag_matches(text, "\"tag\"", 1, 0);
    knurl_static_range_parse(text, length, &range);
    if (range.satisfiable && range.requested) {
        assert(range.parts[0].length.high || range.parts[0].length.low);
        assert(knurl_u64_add(range.parts[0].start, range.parts[0].length, &end,
                             NULL) == KNURL_OK);
        assert(knurl_u64_compare(end, length, &order, NULL) == KNURL_OK &&
               order <= 0);
    }
}

/* Only the transport is a fixture. Parsing/validation/control-frame processing
 * uses the real owned websocket_frame.c against a bounded Core channel. */
static knurl_status empty_read(knurl_channel *channel, void *data, size_t size,
                               size_t *count, knurl_error *error)
{
    (void)channel;
    (void)data;
    (void)size;
    (void)error;
    *count = 0;
    return KNURL_AGAIN;
}
static void noop(knurl_channel *channel) { (void)channel; }
static void wake(void *data) { (void)data; }
static void websocket(const unsigned char *data, size_t length)
{
    knurl_websocket *ws = calloc(1, sizeof(struct knurl_websocket_impl));
    knurl_channel *channel = calloc(1, sizeof(struct knurl_channel_impl));
    struct knurl_channel_chunk *chunk;
    knurl_server *server = calloc(1, sizeof(struct knurl_server_impl));
    knurl_loop *loop = calloc(1, sizeof(struct knurl_loop_impl));
    if (ws == NULL || channel == NULL || server == NULL || loop == NULL)
        abort();
    knurl_websocket_receiver_init(ws);
    knurl_channel_receiver_init(channel);
    knurl_server_receiver_init(server);
    knurl_loop_receiver_init(loop);
    server->impl->loop = loop;
    server->impl->config.io.chunk_bytes = 32;
    loop->impl->owner = pthread_self();
    loop->impl->process_id = (long)getpid();
    loop->impl->backend.wake = wake;
    ws->impl->server = channel->impl->server = server;
    ws->impl->channel = channel;
    ws->impl->phase = KNURL_WEBSOCKET_OPEN;
    knurl_websocket_config_init(&ws->impl->config);
    ws->impl->config.max_frame_bytes = 4096;
    ws->impl->config.max_message_bytes = 4096;
    ws->impl->payload = malloc(4096);
    channel->impl->receive = malloc(FUZZ_INPUT_BYTES);
    if (ws->impl->payload == NULL || channel->impl->receive == NULL)
        abort();
    memcpy(channel->impl->receive, data, length);
    channel->impl->receive_length = length;
    channel->impl->phase = KNURL_CHANNEL_TUNNEL;
    channel->impl->config.send_bytes = 4096;
    channel->impl->config.receive_bytes = FUZZ_INPUT_BYTES;
    channel->impl->ops.read = empty_read;
    channel->impl->ops.interest = noop;
    channel->impl->schedule = noop;
    knurl_websocket_reset_frame(ws);
    knurl_websocket_step(ws);
    while ((chunk = channel->impl->head) != NULL) {
        channel->impl->head = chunk->next;
        free(chunk->data);
        free(chunk);
    }
    free(channel->impl->receive);
    free(channel);
    free(ws->impl->payload);
    free(ws);
    free(loop);
    free(server);
}
int main(int argc, char **argv)
{
    FILE *input;
    unsigned char *data;
    size_t length;
    /* Keep crash detection in AFL's wait status/input artifacts. Do not invoke
     * workstation core-dump handlers or change host sysctl configuration. */
    if (prctl(PR_SET_DUMPABLE, 0) != 0)
        return 2;
    if (argc != 2 || (input = fopen(argv[1], "rb")) == NULL)
        return 2;
    data = malloc(FUZZ_INPUT_BYTES + 1);
    if (data == NULL)
        return 2;
    length = fread(data, 1, FUZZ_INPUT_BYTES, input);
    fclose(input);
    data[length] = 0;
    framing(data, length);
    heads(data, length);
    paths((char *)data);
    websocket(data, length);
    free(data);
    return 0;
}
