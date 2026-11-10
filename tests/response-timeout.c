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
    knurl_server server;
    knurl_request request;
    struct http_request native;
    knurl_response response;
    knurl_channel channel;
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
    request->response->outcome.status = code;
    request->response->phase = KNURL_RESPONSE_FINISHING;
    request->response->head = &fixture.segment;
    request->response->queued = 1;
    schedule(request->response);
}

void knurl_test_body_timeout(knurl_request *request)
{
    if (fixture.stimulus == 1)
        queue_error(request, 408);
}

void knurl_test_channel_timeout(knurl_channel *channel)
{
    if (fixture.stimulus == 2 && channel != NULL) {
        queue_error(&fixture.request, 400);
        channel->phase = KNURL_CHANNEL_CLOSED;
    }
}

void knurl_test_request_fail(knurl_request *request, knurl_status cause,
                             int code)
{
    ++fixture.failures;
    fixture.cause = cause;
    fixture.code = request->response->outcome.status = code;
}

static void setup(int output)
{
    memset(&fixture, 0, sizeof(fixture));
    fixture.now = 1000;
    fixture.runtime.server = &fixture.server;
    fixture.runtime.responses = &fixture.delivery;
    fixture.server.config.timeouts.response_write_idle.milliseconds = 50;
    fixture.server.config.timeouts.deferred_response.milliseconds = 50;
    fixture.request.server = &fixture.server;
    fixture.request.native = &fixture.native;
    fixture.request.response = &fixture.response;
    fixture.request.outcome.body_complete = 1;
    fixture.response.request = &fixture.request;
    fixture.response.phase = KNURL_RESPONSE_BUILDING;
    fixture.response.backend = &fixture.delivery;
    fixture.delivery.runtime = &fixture.runtime;
    fixture.delivery.response = &fixture.response;
    fixture.delivery.last_progress = fixture.now;
    /* Exercise deadline evaluation without draining output into a socket. */
    fixture.delivery.queued = 1;
    if (output) {
        fixture.response.head = &fixture.segment;
        fixture.response.queued = 1;
        fixture.delivery.output_pending = 1;
    }
}

int main(void)
{
    int output, stimulus;
    for (stimulus = 1; stimulus <= 2; ++stimulus) {
        setup(0);
        fixture.stimulus = stimulus;
        fixture.request.outcome.body_complete = 0;
        if (stimulus == 2)
            fixture.request.channel = &fixture.channel;
        knurl_kore_responses_turn(&fixture.runtime);
        CHECK(fixture.failures == 0);
        CHECK(fixture.delivery.last_progress == 1001);
        CHECK(fixture.response.outcome.status == (stimulus == 1 ? 408 : 400));
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
        fixture.server.config.timeouts.response_write_idle.disabled = 1;
        fixture.server.config.timeouts.deferred_response.disabled = 1;
        knurl_kore_responses_turn(&fixture.runtime);
        CHECK(fixture.failures == 0);
        CHECK(knurl_kore_responses_wait(&fixture.runtime, 100) == 100);
    }
    setup(0);
    fixture.now = 2000;
    fixture.request.outcome.body_complete = 0;
    knurl_kore_responses_turn(&fixture.runtime);
    CHECK(fixture.failures == 0);
    CHECK(knurl_kore_responses_wait(&fixture.runtime, 100) == 100);
    return 0;
}
