/* Compile the production response adapter with controlled time and timeout
 * callbacks. No test hooks enter the shipped library. */
#define kore_time_ms knurl_test_time_ms
#define knurl_kore_body_timeout knurl_test_body_timeout
#define knurl_channel_check_timeout knurl_test_channel_timeout
#define knurl_kore_request_fail knurl_test_request_fail
#include "../src/kore_response.c"
#undef kore_time_ms
#undef knurl_kore_body_timeout
#undef knurl_channel_check_timeout
#undef knurl_kore_request_fail

#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            fprintf(stderr, "response timeout %d: %s\n", __LINE__, #test);     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static struct {
    struct knurl_runtime runtime;
    struct knurl_server_impl server;
    struct knurl_request_impl request;
    struct http_request native;
    struct knurl_response_impl response;
    struct knurl_channel_impl channel;
    struct knurl_delivery delivery;
    struct knurl_response_segment segment;
    u_int64_t now;
    int stimulus, failures, code;
    knurl_status cause;
} fixture;

u_int64_t knurl_test_time_ms(void) { return fixture.now; }

static void queue_error(knurl_request *request, int code)
{
    ++fixture.now;
    request->impl->response->impl->outcome.status = code;
    request->impl->response->impl->phase = KNURL_RESPONSE_FINISHING;
    request->impl->response->impl->head = &fixture.segment;
    request->impl->response->impl->queued = 1;
    schedule(request->impl->response);
}

void knurl_test_body_timeout(knurl_request *request)
{
    if (fixture.stimulus == 1)
        queue_error(request, 408);
}

void knurl_test_channel_timeout(knurl_channel *channel)
{
    if (fixture.stimulus == 2 && channel != NULL) {
        queue_error(&fixture.request.receiver, 400);
        channel->impl->phase = KNURL_CHANNEL_CLOSED;
    }
}

void knurl_test_request_fail(knurl_request *request, knurl_status cause,
                             int code)
{
    ++fixture.failures;
    fixture.cause = cause;
    fixture.code = request->impl->response->impl->outcome.status = code;
}

static void setup(int output)
{
    memset(&fixture, 0, sizeof(fixture));
    knurl_server_receiver_init(&fixture.server.receiver);
    knurl_request_receiver_init(&fixture.request.receiver);
    knurl_response_receiver_init(&fixture.response.receiver);
    knurl_channel_receiver_init(&fixture.channel.receiver);
    fixture.now = 1000;
    fixture.runtime.server = &fixture.server.receiver;
    fixture.runtime.responses = &fixture.delivery;
    fixture.server.receiver.impl->config.timeouts.response_write_idle
        .milliseconds = 50;
    fixture.server.receiver.impl->config.timeouts.deferred_response
        .milliseconds = 50;
    fixture.request.receiver.impl->server = &fixture.server.receiver;
    fixture.request.receiver.impl->native = &fixture.native;
    fixture.request.receiver.impl->response = &fixture.response.receiver;
    fixture.request.receiver.impl->outcome.body_complete = 1;
    fixture.response.receiver.impl->request = &fixture.request.receiver;
    fixture.response.receiver.impl->phase = KNURL_RESPONSE_BUILDING;
    fixture.response.receiver.impl->backend = &fixture.delivery;
    fixture.delivery.runtime = &fixture.runtime;
    fixture.delivery.response = &fixture.response.receiver;
    fixture.delivery.last_progress = fixture.now;
    /* Exercise deadline evaluation without draining output into a socket. */
    fixture.delivery.queued = 1;
    if (output) {
        fixture.response.receiver.impl->head = &fixture.segment;
        fixture.response.receiver.impl->queued = 1;
        fixture.delivery.output_pending = 1;
    }
}

int main(void)
{
    int output, stimulus;
    for (stimulus = 1; stimulus <= 2; ++stimulus) {
        setup(0);
        fixture.stimulus = stimulus;
        fixture.request.receiver.impl->outcome.body_complete = 0;
        if (stimulus == 2)
            fixture.request.receiver.impl->channel = &fixture.channel.receiver;
        knurl_kore_responses_turn(&fixture.runtime);
        CHECK(fixture.failures == 0);
        CHECK(fixture.delivery.last_progress == 1001);
        CHECK(fixture.response.receiver.impl->outcome.status ==
              (stimulus == 1 ? 408 : 400));
        CHECK(knurl_kore_responses_wait(&fixture.runtime, 100) == 50);
    }
    /* Existing write-idle and deferred deadlines still expire exactly at the
     * boundary; disabled timers and unfinished input remain exempt. */
    for (output = 0; output <= 1; ++output) {
        setup(output);
        fixture.now = 1049;
        knurl_kore_responses_turn(&fixture.runtime);
        CHECK(fixture.failures == 0);
        CHECK(knurl_kore_responses_wait(&fixture.runtime, 100) == 1);
        fixture.now = 1050;
        knurl_kore_responses_turn(&fixture.runtime);
        CHECK(fixture.failures == 1 && fixture.cause == KNURL_TIMEOUT &&
              fixture.code == 500);
        setup(output);
        fixture.now = 2000;
        fixture.server.receiver.impl->config.timeouts.response_write_idle
            .disabled = 1;
        fixture.server.receiver.impl->config.timeouts.deferred_response
            .disabled = 1;
        knurl_kore_responses_turn(&fixture.runtime);
        CHECK(fixture.failures == 0);
        CHECK(knurl_kore_responses_wait(&fixture.runtime, 100) == 100);
    }
    setup(0);
    fixture.now = 2000;
    fixture.request.receiver.impl->outcome.body_complete = 0;
    knurl_kore_responses_turn(&fixture.runtime);
    CHECK(fixture.failures == 0);
    CHECK(knurl_kore_responses_wait(&fixture.runtime, 100) == 100);
    return 0;
}
