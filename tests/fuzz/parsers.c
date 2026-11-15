#define _GNU_SOURCE 1
#include "http_framing.h"
#include "proxy_http.h"
#include "proxy_ws_wire.h"
#include "static_internal.h"
#include "websocket_internal.h"
#include "server_internal.h" /* IWYU pragma: keep (fixture's server layout) */
#include "loop_internal.h"
#include "receivers_internal.h"
#include "forwarded.h"
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
    size_t empty = 0;
    length.high = 1;
    length.low = 0;
    (void)knurl_route_path_validate(text, KNURL_PATH_LITERAL, NULL);
    (void)knurl_http_tag_matches(text, "\"tag\"", 1, 0, &empty);
    knurl_static_range_parse(text, length, &range);
    if (range.satisfiable && range.requested) {
        assert(range.parts[0].length.high || range.parts[0].length.low);
        assert(knurl_u64_add(range.parts[0].start, range.parts[0].length, &end,
                             NULL) == KNURL_OK);
        assert(knurl_u64_compare(end, length, &order, NULL) == KNURL_OK &&
               order <= 0);
    }
}

static void forwarded(const unsigned char *data, size_t length)
{
    static const size_t limits[] = {1, 0, 64};
    struct knurl_forward_chain *chain = malloc(sizeof(*chain));
    knurl_client_ip_config config;
    knurl_mmdb_address normalized;
    knurl_bytes input;
    char *extended = malloc(length + 32);
    size_t format, bound, i, count;
    if (chain == NULL || extended == NULL)
        abort();
    memset(&config, 0, sizeof(config));
    input.data = (const char *)data;
    input.length = length;
    for (format = 0; format < 2; ++format)
        for (bound = 0; bound < sizeof(limits) / sizeof(limits[0]); ++bound) {
            config.forwarded_format =
                format ? KNURL_FORWARDED_RFC7239 : KNURL_FORWARDED_COMMA;
            config.max_hops = limits[bound];
            if (!knurl_forwarded_parse(&config, input, chain))
                continue;
            count = chain->count;
            assert(count && count <= (limits[bound] ? limits[bound] : 16));
            for (i = 0; i < count; ++i) {
                assert(knurl_ip_normalize(&chain->hops[i], &normalized));
                assert(normalized.size == chain->hops[i].size);
                assert(!memcmp(normalized.bytes, chain->hops[i].bytes,
                               normalized.size));
            }
            if (count > 1) {
                config.max_hops = count - 1;
                assert(!knurl_forwarded_parse(&config, input, chain));
            }
            config.max_hops = 64;
            memcpy(extended, data, length);
            strcpy(extended + length, format ? ",for=unknown" : ",invalid");
            input.data = extended;
            input.length = strlen(extended);
            assert(!knurl_forwarded_parse(&config, input, chain));
            input.data = (const char *)data;
            input.length = length;
        }
    free(extended);
    free(chain);
}
/* Never allow mutated hostnames to initiate system DNS or file I/O. */
static knurl_status resolve(const char *name, knurl_mmdb_address *out,
                            size_t capacity, size_t *count, void *data,
                            knurl_error *error)
{
    (void)name;
    (void)data;
    assert(capacity == 2);
    assert(knurl_mmdb_address_parse("192.0.2.1", &out[0], error) == KNURL_OK);
    assert(knurl_mmdb_address_parse("::ffff:192.0.2.1", &out[1], error) ==
           KNURL_OK);
    *count = 2;
    return KNURL_OK;
}
static void acl_input(const unsigned char *data, size_t length)
{
    knurl_acl_config config;
    knurl_acl *acl;
    knurl_mmdb_address peer;
    struct knurl_ip_network network;
    knurl_status status;
    knurl_error error;
    char *text = malloc(length + 1), *line;
    const char *rules[1];
    int allowed = 1;
    if (text == NULL)
        abort();
    memcpy(text, data, length);
    text[length] = 0;
    line = strchr(text, '\n');
    if (line != NULL)
        *line = 0;
    rules[0] = text;
    knurl_acl_config_init(&config);
    config.policy = KNURL_ACL_DENY;
    config.allow.addresses = config.deny.addresses = rules;
    config.allow.address_count = config.deny.address_count = 1;
    config.resolve = resolve;
    config.dns_max_addresses = 2;
    status = knurl_acl_new(&config, &acl, &error);
    if (status == KNURL_OK) {
        assert(acl->refresh(acl, &error) == KNURL_OK);
        if (knurl_ip_network_parse(text, &network))
            peer = network.address;
        else
            assert(knurl_mmdb_address_parse("192.0.2.1", &peer, &error) ==
                   KNURL_OK);
        assert(acl->check(acl, &peer, NULL, &allowed, &error) == KNURL_OK &&
               !allowed);
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.bytes, data, length < 16 ? length : 16);
        peer.size = length ? data[0] % 20 : 0;
        allowed = 1;
        status = acl->check(acl, &peer, NULL, &allowed, &error);
        assert(status ==
               (peer.size == 4 || peer.size == 16 ? KNURL_OK : KNURL_INVALID));
        assert(
            !allowed); /* Identical allow/deny sets can never grant access. */
        assert(acl->close(acl, &error) == KNURL_OK);
    } else
        assert(status == KNURL_INVALID && acl == NULL);
    free(text);
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
    forwarded(data, length);
    acl_input(data, length);
    websocket(data, length);
    free(data);
    return 0;
}
