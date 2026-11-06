#define _GNU_SOURCE 1
#include "proxy_framing.h"
#include "proxy_http.h"
#include "proxy_ws_wire.h"
#include "static_internal.h"
#include "websocket_internal.h"
#include "server_internal.h" /* IWYU pragma: keep (fixture's server layout) */
#include "loop_internal.h"
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
    rotmos_proxy_body_framer *framer = malloc(sizeof(*framer));
    size_t at = 0, count, chunk;
    rotmos_proxy_frame_result status;
    if (framer == NULL)
        abort();
    rotmos_proxy_body_framer_chunked(framer);
    while (at < length) {
        chunk = (data[at] & 31) + 1;
        if (chunk > length - at)
            chunk = length - at;
        status =
            rotmos_proxy_body_framer_feed(framer, data + at, chunk, &count,
                                          accept_body, accept_trailer, NULL);
        assert(count <= chunk);
        at += count;
        if (status == ROTMOS_PROXY_FRAME_COMPLETE ||
            status == ROTMOS_PROXY_FRAME_INVALID)
            break;
        assert(count > 0);
    }
    free(framer);
}
static void heads(const unsigned char *data, size_t length)
{
    rotmos_proxy_http_response response;
    rotmos_proxy_headers headers;
    rotmos_proxy_http_event event;
    const char *reason;
    size_t at = 0, end, consumed;
    unsigned status;
    rotmos_proxy_http_response_init(&response, length && (data[0] & 1));
    while (at < length) {
        end = at;
        while (end < length && data[end++] != '\n')
            ;
        if (rotmos_proxy_http_response_header(
                &response, (const char *)data + at, end - at, &event,
                &reason) != ROTMOS_PROXY_HEADER_OK)
            break;
        at = end;
    }
    (void)rotmos_proxy_http_response_finish(&response, &reason);
    rotmos_proxy_http_response_cleanup(&response);
    rotmos_proxy_headers_init(&headers);
    (void)rotmos_proxy_ws_wire_response_head(data, length, &consumed, &status,
                                             &headers, &reason);
    rotmos_proxy_headers_cleanup(&headers);
}
static void paths(char *text)
{
    rotmos_u64 length;
    rotmos_u64 end;
    int order;
    struct rotmos_static_range range;
    length.high = 1;
    length.low = 0;
    (void)rotmos_route_path_validate(text, ROTMOS_PATH_LITERAL, NULL);
    (void)rotmos_http_tag_matches(text, "\"tag\"", 1, 0);
    rotmos_static_range_parse(text, length, &range);
    if (range.satisfiable && range.requested) {
        assert(range.length.high || range.length.low);
        assert(rotmos_u64_add(range.start, range.length, &end, NULL) ==
               ROTMOS_OK);
        assert(rotmos_u64_compare(end, length, &order, NULL) == ROTMOS_OK &&
               order <= 0);
    }
}

/* Only the transport is a fixture. Parsing/validation/control-frame processing
 * uses the real owned websocket_frame.c against a bounded Core channel. */
static rotmos_status empty_read(rotmos_channel *channel, void *data,
                                size_t size, size_t *count, rotmos_error *error)
{
    (void)channel;
    (void)data;
    (void)size;
    (void)error;
    *count = 0;
    return ROTMOS_AGAIN;
}
static void noop(rotmos_channel *channel) { (void)channel; }
static void wake(void *data) { (void)data; }
static void websocket(const unsigned char *data, size_t length)
{
    rotmos_websocket *ws = calloc(1, sizeof(*ws));
    rotmos_channel *channel = calloc(1, sizeof(*channel));
    struct rotmos_channel_chunk *chunk;
    rotmos_server *server = calloc(1, sizeof(*server));
    rotmos_loop *loop = calloc(1, sizeof(*loop));
    if (ws == NULL || channel == NULL || server == NULL || loop == NULL)
        abort();
    server->loop = loop;
    server->config.io.chunk_bytes = 32;
    loop->owner = pthread_self();
    loop->process_id = (long)getpid();
    loop->backend.wake = wake;
    ws->server = channel->server = server;
    ws->channel = channel;
    ws->phase = ROTMOS_WEBSOCKET_OPEN;
    rotmos_websocket_config_init(&ws->config);
    ws->config.max_frame_bytes = 4096;
    ws->payload = malloc(4096);
    channel->receive = malloc(FUZZ_INPUT_BYTES);
    if (ws->payload == NULL || channel->receive == NULL)
        abort();
    memcpy(channel->receive, data, length);
    channel->receive_length = length;
    channel->phase = ROTMOS_CHANNEL_TUNNEL;
    channel->config.send_bytes = 4096;
    channel->config.receive_bytes = FUZZ_INPUT_BYTES;
    channel->ops.read = empty_read;
    channel->ops.interest = noop;
    channel->schedule = noop;
    rotmos_websocket_reset_frame(ws);
    rotmos_websocket_step(ws);
    while ((chunk = channel->head) != NULL) {
        channel->head = chunk->next;
        free(chunk->data);
        free(chunk);
    }
    free(channel->receive);
    free(channel);
    free(ws->payload);
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
