#define _POSIX_C_SOURCE 200809L
#include "../src/autoblock_internal.h"
#include "../src/policy_internal.h"
#include "../src/receivers_internal.h"
#include <sys/wait.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "autoblock %d: %s\n", __LINE__, #t);               \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static knurl_bytes ip(const char *text)
{
    knurl_bytes bytes;
    bytes.data = text;
    bytes.length = strlen(text);
    return bytes;
}
static knurl_status resolve(knurl_request *request, char *out, size_t capacity,
                            size_t *length, void *data, knurl_error *error)
{
    int *calls = data;
    (void)request;
    (void)capacity;
    ++*calls;
    strcpy(out, "203.0.113.42");
    *length = strlen(out);
    knurl_error_clear(error);
    return KNURL_OK;
}
static int paths(void)
{
    const char *proxies[] = {"127.0.0.1", "10.0.0.1"};
    const char *expected[] = {"198.51.100.1", "192.0.2.254",  "127.0.0.1",
                              "203.0.113.42", "198.51.100.1", "2001:db8::1",
                              "127.0.0.1"};
    const knurl_client_address_source sources[] = {
        KNURL_CLIENT_ADDRESS_FORWARDED_FOR, KNURL_CLIENT_ADDRESS_PEER,
        KNURL_CLIENT_ADDRESS_PEER,          KNURL_CLIENT_ADDRESS_RESOLVER,
        KNURL_CLIENT_ADDRESS_FORWARDED_FOR, KNURL_CLIENT_ADDRESS_REAL_IP,
        KNURL_CLIENT_ADDRESS_PEER};
    const size_t via_counts[] = {2, 0, 0, 1, 16, 1, 0};
    knurl_server_config config;
    knurl_autoblock_notifications_config notifications;
    knurl_autoblock_event_rule rule = {"path", 1};
    struct knurl_request_impl *impl;
    knurl_request *request;
    knurl_header headers[2];
    knurl_server *server;
    knurl_autoblock_event event;
    knurl_error error;
    int mode, calls = 0;
    for (mode = 0; mode < 7; ++mode) {
        knurl_server_config_init(&config);
        config.client_ip.trusted_proxies = proxies;
        config.client_ip.trusted_proxy_count = 2;
        if (mode == 4)
            config.client_ip.max_hops = 64;
        if (mode == 3) {
            config.client_ip.resolve = resolve;
            config.client_ip.userdata = &calls;
        }
        config.autoblock.enabled = 1;
        config.autoblock.max_entries = 1;
        config.autoblock.event_rules = &rule;
        config.autoblock.event_rule_count = 1;
        CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
        knurl_autoblock_notifications_config_init(&notifications);
        notifications.queue_capacity = 1;
        CHECK(server->set_autoblock_notifications(server, &notifications,
                                                  &error) == KNURL_OK);
        CHECK(knurl_autoblock_open(server, &error) == KNURL_OK);
        impl = calloc(1, sizeof(*impl));
        CHECK(impl != NULL);
        request = &impl->receiver;
        knurl_request_receiver_init(request);
        request->impl = impl;
        impl->server = server;
        impl->peer.address = ip(mode == 1 ? "192.0.2.254" : "127.0.0.1");
        impl->local.address = ip("127.0.0.1");
        headers[0].name = ip("x-forwarded-for");
        headers[0].value = ip("203.0.113.99, 198.51.100.1, 10.0.0.1");
        headers[1].name = ip("x-real-ip");
        headers[1].value = ip("2001:db8::1");
        if (mode == 2)
            headers[0].value = ip("invalid");
        if (mode == 5)
            headers[0].name = ip("x-ignored");
        if (mode == 4 || mode == 6)
            headers[0].value = ip("198.51.100.1, 10.0.0.1, 10.0.0.1, 10.0.0.1,"
                                  "10.0.0.1, 10.0.0.1, 10.0.0.1, 10.0.0.1,"
                                  "10.0.0.1, 10.0.0.1, 10.0.0.1, 10.0.0.1,"
                                  "10.0.0.1, 10.0.0.1, 10.0.0.1, 10.0.0.1,"
                                  "10.0.0.1, 10.0.0.1");
        impl->headers = headers;
        impl->header_count = 2;
        CHECK(knurl_request_identity(request, &error) == KNURL_OK);
        knurl_autoblock_note(server, impl->effective_address,
                             KNURL_OBSERVE_APPLICATION, 0, "path", request,
                             NULL);
        CHECK(server->autoblock_next(server, &event, NULL, &error) == KNURL_OK);
        CHECK(!strcmp(event.address, expected[mode]));
        CHECK(event.address_source == sources[mode]);
        CHECK(event.via_count == via_counts[mode]);
        CHECK(event.via_truncated == (mode == 4));
        if (event.via_count != 0)
            CHECK(!strcmp(event.via[event.via_count - 1], "127.0.0.1"));
        if (mode == 0 || mode == 4)
            CHECK(!strcmp(event.via[0], "10.0.0.1"));
        free(impl);
        knurl_autoblock_close(server);
        CHECK(server->destroy(server, &error) == KNURL_OK);
    }
    CHECK(calls == 1);
    return 0;
}
struct notification_fixture {
    int calls, failed;
};
static void notified(knurl_server *server, const knurl_autoblock_event *event,
                     void *data)
{
    struct notification_fixture *f = data;
    knurl_error error;
    ++f->calls;
    /* This query takes the same lock, proving callback delivery is outside it.
     */
    if (knurl_autoblock_allowed(server, ip(event->address)) ||
        event->count < event->threshold || event->duration_ms.high != 0 ||
        event->duration_ms.low != 60000 ||
        server->destroy(server, &error) != KNURL_STATE)
        f->failed = 1;
}
static int notifications(void)
{
    struct notification_fixture fixture = {0, 0};
    knurl_server_config config;
    knurl_autoblock_notifications_config notifications;
    knurl_autoblock_status_rule rules[2] = {{404, 2}, {404, 3}};
    knurl_autoblock_event_rule named;
    knurl_autoblock_event event;
    knurl_server *server;
    knurl_error error;
    knurl_u64 dropped;
    char long_name[300];
    pid_t child;
    int result, calls;
    memset(long_name, 'n', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    named.name = long_name;
    named.threshold = 1;
    knurl_server_config_init(&config);
    config.autoblock.enabled = 1;
    config.autoblock.block_seconds = 60;
    config.autoblock.max_entries = 8;
    config.autoblock.status_rules = rules;
    config.autoblock.status_rule_count = 2;
    config.autoblock.event_rules = &named;
    config.autoblock.event_rule_count = 1;
    config.autoblock.tls_failure_threshold =
        config.autoblock.tcp_stall_threshold = 1;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    knurl_autoblock_notifications_config_init(&notifications);
    CHECK(notifications.queue_capacity == 0 && notifications.callback == NULL);
    notifications.queue_capacity = 2;
    notifications.callback = notified;
    notifications.userdata = &fixture;
    CHECK(server->set_autoblock_notifications(server, &notifications, &error) ==
          KNURL_OK);
    CHECK(knurl_autoblock_open(server, &error) == KNURL_OK);
    CHECK(server->set_autoblock_notifications(server, NULL, &error) ==
          KNURL_STATE);
    knurl_autoblock_note(server, ip("192.0.2.1"), KNURL_OBSERVE_HTTP_STATUS,
                         404, NULL, NULL, NULL);
    CHECK(fixture.calls == 0);
    CHECK(server->autoblock_next(server, &event, &dropped, &error) ==
          KNURL_EOF);
    knurl_autoblock_note(server, ip("192.0.2.1"), KNURL_OBSERVE_HTTP_STATUS,
                         404, NULL, NULL, NULL);
    CHECK(fixture.calls == 1 && fixture.failed == 0);
    CHECK(server->autoblock_next(server, &event, &dropped, &error) == KNURL_OK);
    CHECK(event.change == KNURL_AUTOBLOCK_BLOCKED &&
          event.reason == KNURL_AUTOBLOCK_HTTP_STATUS);
    CHECK(event.status == 404 && event.count == 2 && event.threshold == 2 &&
          event.rule_index == 0);
    CHECK(!strcmp(event.address, "192.0.2.1") &&
          !strcmp(event.peer_address, event.address));
    CHECK(event.via_count == 0 &&
          event.address_source == KNURL_CLIENT_ADDRESS_PEER);
    CHECK(dropped.high == 0 && dropped.low == 0);
    (void)poll(NULL, 0, 2);
    knurl_autoblock_note(server, ip("192.0.2.1"), KNURL_OBSERVE_HTTP_STATUS,
                         404, NULL, NULL, NULL);
    CHECK(server->impl->autoblock->shared->entries[0].counters[3] == 3);
    CHECK(fixture.calls == 2 && fixture.failed == 0);
    CHECK(server->autoblock_next(server, &event, &dropped, &error) == KNURL_OK);
    CHECK(event.change == KNURL_AUTOBLOCK_EXTENDED && event.count == 3 &&
          dropped.low == 0);
    knurl_autoblock_note(server, ip("192.0.2.8"), KNURL_OBSERVE_TLS_FAILURE, 0,
                         NULL, NULL, NULL);
    knurl_autoblock_note(server, ip("192.0.2.2"), KNURL_OBSERVE_APPLICATION, 0,
                         long_name, NULL, NULL);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TLS_FAILURE,
                             0, NULL, NULL, NULL);
        _exit(fixture.failed);
    }
    CHECK(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
          WEXITSTATUS(result) == 0);
    CHECK(server->autoblock_next(server, &event, &dropped, &error) == KNURL_OK);
    CHECK(event.reason == KNURL_AUTOBLOCK_APPLICATION && event.name_truncated);
    CHECK(strlen(event.name) == KNURL_AUTOBLOCK_NAME_MAX &&
          !memcmp(event.name, long_name, KNURL_AUTOBLOCK_NAME_MAX));
    CHECK(dropped.high == 0 && dropped.low == 1);
    knurl_autoblock_close(server);
    CHECK(server->autoblock_next(server, &event, &dropped, &error) == KNURL_OK);
    CHECK(event.reason == KNURL_AUTOBLOCK_TLS_FAILURE &&
          !strcmp(event.address, "192.0.2.3"));
    CHECK(server->autoblock_next(server, &event, &dropped, &error) ==
              KNURL_EOF &&
          dropped.low == 0);
    CHECK(knurl_autoblock_open(server, &error) == KNURL_OK);
    CHECK(server->autoblock_next(server, &event, &dropped, &error) ==
          KNURL_EOF);
    server->impl->autoblock->shared->dropped = (uint64_t)-1;
    CHECK(server->autoblock_next(server, &event, NULL, &error) == KNURL_EOF);
    CHECK(server->autoblock_next(server, &event, &dropped, &error) ==
          KNURL_EOF);
    CHECK(dropped.high == 0xffffffffUL && dropped.low == 0xffffffffUL);
    CHECK(server->autoblock_next(server, &event, &dropped, &error) ==
          KNURL_EOF);
    CHECK(dropped.high == 0 && dropped.low == 0);
    knurl_autoblock_note(server, ip("192.0.2.4"), KNURL_OBSERVE_TCP_STALL, 0,
                         NULL, NULL, NULL);
    knurl_autoblock_note(server, ip("192.0.2.5"), KNURL_OBSERVE_TLS_FAILURE, 0,
                         NULL, NULL, NULL);
    child = fork();
    CHECK(child >= 0);
    if (child == 0)
        _exit(knurl_autoblock_lock(server->impl->autoblock) ? 0 : 1);
    CHECK(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
          WEXITSTATUS(result) == 0);
#if defined(__linux__)
    CHECK(server->autoblock_next(server, &event, &dropped, &error) ==
              KNURL_EOF &&
          dropped.low == 2);
#else
    CHECK(server->autoblock_next(server, &event, &dropped, &error) ==
              KNURL_OK &&
          dropped.low == 0);
#endif
    knurl_autoblock_close(server);
    notifications.queue_capacity = 0;
    CHECK(server->set_autoblock_notifications(server, &notifications, &error) ==
          KNURL_OK);
    CHECK(knurl_autoblock_open(server, &error) == KNURL_OK);
    calls = fixture.calls;
    knurl_autoblock_note(server, ip("192.0.2.6"), KNURL_OBSERVE_APPLICATION, 0,
                         long_name, NULL, NULL);
    CHECK(fixture.calls == calls + 1 && fixture.failed == 0);
    CHECK(server->autoblock_next(server, &event, &dropped, &error) ==
          KNURL_EOF);
    knurl_autoblock_close(server);
    notifications.queue_capacity = 1;
    notifications.callback = NULL;
    CHECK(server->set_autoblock_notifications(server, &notifications, &error) ==
          KNURL_OK);
    CHECK(knurl_autoblock_open(server, &error) == KNURL_OK);
    knurl_autoblock_note(server, ip("192.0.2.7"), KNURL_OBSERVE_APPLICATION, 0,
                         long_name, NULL, NULL);
    CHECK(fixture.calls == calls + 1);
    knurl_autoblock_close(server);
    notifications.queue_capacity = (size_t)-1;
    CHECK(server->set_autoblock_notifications(server, &notifications, &error) ==
          KNURL_OVERFLOW);
    CHECK(server->autoblock_next(server, &event, &dropped, &error) == KNURL_OK);
    CHECK(server->set_autoblock_notifications(server, NULL, &error) ==
          KNURL_OK);
    CHECK(server->autoblock_next(server, &event, &dropped, &error) ==
          KNURL_EOF);
    CHECK(server->destroy(server, &error) == KNURL_OK);
    return 0;
}

int main(void)
{
    knurl_server_config config;
    knurl_server *server;
    knurl_error error;
    knurl_autoblock_status_rule status = {404, 2};
    knurl_autoblock_event_rule event = {"named", 2};
    struct knurl_autoblock_entry *entry;
    pid_t child;
    int result;
    CHECK(notifications() == 0);
    CHECK(paths() == 0);
    knurl_server_config_init_production_webserver(&config);
    CHECK(config.autoblock.status_rule_count == 4 &&
          config.autoblock.status_rules[0].threshold == 10 &&
          config.autoblock.status_rules[2].threshold == 50 &&
          config.autoblock.tcp_stall_threshold == 20 &&
          config.limits.hard_body_bytes.low ==
              config.limits.default_body_bytes.low);
    config.autoblock.status_rules = &status;
    config.autoblock.status_rule_count = 1;
    config.autoblock.event_rules = &event;
    config.autoblock.event_rule_count = 1;
    config.autoblock.max_entries = 2;
    config.autoblock.window_seconds = 1;
    config.autoblock.tcp_stall_threshold = 2;
    config.autoblock.tls_failure_threshold = 2;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    CHECK(server->impl->autoblock == NULL);
    CHECK(knurl_autoblock_open(server, &error) == KNURL_OK);
    knurl_autoblock_note(server, ip("192.0.2.1"), KNURL_OBSERVE_HTTP_STATUS,
                         404, NULL, NULL, NULL);
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.1")));
    knurl_autoblock_note(server, ip("192.0.2.1"), KNURL_OBSERVE_HTTP_STATUS,
                         404, NULL, NULL, NULL);
    CHECK(!knurl_autoblock_allowed(server, ip("192.0.2.1")));
    knurl_autoblock_note(server, ip("192.0.2.2"), KNURL_OBSERVE_APPLICATION, 0,
                         "named", NULL, NULL);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        knurl_autoblock_note(server, ip("192.0.2.2"), KNURL_OBSERVE_APPLICATION,
                             0, "named", NULL, NULL);
        _exit(0);
    }
    CHECK(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
          WEXITSTATUS(result) == 0);
    CHECK(!knurl_autoblock_allowed(server, ip("192.0.2.2")));
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TCP_STALL, 0,
                         NULL, NULL, NULL);
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TCP_STALL, 0,
                         NULL, NULL, NULL);
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.3")));
    entry = &server->impl->autoblock->shared->entries[0];
    entry->until = entry->window = 0;
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TLS_FAILURE, 0,
                         NULL, NULL, NULL);
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.3")));
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TLS_FAILURE, 0,
                         NULL, NULL, NULL);
    CHECK(!knurl_autoblock_allowed(server, ip("192.0.2.3")));
    entry->window = 0;
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TLS_FAILURE, 0,
                         NULL, NULL, NULL);
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.3")));
    CHECK(entry->counters[1] == 1);
    server->impl->config.autoblock.tcp_stall_threshold = UINT_MAX;
    server->impl->config.autoblock.block_seconds = UINT_MAX;
    entry->counters[0] = UINT_MAX;
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TCP_STALL, 0,
                         NULL, NULL, NULL);
    CHECK(entry->counters[0] == UINT_MAX &&
          entry->until > (uint64_t)0xffffffffUL);
    child = fork();
    CHECK(child >= 0);
    if (child == 0)
        _exit(knurl_autoblock_lock(server->impl->autoblock) ? 0 : 1);
    CHECK(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
          WEXITSTATUS(result) == 0);
#if defined(__linux__)
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.3")));
#else
    CHECK(!knurl_autoblock_allowed(server, ip("192.0.2.3")));
#endif
    knurl_autoblock_close(server);
    CHECK(server->impl->autoblock == NULL);
    CHECK(knurl_autoblock_open(server, &error) == KNURL_OK);
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.2")));
    knurl_autoblock_close(server);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    return 0;
}
